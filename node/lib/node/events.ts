// node:events — EventEmitter and its helpers (once, on, getEventListeners...).

import { AsyncResource } from "node:async_hooks";
import { abortError, codedError, invalidArgType, validateFunction } from "internal:errors";

type Listener = (...args: unknown[]) => unknown;
type Wrapped = Listener & { listener?: Listener };
type EventKey = string | symbol;
type EventMap = Record<EventKey, Wrapped | Wrapped[] | undefined>;

const kCapture = Symbol("kCapture");
export const errorMonitor: unique symbol = Symbol("events.errorMonitor");
export const captureRejectionSymbol: unique symbol = Symbol.for("nodejs.rejection");
const kMaxEventTargetListeners = Symbol("events.maxEventTargetListeners");

let defaultMaxListeners = 10;

function warnLeak(emitter: EventEmitter, type: EventKey, count: number): void {
	const name = emitter.constructor?.name ?? "EventEmitter";
	const msg =
		`Possible EventEmitter memory leak detected. ${count} ${String(type)} listeners added to [${name}]. ` +
		"MaxListeners is " +
		emitter.getMaxListeners() +
		". Use emitter.setMaxListeners() to increase limit";
	const process = globalThis.process as unknown as { emitWarning?: (w: Error) => void } | undefined;
	const w = Object.assign(new Error(msg), { name: "MaxListenersExceededWarning", emitter, type, count });
	if (process?.emitWarning) process.emitWarning(w);
}

export interface EventEmitterOptions {
	captureRejections?: boolean;
}

export class EventEmitter {
	_events!: EventMap;
	_eventsCount!: number;
	_maxListeners!: number | undefined;
	[kCapture]!: boolean;

	static EventEmitter: typeof EventEmitter;
	static errorMonitor = errorMonitor;
	static captureRejectionSymbol = captureRejectionSymbol;
	static captureRejections = false;
	static usingDomains = false;

	static get defaultMaxListeners(): number {
		return defaultMaxListeners;
	}
	static set defaultMaxListeners(n: number) {
		if (typeof n !== "number" || n < 0 || Number.isNaN(n)) {
			throw codedError(
				"ERR_OUT_OF_RANGE",
				`The value of "defaultMaxListeners" is out of range. It must be a non-negative number. Received ${n}`,
				RangeError,
			);
		}
		defaultMaxListeners = n;
	}

	constructor(opts?: EventEmitterOptions) {
		EventEmitter.init.call(this, opts);
	}

	static init(this: EventEmitter, opts?: EventEmitterOptions): void {
		if (this._events === undefined || this._events === Object.getPrototypeOf(this)._events) {
			this._events = Object.create(null);
			this._eventsCount = 0;
		}
		this._maxListeners = this._maxListeners || undefined;
		this[kCapture] = opts?.captureRejections ?? EventEmitter.captureRejections;
	}

	setMaxListeners(n: number): this {
		if (typeof n !== "number" || n < 0 || Number.isNaN(n)) {
			throw codedError(
				"ERR_OUT_OF_RANGE",
				`The value of "n" is out of range. It must be a non-negative number. Received ${n}`,
				RangeError,
			);
		}
		this._maxListeners = n;
		return this;
	}

	getMaxListeners(): number {
		return this._maxListeners === undefined ? defaultMaxListeners : this._maxListeners;
	}

	emit(type: EventKey, ...args: unknown[]): boolean {
		const events = this._events;
		if (type === "error" && events !== undefined && events[errorMonitor] !== undefined) {
			this.emit(errorMonitor, ...args);
		}
		const handler = events?.[type];
		if (handler === undefined) {
			if (type === "error") {
				const er = args[0];
				if (er instanceof Error) throw er;
				const err = codedError("ERR_UNHANDLED_ERROR", `Unhandled error. (${safeString(er)})`);
				(err as Error & { context?: unknown }).context = er;
				throw err;
			}
			return false;
		}
		if (typeof handler === "function") {
			const r = handler.apply(this, args);
			if (r !== undefined && r !== null && this[kCapture]) addCatch(this, r, type, args);
		} else {
			const listeners = handler.slice();
			for (let i = 0; i < listeners.length; ++i) {
				const r = listeners[i].apply(this, args);
				if (r !== undefined && r !== null && this[kCapture]) addCatch(this, r, type, args);
			}
		}
		return true;
	}

	addListener(type: EventKey, listener: Listener): this {
		return addListenerImpl(this, type, listener, false);
	}
	on(type: EventKey, listener: Listener): this {
		return addListenerImpl(this, type, listener, false);
	}
	prependListener(type: EventKey, listener: Listener): this {
		return addListenerImpl(this, type, listener, true);
	}
	once(type: EventKey, listener: Listener): this {
		validateFunction(listener, "listener");
		this.on(type, onceWrapper(this, type, listener));
		return this;
	}
	prependOnceListener(type: EventKey, listener: Listener): this {
		validateFunction(listener, "listener");
		this.prependListener(type, onceWrapper(this, type, listener));
		return this;
	}

	removeListener(type: EventKey, listener: Listener): this {
		validateFunction(listener, "listener");
		const events = this._events;
		if (events === undefined) return this;
		const list = events[type];
		if (list === undefined) return this;
		if (list === listener || list.listener === listener) {
			this._eventsCount -= 1;
			if (this._eventsCount === 0) this._events = Object.create(null);
			else delete events[type];
			if (events.removeListener !== undefined) this.emit("removeListener", type, list.listener || listener);
		} else if (typeof list !== "function") {
			let position = -1;
			for (let i = list.length - 1; i >= 0; i--) {
				if (list[i] === listener || list[i].listener === listener) {
					position = i;
					break;
				}
			}
			if (position < 0) return this;
			const removed = list[position];
			list.splice(position, 1);
			if (list.length === 1) events[type] = list[0];
			if (events.removeListener !== undefined) this.emit("removeListener", type, removed.listener || listener);
		}
		return this;
	}
	off(type: EventKey, listener: Listener): this {
		return this.removeListener(type, listener);
	}

	removeAllListeners(type?: EventKey): this {
		const events = this._events;
		if (events === undefined) return this;
		if (events.removeListener === undefined) {
			if (arguments.length === 0) {
				this._events = Object.create(null);
				this._eventsCount = 0;
			} else if (events[type!] !== undefined) {
				if (--this._eventsCount === 0) this._events = Object.create(null);
				else delete events[type!];
			}
			return this;
		}
		if (arguments.length === 0) {
			for (const key of Reflect.ownKeys(events)) {
				if (key === "removeListener") continue;
				this.removeAllListeners(key);
			}
			this.removeAllListeners("removeListener");
			this._events = Object.create(null);
			this._eventsCount = 0;
			return this;
		}
		const listeners = events[type!];
		if (typeof listeners === "function") {
			this.removeListener(type!, listeners);
		} else if (listeners !== undefined) {
			for (let i = listeners.length - 1; i >= 0; i--) this.removeListener(type!, listeners[i]);
		}
		return this;
	}

	listeners(type: EventKey): Listener[] {
		return listenersImpl(this, type, true);
	}
	rawListeners(type: EventKey): Listener[] {
		return listenersImpl(this, type, false);
	}
	listenerCount(type: EventKey, listener?: Listener): number {
		const list = this._events?.[type];
		if (list === undefined) return 0;
		if (typeof list === "function")
			return listener === undefined || list === listener || list.listener === listener ? 1 : 0;
		if (listener === undefined) return list.length;
		let n = 0;
		for (const l of list) if (l === listener || l.listener === listener) n++;
		return n;
	}
	eventNames(): EventKey[] {
		return this._eventsCount > 0 ? Reflect.ownKeys(this._events) : [];
	}

	static listenerCount(emitter: EventEmitter, type: EventKey): number {
		return emitter.listenerCount(type);
	}
	static once = once;
	static on = on;
	static getEventListeners = getEventListeners;
	static getMaxListeners = getMaxListeners;
	static setMaxListeners = setMaxListeners;
	static addAbortListener = addAbortListener;
}

function safeString(v: unknown): string {
	try {
		return String(v);
	} catch {
		return "[object]";
	}
}

function addCatch(emitter: EventEmitter, r: unknown, type: EventKey, args: unknown[]): void {
	const then = (r as { then?: unknown }).then;
	if (typeof then !== "function") return;
	then.call(r, undefined, (err: unknown) => {
		queueMicrotask(() => {
			const handler = (emitter as unknown as Record<symbol, unknown>)[captureRejectionSymbol];
			if (typeof handler === "function") handler.call(emitter, err, type, ...args);
			else emitter.emit("error", err);
		});
	});
}

function addListenerImpl(target: EventEmitter, type: EventKey, listener: Listener, prepend: boolean): EventEmitter {
	validateFunction(listener, "listener");
	let events = target._events;
	if (events === undefined) {
		events = target._events = Object.create(null);
		target._eventsCount = 0;
	} else if (events.newListener !== undefined) {
		target.emit("newListener", type, (listener as Wrapped).listener ?? listener);
		events = target._events;
	}
	const existing = events[type];
	if (existing === undefined) {
		events[type] = listener;
		++target._eventsCount;
	} else {
		let list: Wrapped[];
		if (typeof existing === "function") {
			list = events[type] = prepend ? [listener, existing] : [existing, listener];
		} else {
			list = existing;
			if (prepend) list.unshift(listener);
			else list.push(listener);
		}
		const m = target.getMaxListeners();
		if (m > 0 && list.length > m && !(list as unknown as { warned?: boolean }).warned) {
			(list as unknown as { warned?: boolean }).warned = true;
			warnLeak(target, type, list.length);
		}
	}
	return target;
}

function onceWrapper(target: EventEmitter, type: EventKey, listener: Listener): Wrapped {
	const state = { fired: false, wrapFn: undefined as unknown as Wrapped };
	const wrapped: Wrapped = function (this: unknown, ...args: unknown[]) {
		if (!state.fired) {
			target.removeListener(type, state.wrapFn);
			state.fired = true;
			return listener.apply(target, args);
		}
	};
	wrapped.listener = listener;
	state.wrapFn = wrapped;
	return wrapped;
}

function listenersImpl(target: EventEmitter, type: EventKey, unwrap: boolean): Listener[] {
	const l = target._events?.[type];
	if (l === undefined) return [];
	if (typeof l === "function") return unwrap ? [l.listener || l] : [l];
	return unwrap ? l.map((x) => x.listener || x) : l.slice();
}

interface EventTargetLike {
	addEventListener(type: string, listener: Listener, options?: unknown): void;
	removeEventListener(type: string, listener: Listener, options?: unknown): void;
}

function isEventTarget(x: unknown): x is EventTargetLike {
	return typeof (x as EventTargetLike)?.addEventListener === "function";
}

export interface StaticEventEmitterOptions {
	signal?: AbortSignal;
}

/** Resolve with the arguments of the next `name` event (reject on 'error'). */
export function once(
	emitter: EventEmitter | EventTargetLike,
	name: EventKey,
	options: StaticEventEmitterOptions = {},
): Promise<unknown[]> {
	const signal = options?.signal;
	if (signal?.aborted) return Promise.reject(abortError(signal.reason));
	return new Promise((resolve, reject) => {
		const cleanup = () => {
			if (signal) signal.removeEventListener("abort", onAbort);
		};
		const onAbort = () => {
			if (isEventTarget(emitter)) emitter.removeEventListener(name as string, resolver);
			else {
				emitter.removeListener(name, resolver);
				emitter.removeListener("error", errorListener);
			}
			reject(abortError(signal!.reason));
		};
		const errorListener = (err: unknown) => {
			(emitter as EventEmitter).removeListener(name, resolver);
			cleanup();
			reject(err);
		};
		const resolver = (...args: unknown[]) => {
			if (!isEventTarget(emitter) && name !== "error") emitter.removeListener("error", errorListener);
			cleanup();
			resolve(args);
		};
		if (isEventTarget(emitter)) {
			emitter.addEventListener(name as string, resolver, { once: true });
		} else {
			emitter.once(name, resolver);
			if (name !== "error") emitter.once("error", errorListener);
		}
		if (signal) signal.addEventListener("abort", onAbort, { once: true });
	});
}

/** Async iterator over the arguments of each `name` event. */
export function on(
	emitter: EventEmitter | EventTargetLike,
	event: EventKey,
	options: StaticEventEmitterOptions & { close?: string[]; highWaterMark?: number } = {},
): AsyncIterableIterator<unknown[]> {
	const signal = options?.signal;
	if (signal?.aborted) throw abortError(signal.reason);
	const unconsumed: unknown[][] = [];
	const pending: { resolve: (v: IteratorResult<unknown[]>) => void; reject: (e: unknown) => void }[] = [];
	let error: unknown = null;
	let finished = false;

	const eventHandler = (...args: unknown[]) => {
		const p = pending.shift();
		if (p) p.resolve({ value: args, done: false });
		else unconsumed.push(args);
	};
	const errorHandler = (err: unknown) => {
		finished = true;
		const p = pending.shift();
		if (p) p.reject(err);
		else error = err;
		iterator.return();
	};
	const closeHandler = () => {
		finished = true;
		for (const p of pending.splice(0)) p.resolve({ value: undefined as unknown as unknown[], done: true });
	};
	const onAbort = () => errorHandler(abortError(signal!.reason));
	if (isEventTarget(emitter)) emitter.addEventListener(event as string, eventHandler);
	else {
		emitter.on(event, eventHandler);
		if (event !== "error") emitter.on("error", errorHandler);
		for (const c of options.close ?? []) emitter.on(c, closeHandler);
	}
	signal?.addEventListener("abort", onAbort, { once: true });

	const removeAll = () => {
		if (isEventTarget(emitter)) emitter.removeEventListener(event as string, eventHandler);
		else {
			emitter.removeListener(event, eventHandler);
			emitter.removeListener("error", errorHandler);
			for (const c of options.close ?? []) emitter.removeListener(c, closeHandler);
		}
		signal?.removeEventListener("abort", onAbort);
	};

	const iterator: AsyncIterableIterator<unknown[]> = {
		next(): Promise<IteratorResult<unknown[]>> {
			const value = unconsumed.shift();
			if (value) return Promise.resolve({ value, done: false });
			if (error) {
				const p = Promise.reject(error);
				error = null;
				return p;
			}
			if (finished) return Promise.resolve({ value: undefined as unknown as unknown[], done: true });
			return new Promise((resolve, reject) => pending.push({ resolve, reject }));
		},
		return(): Promise<IteratorResult<unknown[]>> {
			removeAll();
			finished = true;
			for (const p of pending.splice(0)) p.resolve({ value: undefined as unknown as unknown[], done: true });
			return Promise.resolve({ value: undefined as unknown as unknown[], done: true });
		},
		throw(err: unknown): Promise<IteratorResult<unknown[]>> {
			error = err;
			removeAll();
			return Promise.reject(err);
		},
		[Symbol.asyncIterator]() {
			return this;
		},
	};
	return iterator;
}

export function getEventListeners(emitter: EventEmitter | EventTargetLike, name: EventKey): Listener[] {
	if (typeof (emitter as EventEmitter).listeners === "function") return (emitter as EventEmitter).listeners(name);
	const fn = (emitter as unknown as { [k: symbol]: (n: EventKey) => Listener[] })[
		Symbol.for("qbun.eventTargetListeners")
	];
	return typeof fn === "function" ? fn.call(emitter, name) : [];
}

export function getMaxListeners(emitter: EventEmitter | EventTargetLike): number {
	if (typeof (emitter as EventEmitter).getMaxListeners === "function")
		return (emitter as EventEmitter).getMaxListeners();
	return (emitter as unknown as Record<symbol, number>)[kMaxEventTargetListeners] ?? defaultMaxListeners;
}

export function setMaxListeners(n = defaultMaxListeners, ...targets: (EventEmitter | EventTargetLike)[]): void {
	if (targets.length === 0) {
		defaultMaxListeners = n;
		return;
	}
	for (const t of targets) {
		if (typeof (t as EventEmitter).setMaxListeners === "function") (t as EventEmitter).setMaxListeners(n);
		else (t as unknown as Record<symbol, number>)[kMaxEventTargetListeners] = n;
	}
}

export function addAbortListener(signal: AbortSignal, listener: (e: Event) => void): Disposable {
	if (signal === undefined) throw invalidArgType("signal", "an instance of AbortSignal", signal);
	validateFunction(listener, "listener");
	if (signal.aborted) {
		queueMicrotask(() => listener(new Event("abort")));
		return { [Symbol.dispose]() {} };
	}
	signal.addEventListener("abort", listener, { once: true });
	return {
		[Symbol.dispose]() {
			signal.removeEventListener("abort", listener);
		},
	};
}

interface AsyncResourceLike {
	asyncId(): number;
	triggerAsyncId(): number;
	emitDestroy(): void;
	runInAsyncScope<R>(fn: (...args: unknown[]) => R, thisArg?: unknown, ...args: unknown[]): R;
}

/** EventEmitter whose listeners run in the async context of its creation. */
export class EventEmitterAsyncResource extends EventEmitter {
	#resource: AsyncResourceLike;

	constructor(
		options?: EventEmitterOptions & { name?: string; triggerAsyncId?: number; requireManualDestroy?: boolean },
	) {
		super(options);
		this.#resource = new AsyncResource(options?.name ?? new.target.name, options);
	}

	emit(type: EventKey, ...args: unknown[]): boolean {
		return this.#resource.runInAsyncScope(super.emit, this, type, ...args);
	}

	get asyncResource(): AsyncResourceLike {
		return this.#resource;
	}
	get asyncId(): number {
		return this.#resource.asyncId();
	}
	get triggerAsyncId(): number {
		return this.#resource.triggerAsyncId();
	}
	emitDestroy(): void {
		this.#resource.emitDestroy();
	}
}

EventEmitter.EventEmitter = EventEmitter;
(EventEmitter as unknown as Record<string, unknown>).EventEmitterAsyncResource = EventEmitterAsyncResource;
EventEmitter.prototype._events = undefined as unknown as EventMap;
EventEmitter.prototype._eventsCount = 0;
EventEmitter.prototype._maxListeners = undefined;

export const usingDomains = false;
export { defaultMaxListeners };
export default EventEmitter;
