// node:async_hooks — AsyncLocalStorage and AsyncResource on the engine's async
// context (internal:async-context), like Node's AsyncContextFrame mode.
//
// The low-level async_hooks API is not tracked: createHook() validates its
// callbacks and returns an AsyncHook whose enable()/disable() register nothing
// (the hooks are never called, as in Bun); executionAsyncId() and
// triggerAsyncId() return the ids of the top-level execution (1 and 0), and
// executionAsyncResource() returns the same top-level resource object every time.

import * as ctx from "internal:async-context";
import { codedError, validateFunction, validateObject, validateString } from "internal:errors";

// Async ids: 1 is the top-level execution (its trigger is 0); resources get
// fresh ids from a counter.
let lastAsyncId = 1;

/** The ids of the current execution context; qbun does not track async resources. */
export function executionAsyncId(): number {
	return 1;
}

export function triggerAsyncId(): number {
	return 0;
}

const topLevelResource: object = {};

/** The resource of the current execution: the top-level resource object. */
export function executionAsyncResource(): object {
	return topLevelResource;
}

export const asyncWrapProviders: Readonly<Record<string, number>> = Object.freeze({
	__proto__: null,
	NONE: 0,
	DIRHANDLE: 1,
	DNSCHANNEL: 2,
	ELDHISTOGRAM: 3,
	FILEHANDLE: 4,
	FILEHANDLECLOSEREQ: 5,
	FIXEDSIZEBLOBCOPY: 6,
	FSEVENTWRAP: 7,
	FSREQCALLBACK: 8,
	FSREQPROMISE: 9,
	GETADDRINFOREQWRAP: 10,
	GETNAMEINFOREQWRAP: 11,
	HEAPSNAPSHOT: 12,
	HTTP2SESSION: 13,
	HTTP2STREAM: 14,
	HTTP2PING: 15,
	HTTP2SETTINGS: 16,
	HTTPINCOMINGMESSAGE: 17,
	HTTPCLIENTREQUEST: 18,
	JSSTREAM: 19,
	JSUDPWRAP: 20,
	MESSAGEPORT: 21,
	PIPECONNECTWRAP: 22,
	PIPESERVERWRAP: 23,
	PIPEWRAP: 24,
	PROCESSWRAP: 25,
	PROMISE: 26,
	QUERYWRAP: 27,
	SHUTDOWNWRAP: 28,
	SIGNALWRAP: 29,
	STATWATCHER: 30,
	STREAMPIPE: 31,
	TCPCONNECTWRAP: 32,
	TCPSERVERWRAP: 33,
	TCPWRAP: 34,
	TTYWRAP: 35,
	UDPSENDWRAP: 36,
	UDPWRAP: 37,
	SIGINTWATCHDOG: 38,
	WORKER: 39,
	WORKERHEAPSNAPSHOT: 40,
	WRITEWRAP: 41,
	ZLIB: 42,
	CHECKPRIMEREQUEST: 43,
	PBKDF2REQUEST: 44,
	KEYPAIRGENREQUEST: 45,
	KEYGENREQUEST: 46,
	KEYEXPORTREQUEST: 47,
	CIPHERREQUEST: 48,
	DERIVEBITSREQUEST: 49,
	HASHREQUEST: 50,
	RANDOMBYTESREQUEST: 51,
	RANDOMPRIMEREQUEST: 52,
	SCRYPTREQUEST: 53,
	SIGNREQUEST: 54,
	TLSWRAP: 55,
	VERIFYREQUEST: 56,
	INSPECTORJSBINDING: 57,
} as Record<string, number>);

export interface HookCallbacks {
	init?(asyncId: number, type: string, triggerAsyncId: number, resource: object): void;
	before?(asyncId: number): void;
	after?(asyncId: number): void;
	destroy?(asyncId: number): void;
	promiseResolve?(asyncId: number): void;
}

const hookNames = ["init", "before", "after", "destroy", "promiseResolve"] as const;

/**
 * An async hook. qbun does not track async resources: enable() and disable()
 * register nothing, the callbacks are never called.
 */
class AsyncHook {
	constructor(callbacks: HookCallbacks) {
		validateObject(callbacks, "callbacks");
		for (const name of hookNames) {
			const fn = callbacks[name];
			if (fn !== undefined && typeof fn !== "function") {
				throw codedError("ERR_ASYNC_CALLBACK", `hook.${name} must be a function`, TypeError);
			}
		}
	}

	enable(): this {
		return this;
	}

	disable(): this {
		return this;
	}
}

export function createHook(callbacks: HookCallbacks): AsyncHook {
	return new AsyncHook(callbacks);
}

export interface AsyncResourceOptions {
	triggerAsyncId?: number;
	requireManualDestroy?: boolean;
}

/** A resource whose callbacks run in the async context current at its creation. */
export class AsyncResource {
	#frame: ctx.Frame | undefined;
	#asyncId: number;
	#triggerAsyncId: number;

	constructor(type: string, opts: AsyncResourceOptions | number = {}) {
		validateString(type, "type");
		let trigger: unknown = opts;
		if (typeof opts !== "number") {
			trigger = opts.triggerAsyncId === undefined ? executionAsyncId() : opts.triggerAsyncId;
		}
		if (!Number.isSafeInteger(trigger) || (trigger as number) < -1) {
			throw codedError("ERR_INVALID_ASYNC_ID", `Invalid triggerAsyncId value: ${String(trigger)}`, RangeError);
		}
		this.#frame = ctx.current();
		this.#asyncId = ++lastAsyncId;
		this.#triggerAsyncId = trigger as number;
	}

	runInAsyncScope<This, A extends unknown[], R>(fn: (this: This, ...args: A) => R, thisArg?: This, ...args: A): R {
		const prior = ctx.exchange(this.#frame);
		try {
			return Reflect.apply(fn, thisArg, args) as R;
		} finally {
			ctx.set(prior);
		}
	}

	/** No destroy hook is ever called (see createHook()); returns the resource. */
	emitDestroy(): this {
		return this;
	}

	asyncId(): number {
		return this.#asyncId;
	}

	triggerAsyncId(): number {
		return this.#triggerAsyncId;
	}

	bind<F extends (...args: never[]) => unknown>(fn: F, thisArg?: unknown): F {
		validateFunction(fn, "fn");
		let bound: (...args: unknown[]) => unknown;
		if (thisArg === undefined) {
			const resource = this;
			bound = function (this: unknown, ...args: unknown[]): unknown {
				return resource.runInAsyncScope(fn as unknown as (...a: unknown[]) => unknown, this, ...args);
			};
		} else {
			bound = this.runInAsyncScope.bind(this, fn as unknown as (...a: unknown[]) => unknown, thisArg) as (
				...args: unknown[]
			) => unknown;
		}
		Object.defineProperty(bound, "length", {
			configurable: true,
			enumerable: false,
			value: fn.length,
			writable: false,
		});
		Object.defineProperty(bound, "asyncResource", {
			configurable: true,
			enumerable: true,
			value: this,
			writable: true,
		});
		return bound as unknown as F;
	}

	static bind<F extends (...args: never[]) => unknown>(fn: F, type?: string, thisArg?: unknown): F {
		const name = type || fn.name;
		return new AsyncResource(name || "bound-anonymous-fn").bind(fn, thisArg);
	}

	get domain(): never {
		throw codedError(
			"ERR_ASYNC_RESOURCE_DOMAIN_REMOVED",
			"The `domain` property on AsyncResource has been removed. Use AsyncLocalStorage instead.",
		);
	}
}

export interface AsyncLocalStorageOptions<T> {
	defaultValue?: T;
	name?: string;
}

/** Enters a store for the rest of the synchronous execution; dispose() restores the previous one. */
class RunScope<T> {
	#storage: AsyncLocalStorage<T>;
	#previousStore: T | undefined;
	#disposed = false;

	constructor(storage: AsyncLocalStorage<T>, store: T) {
		this.#storage = storage;
		this.#previousStore = storage.getStore();
		storage.enterWith(store);
	}

	dispose(): void {
		if (this.#disposed) return;
		this.#disposed = true;
		this.#storage.enterWith(this.#previousStore as T);
	}

	[Symbol.dispose](): void {
		this.dispose();
	}
}

/** A store that follows the asynchronous continuations of the code that enters it. */
export class AsyncLocalStorage<T = unknown> {
	// Key of this storage in the frames; disable() replaces it, which exits
	// every context holding the old one.
	#key: object = {};
	#defaultValue: T | undefined = undefined;
	#name: string | undefined = undefined;

	constructor(options: AsyncLocalStorageOptions<T> = {}) {
		validateObject(options, "options");
		this.#defaultValue = options.defaultValue;
		if (options.name !== undefined) this.#name = `${options.name}`;
	}

	get name(): string {
		return this.#name || "";
	}

	static bind<F extends (...args: never[]) => unknown>(fn: F): F {
		return AsyncResource.bind(fn);
	}

	static snapshot(): <R, A extends unknown[]>(fn: (...args: A) => R, ...args: A) => R {
		return AsyncLocalStorage.bind(<R, A extends unknown[]>(cb: (...args: A) => R, ...args: A): R => cb(...args));
	}

	disable(): void {
		this.#key = {};
	}

	getStore(): T | undefined {
		return ctx.lookup(ctx.current(), this.#key, this.#defaultValue) as T | undefined;
	}

	enterWith(store: T): void {
		ctx.enable();
		ctx.set(ctx.withStore(ctx.current(), this.#key, store));
	}

	run<R, A extends unknown[]>(store: T, callback: (...args: A) => R, ...args: A): R {
		const prior = ctx.current();
		if (prior !== undefined && prior.has(this.#key) && Object.is(prior.get(this.#key), store)) {
			return callback(...args);
		}
		ctx.enable();
		ctx.set(ctx.withStore(prior, this.#key, store));
		try {
			return callback(...args);
		} finally {
			ctx.set(prior);
		}
	}

	exit<R, A extends unknown[]>(callback: (...args: A) => R, ...args: A): R {
		return this.run(undefined as T, callback, ...args);
	}

	withScope(store: T): RunScope<T> {
		return new RunScope(this, store);
	}
}

export default {
	AsyncLocalStorage,
	createHook,
	executionAsyncId,
	triggerAsyncId,
	executionAsyncResource,
	asyncWrapProviders,
	AsyncResource,
};
