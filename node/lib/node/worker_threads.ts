// node:worker_threads — Worker threads (each runs its own qbun runtime and
// event loop, see node/worker.c), message ports, BroadcastChannel and the
// environment data. Importing it starts nothing.

import { EventEmitter } from "node:events";
import * as native from "qbun:worker";
import { codedError, invalidArgType, validateNumber, validateObject } from "internal:errors";
import {
	BroadcastChannel,
	MessageChannel,
	MessagePort,
	isMarkedAsUntransferable,
	markAsUncloneable,
	markAsUntransferable,
	moveMessagePortToContext,
	postOn,
	readMessage,
	receiveMessageOnPort,
	type PortHandle,
} from "internal:worker/messaging";
import { SHARE_ENV, state } from "internal:worker/state";
import { WorkerConnection, type HeapStatistics } from "internal:worker/parent";
import { nodeEntry } from "internal:worker/entry";
import { Readable, Writable } from "node:stream";

export {
	BroadcastChannel,
	MessageChannel,
	MessagePort,
	isMarkedAsUntransferable,
	markAsUncloneable,
	markAsUntransferable,
	moveMessagePortToContext,
	receiveMessageOnPort,
	SHARE_ENV,
};

export const isMainThread = state.isMainThread;
export const isInternalThread = false;
export const threadId = state.threadId;
export const threadName = state.threadName;
export const parentPort = state.parentPort;
export const workerData = state.workerData;
export const resourceLimits = state.resourceLimits;

export function getEnvironmentData(key: unknown): unknown {
	return state.environmentData.get(key);
}

export function setEnvironmentData(key: unknown, value?: unknown): void {
	if (value === undefined) state.environmentData.delete(key);
	else state.environmentData.set(key, value);
}


export interface WorkerOptions {
	argv?: unknown[];
	env?: Record<string, string | undefined> | typeof SHARE_ENV;
	eval?: boolean;
	execArgv?: string[];
	stdin?: boolean;
	stdout?: boolean;
	stderr?: boolean;
	workerData?: unknown;
	trackUnmanagedFds?: boolean;
	transferList?: unknown[];
	resourceLimits?: Record<string, number>;
	name?: string;
}

interface EventLoopUtilization {
	idle: number;
	active: number;
	utilization: number;
}

export class Worker extends EventEmitter {
	readonly threadId: number;
	readonly threadName: string;
	readonly performance: {
		eventLoopUtilization(a?: EventLoopUtilization, b?: EventLoopUtilization): EventLoopUtilization;
	};
	#conn: WorkerConnection | null = null;
	#resourceLimits: Record<string, number>;
	#stdin: Writable | null = null;
	#stdout: Readable | null = null;
	#stderr: Readable | null = null;
	#exited = false;
	#terminating: PromiseWithResolvers<number | undefined> | null = null;

	constructor(filename: string | URL, options: WorkerOptions = {}) {
		super();
		if (options !== undefined) validateObject(options, "options");
		if (options.argv !== undefined && !Array.isArray(options.argv))
			throw invalidArgType("options.argv", "an instance of Array", options.argv);
		if (options.execArgv !== undefined && !Array.isArray(options.execArgv))
			throw invalidArgType("options.execArgv", "an instance of Array", options.execArgv);
		if (
			options.env !== undefined &&
			options.env !== null &&
			options.env !== SHARE_ENV &&
			typeof options.env !== "object"
		) {
			throw invalidArgType("options.env", "of type object or one of undefined, null, or worker.SHARE_ENV", options.env);
		}
		if (options.name !== undefined && typeof options.name !== "string")
			throw invalidArgType("options.name", "of type string", options.name);
		if (options.resourceLimits !== undefined) validateObject(options.resourceLimits, "options.resourceLimits");
		const entry = nodeEntry(filename, !!options.eval);
		this.#resourceLimits = { ...(options.resourceLimits ?? {}) };
		this.threadName = options.name ?? "WorkerThread";
		this.performance = {
			eventLoopUtilization: () => ({ idle: 0, active: 0, utilization: 0 }),
		};
		if (options.stdin) {
			this.#stdin = new Writable({
				write: (chunk: Uint8Array | string, encoding: BufferEncoding, cb: (err?: Error | null) => void): void => {
					const bytes = typeof chunk === "string" ? new TextEncoder().encode(chunk) : new Uint8Array(chunk);
					this.#conn?.sendControl({ type: "stdin", chunk: bytes });
					cb();
				},
				final: (cb: (err?: Error | null) => void): void => {
					this.#conn?.sendControl({ type: "stdin", chunk: null });
					cb();
				},
			});
		}
		const conn = new WorkerConnection(
			{
				...entry,
				argv: options.argv ?? [],
				execArgv: options.execArgv,
				workerData: options.workerData,
				env: options.env,
				name: this.threadName,
				web: false,
				stdin: !!options.stdin,
				stdout: !!options.stdout,
				stderr: !!options.stderr,
				resourceLimits: this.#resourceLimits,
				transferList: options.transferList,
				preload: [],
			},
			{
				online: () => this.emit("online"),
				message: (data) => this.emit("message", data),
				messageerror: (error) => this.emit("messageerror", error),
				error: (error) => this.emit("error", error),
				stdio: (fd, chunk) => (fd === 2 ? this.stderr : this.stdout).push(chunk),
				exit: (code) => this.#onExit(code),
			},
		);
		this.#conn = conn;
		this.threadId = conn.threadId;
	}

	#onExit(code: number): void {
		this.#exited = true;
		this.#stdout?.push(null);
		this.#stderr?.push(null);
		this.emit("exit", code);
		this.#terminating?.resolve(code);
		this.removeAllListeners("message");
		this.removeAllListeners("messageerror");
		this.removeAllListeners("online");
	}

	get resourceLimits(): Record<string, number> | undefined {
		return this.#exited ? {} : this.#resourceLimits;
	}

	get stdin(): Writable | null {
		return this.#stdin;
	}

	get stdout(): Readable {
		if (!this.#stdout) {
			this.#stdout = new Readable({ read() {} });
			if (this.#exited) this.#stdout.push(null);
		}
		return this.#stdout;
	}

	get stderr(): Readable {
		if (!this.#stderr) {
			this.#stderr = new Readable({ read() {} });
			if (this.#exited) this.#stderr.push(null);
		}
		return this.#stderr;
	}

	postMessage(value: unknown, transferList?: unknown): void {
		if (arguments.length === 0)
			throw codedError("ERR_MISSING_ARGS", 'The "value" argument must be specified', TypeError);
		this.#conn?.postMessage(value, transferList);
	}

	terminate(callback?: (err: Error | null, exitCode?: number) => void): Promise<number | undefined> {
		if (typeof callback === "function") {
			process.emitWarning(
				"Passing a callback to worker.terminate() is deprecated. It returns a Promise instead.",
				"DeprecationWarning",
				"DEP0132",
			);
			if (!this.#exited) this.once("exit", (code: number) => callback(null, code));
		}
		if (this.#exited) return Promise.resolve(undefined);
		if (this.#terminating) return this.#terminating.promise;
		this.#terminating = Promise.withResolvers<number | undefined>();
		this.#conn?.terminate();
		return this.#terminating.promise;
	}

	ref(): void {
		this.#conn?.ref();
	}

	unref(): void {
		this.#conn?.unref();
	}

	getHeapStatistics(): Promise<HeapStatistics> {
		return this.#conn
			? this.#conn.heapStatistics()
			: Promise.reject(codedError("ERR_WORKER_NOT_RUNNING", "Worker instance not running"));
	}

	getHeapSnapshot(): Promise<Readable> {
		return Promise.reject(codedError("ERR_NOT_SUPPORTED", "worker.getHeapSnapshot() is not supported by qbun"));
	}

	cpuUsage(): Promise<{ user: number; system: number }> {
		return Promise.reject(codedError("ERR_NOT_SUPPORTED", "worker.cpuUsage() is not supported by qbun"));
	}

	startCpuProfile(): Promise<never> {
		return Promise.reject(codedError("ERR_NOT_SUPPORTED", "worker.startCpuProfile() is not supported by qbun"));
	}

	startHeapProfile(): Promise<never> {
		return Promise.reject(codedError("ERR_NOT_SUPPORTED", "worker.startHeapProfile() is not supported by qbun"));
	}

	async [Symbol.asyncDispose](): Promise<void> {
		await this.terminate();
	}
}

// ---------------------------------------------------------------- postMessageToThread

interface WorkerNative {
	openBroadcast(name: string): PortHandle;
}
const nativeApi = native as unknown as WorkerNative;

interface ThreadMessage {
	source: number;
	value: unknown;
	reply: string;
}

interface ThreadReply {
	ok: boolean;
	code?: string;
	error?: unknown;
}

const threadChannel = (id: number): string => `\u0000qbun.thread.${id}`;
let nextReply = 1;

function onThreadMessage(msg: ThreadMessage): void {
	let reply: ThreadReply;
	if (process.listenerCount("workerMessage") === 0) {
		reply = { ok: false, code: "ERR_WORKER_MESSAGING_FAILED" };
	} else {
		try {
			process.emit("workerMessage", msg.value, msg.source);
			reply = { ok: true };
		} catch (error) {
			reply = { ok: false, code: "ERR_WORKER_MESSAGING_ERRORED", error };
		}
	}
	const handle = nativeApi.openBroadcast(msg.reply);
	try {
		postOn(handle, null, reply, undefined);
	} catch {
		postOn(handle, null, { ok: false, code: reply.code ?? "ERR_WORKER_MESSAGING_ERRORED" }, undefined);
	} finally {
		handle.close();
	}
}

// this thread's inbox (never keeps the loop alive)
const inbox = nativeApi.openBroadcast(threadChannel(state.threadId));
inbox.setHandler((type, value, ports, kind) => {
	if (type !== 0) return;
	const { data } = readMessage(value, ports, kind ?? 0);
	if (data !== null && typeof data === "object") onThreadMessage(data as ThreadMessage);
});
inbox.start();

function messagingError(code: string, cause?: unknown): Error {
	const messages: Record<string, string> = {
		ERR_WORKER_MESSAGING_FAILED: "Cannot find the destination thread or listener",
		ERR_WORKER_MESSAGING_ERRORED: "The destination thread threw an error while processing the message",
		ERR_WORKER_MESSAGING_TIMEOUT: "Sending a message to another thread timed out",
	};
	const err = codedError(code, messages[code] ?? code);
	if (cause !== undefined) Object.defineProperty(err, "cause", { value: cause, configurable: true, writable: true });
	return err;
}

/** Send `value` to the thread `threadId` ('workerMessage' event of its process). */
export function postMessageToThread(
	target: number,
	value: unknown,
	transferList?: unknown,
	timeout?: number,
): Promise<void> {
	if (typeof transferList === "number" && timeout === undefined) {
		timeout = transferList;
		transferList = undefined;
	}
	validateNumber(target, "threadId", 0);
	if (timeout !== undefined) validateNumber(timeout, "timeout", 0);
	if (target === state.threadId) {
		return Promise.reject(codedError("ERR_WORKER_MESSAGING_SAME_THREAD", "Cannot sent a message to the same thread"));
	}
	const replyName = `\u0000qbun.reply.${state.threadId}.${nextReply++}`;
	const reply = nativeApi.openBroadcast(replyName);
	const { promise, resolve, reject } = Promise.withResolvers<void>();
	let timer: NodeJS.Timeout | undefined;
	const finish = (): void => {
		clearTimeout(timer);
		reply.close();
	};
	reply.setHandler((type, data, ports, kind) => {
		if (type !== 0) return;
		const answer = readMessage(data, ports, kind ?? 0).data as ThreadReply;
		finish();
		if (answer.ok) resolve();
		else reject(messagingError(answer.code ?? "ERR_WORKER_MESSAGING_FAILED", answer.error));
	});
	reply.start();
	reply.ref();
	const handle = nativeApi.openBroadcast(threadChannel(target));
	let reached: number;
	try {
		const message: ThreadMessage = { source: state.threadId, value, reply: replyName };
		reached = postOn(handle, null, message, transferList);
	} catch (error) {
		finish();
		return Promise.reject(error);
	} finally {
		handle.close();
	}
	if (reached === 0) {
		finish();
		return Promise.reject(messagingError("ERR_WORKER_MESSAGING_FAILED"));
	}
	if (timeout !== undefined) {
		timer = setTimeout(() => {
			finish();
			reject(messagingError("ERR_WORKER_MESSAGING_TIMEOUT"));
		}, timeout);
	}
	return promise;
}

export default {
	Worker,
	isMainThread,
	isInternalThread,
	parentPort,
	workerData,
	threadId,
	threadName,
	resourceLimits,
	SHARE_ENV,
	MessageChannel,
	MessagePort,
	BroadcastChannel,
	getEnvironmentData,
	setEnvironmentData,
	markAsUntransferable,
	markAsUncloneable,
	isMarkedAsUntransferable,
	moveMessagePortToContext,
	receiveMessageOnPort,
	postMessageToThread,
};
