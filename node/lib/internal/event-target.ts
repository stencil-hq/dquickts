// DOM events and cancellation. EventTarget has no parent tree in this runtime.
import { codedError, invalidArgType, outOfRange } from "internal:errors";
import { createTimer } from "qbun:core";

function domString(value: unknown): string {
	if (typeof value === "symbol") throw new TypeError("Cannot convert a Symbol value to a string");
	return String(value);
}
interface EventInit {
	bubbles?: boolean;
	cancelable?: boolean;
	composed?: boolean;
}
interface EventState {
	target: EventTarget | null;
	current: EventTarget | null;
	phase: number;
	dispatching: boolean;
	stopped: boolean;
	immediate: boolean;
	passive: boolean;
	canceled: boolean;
	trusted: boolean;
}
const kEventState = Symbol("event state");
function state(event: Event): EventState {
	const s = event[kEventState];
	if (!s) throw new TypeError("Illegal invocation");
	return s;
}
export class Event {
	[kEventState]: EventState;
	readonly type: string;
	readonly bubbles: boolean;
	readonly cancelable: boolean;
	readonly composed: boolean;
	readonly timeStamp: number;
	constructor(type: string, options: EventInit = {}) {
		if (arguments.length === 0) throw new TypeError("Event requires a type");
		if (options !== null && typeof options !== "object" && typeof options !== "function")
			throw new TypeError("Event options must be an object");
		this.type = domString(type);
		this.bubbles = !!options?.bubbles;
		this.cancelable = !!options?.cancelable;
		this.composed = !!options?.composed;
		this.timeStamp = globalThis.performance?.now?.() ?? Date.now();
		this[kEventState] = {
			target: null,
			current: null,
			phase: 0,
			dispatching: false,
			stopped: false,
			immediate: false,
			passive: false,
			canceled: false,
			trusted: false,
		};
		Object.defineProperty(this, kEventState, { enumerable: false });
		Object.defineProperty(this, "isTrusted", { get: () => state(this).trusted, enumerable: true });
		for (const key of ["type", "bubbles", "cancelable", "composed", "timeStamp"])
			Object.defineProperty(this, key, { writable: false, configurable: true });
	}
	get target(): EventTarget | null {
		return state(this).target;
	}
	get srcElement(): EventTarget | null {
		return this.target;
	}
	get currentTarget(): EventTarget | null {
		return state(this).current;
	}
	get eventPhase(): number {
		return state(this).phase;
	}
	get defaultPrevented(): boolean {
		return state(this).canceled;
	}
	get isTrusted(): boolean {
		return false;
	}
	get cancelBubble(): boolean {
		return state(this).stopped;
	}
	set cancelBubble(value: boolean) {
		if (value) this.stopPropagation();
	}
	get returnValue(): boolean {
		return !this.defaultPrevented;
	}
	set returnValue(value: boolean) {
		if (!value) this.preventDefault();
	}
	stopPropagation(): void {
		state(this).stopped = true;
	}
	stopImmediatePropagation(): void {
		const s = state(this);
		s.stopped = s.immediate = true;
	}
	preventDefault(): void {
		const s = state(this);
		if (this.cancelable && !s.passive) s.canceled = true;
	}
	composedPath(): EventTarget[] {
		const s = state(this);
		return s.dispatching && s.target ? [s.target] : [];
	}
	initEvent(type: string, bubbles = false, cancelable = false): void {
		const s = state(this);
		if (s.dispatching) return;
		Object.defineProperties(this, {
			type: { value: String(type), configurable: true, writable: true, enumerable: true },
			bubbles: { value: !!bubbles, configurable: true, writable: true, enumerable: true },
			cancelable: { value: !!cancelable, configurable: true, writable: true, enumerable: true },
		});
		s.target = null;
		s.canceled = s.stopped = s.immediate = false;
	}
	get [Symbol.toStringTag](): string {
		return "Event";
	}
	static readonly NONE = 0;
	static readonly CAPTURING_PHASE = 1;
	static readonly AT_TARGET = 2;
	static readonly BUBBLING_PHASE = 3;
	readonly NONE = 0;
	readonly CAPTURING_PHASE = 1;
	readonly AT_TARGET = 2;
	readonly BUBBLING_PHASE = 3;
}
export class CustomEvent<T = unknown> extends Event {
	readonly detail: T | null;
	constructor(type: string, options: EventInit & { detail?: T } = {}) {
		super(type, options);
		this.detail = options?.detail ?? null;
	}
	initCustomEvent(type: string, bubbles = false, cancelable = false, detail: T | null = null): void {
		if (state(this).dispatching) return;
		this.initEvent(type, bubbles, cancelable);
		Object.defineProperty(this, "detail", { value: detail, configurable: true, enumerable: true });
	}
	get [Symbol.toStringTag](): string {
		return "CustomEvent";
	}
}
export class MessageEvent<T = unknown> extends Event {
	readonly data: T | null;
	readonly origin: string;
	readonly lastEventId: string;
	readonly source: unknown;
	readonly ports: readonly unknown[];
	constructor(
		type: string,
		options: EventInit & { data?: T; origin?: string; lastEventId?: string; source?: unknown; ports?: unknown[] } = {},
	) {
		super(type, options);
		options ??= {};
		this.data = options.data === undefined ? null : options.data;
		this.origin = domString(options.origin ?? "");
		this.lastEventId = domString(options.lastEventId ?? "");
		this.source = options.source ?? null;
		this.ports = Object.freeze([...(options.ports ?? [])]);
	}
	initMessageEvent(
		type: string,
		bubbles = false,
		cancelable = false,
		data: T | null = null,
		origin = "",
		lastEventId = "",
		source: unknown = null,
		ports: unknown[] = [],
	): void {
		if (state(this).dispatching) return;
		this.initEvent(type, bubbles, cancelable);
		const fields: Record<string, unknown> = {
			data,
			origin: String(origin),
			lastEventId: String(lastEventId),
			source,
			ports: Object.freeze([...ports]),
		};
		for (const key in fields)
			Object.defineProperty(this, key, { value: fields[key], configurable: true, enumerable: true });
	}
	get [Symbol.toStringTag](): string {
		return "MessageEvent";
	}
}
export class ErrorEvent extends Event {
	readonly message: string;
	readonly filename: string;
	readonly lineno: number;
	readonly colno: number;
	readonly error: unknown;
	constructor(
		type: string,
		options: EventInit & { message?: string; filename?: string; lineno?: number; colno?: number; error?: unknown } = {},
	) {
		super(type, options);
		options ??= {};
		this.message = domString(options.message ?? "");
		this.filename = domString(options.filename ?? "");
		this.lineno = Number(options.lineno ?? 0) >>> 0;
		this.colno = Number(options.colno ?? 0) >>> 0;
		this.error = options.error === undefined ? null : options.error;
	}
	get [Symbol.toStringTag](): string {
		return "ErrorEvent";
	}
}
export class CloseEvent extends Event {
	readonly wasClean: boolean;
	readonly code: number;
	readonly reason: string;
	constructor(type: string, options: EventInit & { wasClean?: boolean; code?: number; reason?: string } = {}) {
		super(type, options);
		options ??= {};
		this.wasClean = !!options.wasClean;
		this.code = (Number(options.code ?? 0) >>> 0) & 65535;
		this.reason = domString(options.reason ?? "");
	}
	get [Symbol.toStringTag](): string {
		return "CloseEvent";
	}
}
export type EventListener = ((this: EventTarget, event: Event) => unknown) | { handleEvent(event: Event): unknown };
interface ListenerOptions {
	capture?: boolean;
	passive?: boolean;
	once?: boolean;
	signal?: AbortSignal;
}
interface Listener {
	callback: EventListener;
	capture: boolean;
	passive: boolean;
	once: boolean;
	removed: boolean;
	signal?: AbortSignal;
	abort?: () => void;
}
const kListeners = Symbol("event listeners");
const kDispatch = Symbol("dispatch event");
export class EventTarget {
	[kListeners] = new Map<string, Listener[]>();
	constructor() {
		Object.defineProperty(this, kListeners, { enumerable: false });
	}
	addEventListener(type: string, callback: EventListener | null, options: boolean | ListenerOptions = {}): void {
		const map = this[kListeners];
		if (!map) throw new TypeError("Illegal invocation");
		type = domString(type);
		if (callback === null || callback === undefined) return;
		if (typeof callback !== "function" && typeof callback !== "object")
			throw invalidArgType("listener", "a function or an object", callback);
		const opts = typeof options === "boolean" ? { capture: options } : (options ?? {});
		const capture = !!opts.capture,
			signal = opts.signal;
		if (signal !== undefined && !(signal instanceof AbortSignal))
			throw invalidArgType("options.signal", "an AbortSignal", signal);
		if (signal?.aborted) return;
		let list = map.get(type);
		if (!list) map.set(type, (list = []));
		if (list.some((l) => !l.removed && l.callback === callback && l.capture === capture)) return;
		const listener: Listener = {
			callback,
			capture,
			passive: !!opts.passive,
			once: !!opts.once,
			removed: false,
			signal,
		};
		if (signal) {
			listener.abort = () => this.removeEventListener(type, callback, capture);
			signalState(signal).algorithms.add(listener.abort);
		}
		list.push(listener);
	}
	removeEventListener(type: string, callback: EventListener | null, options: boolean | ListenerOptions = {}): void {
		const map = this[kListeners];
		if (!map) throw new TypeError("Illegal invocation");
		type = domString(type);
		const capture = typeof options === "boolean" ? options : !!options?.capture;
		const list = map.get(type);
		if (!list) return;
		const index = list.findIndex((l) => l.callback === callback && l.capture === capture && !l.removed);
		if (index < 0) return;
		const l = list[index];
		l.removed = true;
		list.splice(index, 1);
		if (!list.length) map.delete(type);
		if (l.signal && l.abort) signalState(l.signal).algorithms.delete(l.abort);
	}
	dispatchEvent(event: Event): boolean {
		return this[kDispatch](event, false);
	}
	[kDispatch](event: Event, trusted: boolean): boolean {
		const map = this[kListeners];
		if (!map) throw new TypeError("Illegal invocation");
		if (!(event instanceof Event)) throw invalidArgType("event", "an instance of Event", event);
		const s = state(event);
		if (s.dispatching) throw new DOMException("The event is already being dispatched", "InvalidStateError");
		s.trusted = trusted;
		s.dispatching = true;
		s.target = s.current = this;
		s.phase = Event.AT_TARGET;
		try {
			for (const capture of [true, false]) {
				if (s.stopped) break;
				const list = map.get(event.type)?.slice() ?? [];
				for (const listener of list) {
					if (listener.removed || listener.capture !== capture) continue;
					if (listener.once) this.removeEventListener(event.type, listener.callback, listener.capture);
					s.passive = listener.passive;
					try {
						if (typeof listener.callback === "function") listener.callback.call(this, event);
						else if (typeof listener.callback.handleEvent === "function") listener.callback.handleEvent(event);
					} catch (error) {
						if ("reportError" in globalThis && typeof globalThis.reportError === "function")
							globalThis.reportError(error);
						else
							queueMicrotask(() => {
								throw error;
							});
					}
					s.passive = false;
					if (s.immediate) break;
				}
			}
		} finally {
			s.dispatching = false;
			s.current = null;
			s.phase = Event.NONE;
			s.passive = s.stopped = s.immediate = false;
		}
		return !s.canceled;
	}
	[Symbol.for("qbun.eventTargetListeners")](type: string): EventListener[] {
		return (this[kListeners]?.get(String(type)) ?? []).filter((l) => !l.removed).map((l) => l.callback);
	}
	get [Symbol.toStringTag](): string {
		return "EventTarget";
	}
}
const signalToken = Symbol("AbortSignal constructor");
interface SignalState {
	aborted: boolean;
	reason: unknown;
	dependents: Set<AbortSignal>;
	sources: Set<AbortSignal>;
	algorithms: Set<() => void>;
	onabort: ((event: Event) => unknown) | null;
	handler: ((event: Event) => unknown) | null;
}
const kSignalState = Symbol("abort signal state");
function signalState(signal: AbortSignal): SignalState {
	const s = signal[kSignalState];
	if (!s) throw new TypeError("Illegal invocation");
	return s;
}
function abort(signal: AbortSignal, reason?: unknown): void {
	const s = signalState(signal);
	if (s.aborted) return;
	const abortReason = reason === undefined ? new DOMException("This operation was aborted", "AbortError") : reason;
	const pending: AbortSignal[] = [signal];
	const visit = (current: AbortSignal): void => {
		const data = signalState(current);
		data.aborted = true;
		data.reason = abortReason;
		for (const dependent of data.dependents)
			if (!dependent.aborted) {
				pending.push(dependent);
				visit(dependent);
			}
	};
	visit(signal);
	for (const current of pending) {
		const data = signalState(current);
		for (const source of data.sources) signalState(source).dependents.delete(current);
		data.sources.clear();
		data.dependents.clear();
	}
	for (const current of pending) {
		const data = signalState(current);
		for (const algorithm of data.algorithms) algorithm();
		data.algorithms.clear();
		current[kDispatch](new Event("abort"), true);
	}
}
export class AbortSignal extends EventTarget {
	[kSignalState]: SignalState;
	constructor(token?: symbol) {
		if (token !== signalToken) throw codedError("ERR_ILLEGAL_CONSTRUCTOR", "Illegal constructor", TypeError);
		super();
		Object.defineProperty(this, kSignalState, {
			value: {
				aborted: false,
				reason: undefined,
				dependents: new Set(),
				sources: new Set(),
				algorithms: new Set(),
				onabort: null,
				handler: null,
			},
			enumerable: false,
		});
	}
	get aborted(): boolean {
		return signalState(this).aborted;
	}
	get reason(): unknown {
		return signalState(this).reason;
	}
	throwIfAborted(): void {
		if (this.aborted) throw this.reason;
	}
	get onabort(): ((event: Event) => unknown) | null {
		return signalState(this).onabort;
	}
	set onabort(callback: ((event: Event) => unknown) | null) {
		const s = signalState(this);
		s.onabort = typeof callback === "function" ? callback : null;
		if (!s.onabort && s.handler) {
			this.removeEventListener("abort", s.handler);
			s.handler = null;
		} else if (s.onabort && !s.handler) {
			s.handler = (event) => s.onabort?.call(this, event);
			this.addEventListener("abort", s.handler);
		}
	}
	static abort(reason?: unknown): AbortSignal {
		const signal = new AbortSignal(signalToken);
		abort(signal, reason);
		return signal;
	}
	static timeout(milliseconds: number): AbortSignal {
		if (typeof milliseconds !== "number") throw invalidArgType("delay", "of type number", milliseconds);
		if (!Number.isInteger(milliseconds) || milliseconds < 0 || milliseconds > 4294967295)
			throw outOfRange("delay", ">= 0 && <= 4294967295", milliseconds);
		const signal = new AbortSignal(signalToken);
		const timer = createTimer(() => {
			timer.close();
			abort(signal, new DOMException("The operation was aborted due to timeout", "TimeoutError"));
		}) as { start(ms: number, repeat: number): void; unref(): void; close(): void };
		timer.unref();
		timer.start(milliseconds, 0);
		return signal;
	}
	static any(input: Iterable<AbortSignal>): AbortSignal {
		if (input === null || input === undefined || typeof input[Symbol.iterator] !== "function")
			throw invalidArgType("signals", "an iterable of AbortSignal", input);
		const sources = Array.from(input);
		for (const source of sources)
			if (!(source instanceof AbortSignal)) throw invalidArgType("signals", "an iterable of AbortSignal", source);
		const result = new AbortSignal(signalToken);
		for (const source of sources)
			if (source.aborted) {
				abort(result, source.reason);
				return result;
			}
		for (const source of sources) {
			signalState(source).dependents.add(result);
			signalState(result).sources.add(source);
		}
		return result;
	}
	get [Symbol.toStringTag](): string {
		return "AbortSignal";
	}
}
export class AbortController {
	#signal = new AbortSignal(signalToken);
	get signal(): AbortSignal {
		return this.#signal;
	}
	abort(reason?: unknown): void {
		abort(this.#signal, reason);
	}
	get [Symbol.toStringTag](): string {
		return "AbortController";
	}
}
