// The Web Worker globals: Worker (Bun's flavor: ref/unref, 'open' and
// 'close' events, argv/env/workerData/preload options), MessageChannel,
// MessagePort and BroadcastChannel. Inside the worker, self.onmessage,
// self.addEventListener("message") and postMessage() talk to this object
// (installed by internal:worker/thread).

import { CloseEvent, ErrorEvent, Event, EventTarget, MessageEvent } from "internal:event-target";
import { BroadcastChannel, MessageChannel, MessagePort } from "internal:worker/messaging";
import { WorkerConnection } from "internal:worker/parent";
import { webEntry } from "internal:worker/entry";
import type { SHARE_ENV } from "internal:worker/state";

export { BroadcastChannel, MessageChannel, MessagePort };

export interface WebWorkerOptions {
	name?: string;
	type?: "classic" | "module";
	credentials?: string;
	smol?: boolean;
	ref?: boolean;
	argv?: unknown[];
	execArgv?: string[];
	env?: Record<string, string | undefined> | typeof SHARE_ENV;
	workerData?: unknown;
	preload?: string[] | string;
}

type Handler = ((this: Worker, event: Event) => unknown) | null;
type TargetListener = Parameters<EventTarget["addEventListener"]>[1];

const HANDLER_TYPES = ["message", "messageerror", "error", "open", "close"] as const;
const listenersKey = Symbol.for("qbun.eventTargetListeners");
const kHandlers = Symbol("qbun.worker.handlers");

export class Worker extends EventTarget {
	readonly threadId: number;
	#conn: WorkerConnection;
	[kHandlers]: Record<string, Handler> = {};

	constructor(url: string | URL, options: WebWorkerOptions = {}) {
		if (arguments.length === 0)
			throw new TypeError("Failed to construct 'Worker': 1 argument required, but only 0 present.");
		super();
		const opts = options ?? {};
		const entry = webEntry(url);
		const preload =
			opts.preload === undefined
				? []
				: Array.isArray(opts.preload)
					? opts.preload.map((p) => `${p}`)
					: [`${opts.preload}`];
		this.#conn = new WorkerConnection(
			{
				...entry,
				argv: Array.isArray(opts.argv) ? opts.argv : [],
				execArgv: opts.execArgv,
				workerData: opts.workerData,
				env: opts.env,
				name: opts.name === undefined ? "" : `${opts.name}`,
				web: true,
				stdin: false,
				stdout: false,
				stderr: false,
				resourceLimits: {},
				transferList: undefined,
				preload,
			},
			{
				online: () => this.dispatchEvent(new Event("open")),
				message: (data, ports) => this.dispatchEvent(new MessageEvent("message", { data, ports })),
				messageerror: (error) => this.dispatchEvent(new MessageEvent("messageerror", { data: error })),
				error: (error) => this.#onError(error),
				stdio: () => {},
				exit: (code) => this.dispatchEvent(new CloseEvent("close", { code, reason: "", wasClean: code === 0 })),
			},
		);
		this.threadId = this.#conn.threadId;
		if (opts.ref === false) this.#conn.unref();
	}

	#onError(error: unknown): void {
		const err = error instanceof Error ? error : null;
		const event = new ErrorEvent("error", { message: err ? err.message : `${error}`, error, cancelable: true });
		const listening =
			(this as unknown as Record<symbol, (type: string) => unknown[]>)[listenersKey]("error").length > 0;
		this.dispatchEvent(event);
		// nobody handled it: shown like an uncaught error, the parent goes on
		if (!listening && !event.defaultPrevented) console.error(error);
	}

	postMessage(message: unknown, transfer?: unknown): void {
		if (arguments.length === 0)
			throw new TypeError("Failed to execute 'postMessage' on 'Worker': 1 argument required, but only 0 present.");
		this.#conn.postMessage(message, transfer);
	}

	terminate(): void {
		this.#conn.terminate();
	}

	ref(): void {
		this.#conn.ref();
	}

	unref(): void {
		this.#conn.unref();
	}

	get [Symbol.toStringTag](): string {
		return "Worker";
	}
}

for (const type of HANDLER_TYPES) {
	Object.defineProperty(Worker.prototype, `on${type}`, {
		configurable: true,
		enumerable: true,
		get(this: Worker): Handler {
			return this[kHandlers][type] ?? null;
		},
		set(this: Worker, fn: unknown) {
			const handlers = this[kHandlers];
			const old = handlers[type];
			if (old) this.removeEventListener(type, old as TargetListener);
			handlers[type] = typeof fn === "function" ? (fn as Handler) : null;
			if (handlers[type]) this.addEventListener(type, handlers[type] as TargetListener);
		},
	});
}
