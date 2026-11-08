// The parent side of a worker thread, shared by node:worker_threads Worker
// and the Web Worker global: starts the thread, sends the init message and
// routes the internal channel (user messages, control messages, exit).
//
// Control messages (KIND_CONTROL) on the worker channel:
//   parent → worker: { type: "init", ... } (first), { type: "stdin", chunk },
//                    { type: "heap", id }
//   worker → parent: { type: "online" }, { type: "error", error },
//                    { type: "stdio", fd, chunk }, { type: "heap", id, stats }

import * as native from "qbun:worker";
import process from "node:process";
import {
	createPort,
	kControl,
	kDeliver,
	kDrain,
	postOn,
	type MessagePort,
	type PortHandle,
} from "internal:worker/messaging";
import { SHARE_ENV, state } from "internal:worker/state";
import { codedError } from "internal:errors";

interface WorkerHandle {
	start(): void;
	terminate(): void;
	ref(): void;
	unref(): void;
	hasRef(): boolean;
}

interface WorkerNative {
	createWorker(
		options: { filename?: string; evalSource?: string; evalTs?: boolean; argv: string[]; execArgv: string[] },
		onexit: (code: number) => void,
	): [WorkerHandle, PortHandle, number];
}

export interface InitMessage {
	type: "init";
	filename: string | null;
	workerData: unknown;
	environmentData: Map<unknown, unknown>;
	env: Record<string, string> | null;
	name: string;
	web: boolean;
	stdin: boolean;
	stdout: boolean;
	stderr: boolean;
	resourceLimits: Record<string, number>;
	preload: string[];
}

export interface SpawnRequest {
	/** absolute path of the entry module */
	filename?: string;
	/** code of an eval worker */
	evalSource?: string;
	evalTs?: boolean;
	/** process.argv[1] in the worker */
	displayName: string;
	argv: unknown[];
	execArgv?: string[];
	workerData: unknown;
	env: unknown;
	name: string;
	web: boolean;
	stdin: boolean;
	stdout: boolean;
	stderr: boolean;
	resourceLimits: Record<string, number>;
	transferList: unknown;
	preload: string[];
}

export interface ConnectionEvents {
	online(): void;
	message(data: unknown, ports: MessagePort[]): void;
	messageerror(error: unknown): void;
	error(error: unknown): void;
	stdio(fd: number, chunk: Uint8Array): void;
	exit(code: number): void;
}

export interface HeapStatistics {
	total_heap_size: number;
	total_heap_size_executable: number;
	total_physical_size: number;
	total_available_size: number;
	used_heap_size: number;
	heap_size_limit: number;
	malloced_memory: number;
	peak_malloced_memory: number;
	does_zap_garbage: number;
	number_of_native_contexts: number;
	number_of_detached_contexts: number;
	total_global_handles_size: number;
	used_global_handles_size: number;
	external_memory: number;
}

export class WorkerConnection {
	readonly threadId: number;
	readonly port: MessagePort;
	exitCode: number | undefined = undefined;
	#handle: WorkerHandle;
	#portHandle: PortHandle;
	#events: ConnectionEvents;
	#heap = new Map<number, PromiseWithResolvers<HeapStatistics>>();
	#nextHeapId = 1;

	constructor(req: SpawnRequest, events: ConnectionEvents) {
		this.#events = events;
		const argv = [process.execPath, req.displayName, ...req.argv.map((a) => `${a}`)];
		const [handle, portHandle, threadId] = (native as unknown as WorkerNative).createWorker(
			{
				filename: req.filename,
				evalSource: req.evalSource,
				evalTs: req.evalTs ?? false,
				argv,
				execArgv: (req.execArgv ?? process.execArgv).map((a) => `${a}`),
			},
			(code) => this.#onExit(code),
		);
		this.#handle = handle;
		this.#portHandle = portHandle;
		this.threadId = threadId;
		this.port = createPort(portHandle);
		this.port[kControl] = (data) => this.#onControl(data);
		this.port[kDeliver] = (data, ports) => events.message(data, ports);
		this.port.on("messageerror", (err: unknown) => events.messageerror(err));
		// always delivering, never keeping the loop alive: the handle does
		portHandle.start();

		let env: Record<string, string> | null = null;
		if (req.env !== SHARE_ENV) {
			env = {};
			const source = (req.env === undefined || req.env === null ? process.env : req.env) as Record<string, unknown>;
			for (const key in source) {
				const value = source[key];
				if (value !== undefined) env[key] = `${value}`;
			}
		}
		const init: InitMessage = {
			type: "init",
			filename: req.filename ?? null,
			workerData: req.workerData,
			environmentData: new Map(state.environmentData),
			env,
			name: req.name,
			web: req.web,
			stdin: req.stdin,
			stdout: req.stdout,
			stderr: req.stderr,
			resourceLimits: req.resourceLimits,
			preload: req.preload,
		};
		try {
			postOn(portHandle, null, init, req.transferList, true);
			handle.start();
		} catch (err) {
			portHandle.close();
			throw err;
		}
	}

	get running(): boolean {
		return this.exitCode === undefined;
	}

	#onControl(data: unknown): void {
		if (data === null || typeof data !== "object") return;
		const msg = data as {
			type?: string;
			error?: unknown;
			fd?: number;
			chunk?: Uint8Array;
			id?: number;
			stats?: HeapStatistics;
		};
		switch (msg.type) {
			case "online":
				this.#events.online();
				break;
			case "error":
				this.#events.error(msg.error);
				break;
			case "stdio":
				if (msg.chunk instanceof Uint8Array) this.#events.stdio(msg.fd ?? 1, msg.chunk);
				break;
			case "heap": {
				const pending = this.#heap.get(msg.id ?? 0);
				if (pending && msg.stats) {
					this.#heap.delete(msg.id ?? 0);
					pending.resolve(msg.stats);
				}
				break;
			}
		}
	}

	#onExit(code: number): void {
		// the messages sent before the exit come first
		this.port[kDrain]();
		this.exitCode = code;
		this.port[kDeliver] = null;
		this.port.close();
		for (const pending of this.#heap.values())
			pending.reject(codedError("ERR_WORKER_NOT_RUNNING", "Worker instance not running"));
		this.#heap.clear();
		this.#events.exit(code);
	}

	postMessage(value: unknown, transfer?: unknown): void {
		if (!this.running) return;
		postOn(this.#portHandle, null, value, transfer);
	}

	sendControl(value: unknown): void {
		if (!this.running) return;
		postOn(this.#portHandle, null, value, undefined, true);
	}

	heapStatistics(): Promise<HeapStatistics> {
		if (!this.running) return Promise.reject(codedError("ERR_WORKER_NOT_RUNNING", "Worker instance not running"));
		const id = this.#nextHeapId++;
		const pending = Promise.withResolvers<HeapStatistics>();
		this.#heap.set(id, pending);
		this.sendControl({ type: "heap", id });
		return pending.promise;
	}

	terminate(): void {
		this.#handle.terminate();
	}

	ref(): void {
		this.#handle.ref();
	}

	unref(): void {
		this.#handle.unref();
	}

	hasRef(): boolean {
		return this.#handle.hasRef();
	}
}
