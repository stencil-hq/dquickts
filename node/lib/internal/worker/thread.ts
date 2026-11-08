// The worker side of a worker thread: node/worker.c calls setup(port, hooks)
// after the bootstrap of the worker runtime, before the entry module runs.
// It reads the init message, fills internal:worker/state, installs the
// worker's process.env, stdio, the Web Worker globals (postMessage,
// onmessage, addEventListener...) and turns uncaught errors into 'error'
// events of the parent's Worker.

import * as core from "qbun:core";
import * as native from "qbun:worker";
import { emitExit } from "node:process";
import {
	createPort,
	kControl,
	KIND_CONTROL,
	kExtraListener,
	kKeepStarted,
	postOn,
	readMessage,
	type MessagePort,
	type PortHandle,
} from "internal:worker/messaging";
import { state } from "internal:worker/state";
import type { HeapStatistics, InitMessage } from "internal:worker/parent";
import { Readable, Writable } from "node:stream";

interface Hooks {
	onUncaughtException(error: unknown): boolean;
	onUnhandledRejection(reason: unknown, promise: Promise<unknown>): boolean;
}

interface Native {
	stopRuntime(code: number): void;
}

let parentHandle: PortHandle | null = null;
let fatalReported = false;

function postControl(value: unknown): void {
	if (parentHandle) postOn(parentHandle, null, value, undefined, true);
}

/** Report an uncaught error to the parent and end the worker (exit code 1). */
function fatal(error: unknown): void {
	if (fatalReported) return;
	fatalReported = true;
	try {
		postControl({ type: "error", error });
	} catch {
		const err = error instanceof Error ? error : null;
		try {
			postControl({
				type: "error",
				error: err ? { name: err.name, message: err.message, stack: err.stack } : `${error}`,
			});
		} catch {
			// the parent sees the exit code
		}
	}
	try {
		process.exitCode = 1;
		emitExit();
	} catch {
		// an 'exit' listener failed: the worker still ends
	}
	(native as unknown as Native).stopRuntime(1);
}

function installEnv(env: Record<string, string>): void {
	const target: Record<string, string> = Object.create(null);
	for (const key in env) target[key] = env[key];
	const proxy = new Proxy(target, {
		set(t, key, value) {
			if (typeof key === "string") t[key] = `${value}`;
			return true;
		},
		defineProperty(t, key, descriptor) {
			if (typeof key === "string" && "value" in descriptor) t[key] = `${descriptor.value}`;
			return true;
		},
		deleteProperty(t, key) {
			if (typeof key === "string") delete t[key];
			return true;
		},
	});
	(process as unknown as { env: Record<string, string> }).env = proxy;
}

function installStdio(init: InitMessage): void {
	const output = (fd: number): void => {
		const writable = new Writable({
			write(chunk: Uint8Array | string, encoding: BufferEncoding, cb: (err?: Error | null) => void): void {
				const bytes = typeof chunk === "string" ? new TextEncoder().encode(chunk) : new Uint8Array(chunk);
				postControl({ type: "stdio", fd, chunk: bytes });
				cb();
			},
		});
		Object.defineProperty(writable, "fd", { value: fd, configurable: true, enumerable: true });
		Object.defineProperty(writable, "isTTY", { value: false, configurable: true, enumerable: true });
		(process as unknown as Record<string, unknown>)[fd === 1 ? "stdout" : "stderr"] = writable;
	};
	if (init.stdout) output(1);
	if (init.stderr) output(2);
	// the worker never reads fd 0: process.stdin gets the parent's worker.stdin
	// data ({ stdin: true }) or nothing
	let stdin: Readable | null = null;
	Object.defineProperty(process, "stdin", {
		configurable: true,
		enumerable: true,
		get(): Readable {
			if (!stdin) {
				stdin = new Readable({ read() {} });
				Object.defineProperty(stdin, "fd", { value: 0, configurable: true, enumerable: true });
				Object.defineProperty(stdin, "isTTY", { value: false, configurable: true, enumerable: true });
				for (const chunk of stdinPending) stdin.push(chunk);
				stdinPending.length = 0;
				if (!init.stdin) stdin.push(null);
				stdinStream = stdin;
			}
			return stdin;
		},
		set(value: Readable) {
			stdin = value;
			stdinStream = value;
		},
	});
}

// stdin data received before process.stdin was used
const stdinPending: (Uint8Array | null)[] = [];

let stdinStream: Readable | null = null;

function heapStatistics(): HeapStatistics {
	const mem = process.memoryUsage();
	return {
		total_heap_size: mem.heapTotal,
		total_heap_size_executable: 0,
		total_physical_size: mem.heapTotal,
		total_available_size: Math.max(0, 4 * 1024 * 1024 * 1024 - mem.heapUsed),
		used_heap_size: mem.heapUsed,
		heap_size_limit: 4 * 1024 * 1024 * 1024,
		malloced_memory: mem.heapTotal,
		peak_malloced_memory: mem.heapTotal,
		does_zap_garbage: 0,
		number_of_native_contexts: 1,
		number_of_detached_contexts: 0,
		total_global_handles_size: 0,
		used_global_handles_size: 0,
		external_memory: mem.external,
	};
}

function onControl(data: unknown): void {
	if (data === null || typeof data !== "object") return;
	const msg = data as { type?: string; chunk?: Uint8Array | null; id?: number };
	switch (msg.type) {
		case "stdin":
			if (stdinStream) stdinStream.push(msg.chunk ?? null);
			else stdinPending.push(msg.chunk ?? null);
			break;
		case "heap":
			postControl({ type: "heap", id: msg.id, stats: heapStatistics() });
			break;
	}
}

type ScopeListener = Parameters<EventTarget["addEventListener"]>[1];
type ScopeOptions = Parameters<EventTarget["addEventListener"]>[2];
type Handler = ((this: unknown, event: Event) => unknown) | null;

/** self.onmessage, self.addEventListener("message"), postMessage() and close(). */
function installGlobalScope(port: MessagePort, init: InitMessage): void {
	const scope = new EventTarget();
	const listenersOf = scope as unknown as Record<symbol, (type: string) => unknown[]>;
	const listenerKey = Symbol.for("qbun.eventTargetListeners");
	const handlers: Record<string, Handler> = { message: null, messageerror: null, error: null };
	const deliver = (data: unknown, ports: MessagePort[]): void => {
		scope.dispatchEvent(new MessageEvent("message", { data, ports }));
	};
	const deliverError = (error: unknown): void => {
		scope.dispatchEvent(new MessageEvent("messageerror", { data: error }));
	};
	const sync = (): void => {
		const active = listenersOf[listenerKey]("message").length > 0;
		port[kExtraListener](active ? deliver : null, active ? deliverError : null);
	};
	const define = (name: string, value: unknown): void => {
		Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
	};
	define("addEventListener", (type: string, listener: ScopeListener, options?: ScopeOptions): void => {
		scope.addEventListener(type, listener, options);
		sync();
	});
	define("removeEventListener", (type: string, listener: ScopeListener, options?: ScopeOptions): void => {
		scope.removeEventListener(type, listener, options);
		sync();
	});
	define("dispatchEvent", (event: Event): boolean => scope.dispatchEvent(event));
	define("postMessage", (message: unknown, transfer?: unknown): void => {
		port.postMessage(message, transfer);
	});
	for (const type in handlers) {
		Object.defineProperty(globalThis, `on${type}`, {
			configurable: true,
			enumerable: true,
			get: () => handlers[type],
			set: (fn: unknown) => {
				const old = handlers[type];
				if (old) scope.removeEventListener(type, old as ScopeListener);
				handlers[type] = typeof fn === "function" ? (fn as Handler) : null;
				if (handlers[type]) scope.addEventListener(type, handlers[type] as ScopeListener);
				sync();
			},
		});
	}
	if (init.web) {
		define("name", init.name);
		define("close", (): void => {
			process.exit();
		});
	}
}

export function setup(handle: PortHandle, hooks: Hooks): boolean {
	parentHandle = handle;
	const port = createPort(handle);
	port[kControl] = onControl;
	const raw = handle.receive();
	if (!raw || !(raw[2] & KIND_CONTROL)) return false;
	const init = readMessage(raw[0], raw[1], raw[2]).data as InitMessage;
	port[kKeepStarted]();

	state.isMainThread = false;
	state.parentPort = port;
	state.workerData = init.workerData;
	state.environmentData = init.environmentData instanceof Map ? init.environmentData : new Map();
	state.resourceLimits = init.resourceLimits ?? {};
	state.threadName = init.name;
	state.web = init.web;

	const onUncaught = hooks.onUncaughtException;
	const onRejection = hooks.onUnhandledRejection;
	hooks.onUncaughtException = (error: unknown): boolean => {
		if (onUncaught(error)) return true;
		fatal(error);
		return true;
	};
	hooks.onUnhandledRejection = (reason: unknown, promise: Promise<unknown>): boolean => {
		if (onRejection(reason, promise)) return true;
		fatal(reason);
		return true;
	};

	if (init.env) installEnv(init.env);
	installStdio(init);
	installGlobalScope(port, init);

	if (init.filename !== null && core.statKind(init.filename) !== 1) {
		const err = new Error(`Cannot find module '${init.filename}'`) as Error & { code: string };
		err.code = "ERR_MODULE_NOT_FOUND";
		fatal(err);
		return false;
	}
	postControl({ type: "online" });
	const base = `${core.cwd()}/`;
	for (const spec of init.preload ?? []) {
		try {
			core.loadModuleSync(spec, base);
		} catch (error) {
			fatal(error);
			return false;
		}
	}
	return true;
}
