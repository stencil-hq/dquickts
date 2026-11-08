// Async context propagation for AsyncLocalStorage (Node's AsyncContextFrame).
//
// The engine keeps one "async context" value per runtime and carries it along
// every asynchronous continuation: a promise reaction (then/catch/finally,
// await, for await) runs with the context current when it was registered, a
// queueMicrotask() callback with the context current when it was queued
// (JS_GetAsyncContext/JS_SetAsyncContext in quickjs.h, exposed by qbun:async).
// Host callbacks (timers, immediates, nextTick, I/O completions) are not jobs:
// the library wraps them with bind() when they are scheduled.
//
// The context is a Frame: an immutable Map from storage key to store, never
// modified once installed (a continuation that captured it keeps seeing the
// same stores); entering a store installs a new Frame.

import * as native from "qbun:async";

/** The stores of one async context: storage key → store. Never mutated once installed. */
export class Frame extends Map<object, unknown> {}

interface AsyncNative {
	getContext(): Frame | undefined;
	setContext(frame: Frame | undefined): void;
	exchangeContext(frame: Frame | undefined): Frame | undefined;
}

// qbun:async exports exactly these functions (node/async.c)
const { getContext, setContext, exchangeContext } = native as unknown as AsyncNative;

// No context has been entered yet: every context is undefined, so bind() has
// nothing to capture and returns callbacks unchanged.
let enabled = false;

/** Called before a store is entered for the first time: from now on bind() wraps callbacks. */
export function enable(): void {
	enabled = true;
}

/** The current context (undefined when none). */
export function current(): Frame | undefined {
	return getContext();
}

/** Install `frame` as the current context. */
export function set(frame: Frame | undefined): void {
	setContext(frame);
}

/** Install `frame` as the current context and return the previous one. */
export function exchange(frame: Frame | undefined): Frame | undefined {
	return exchangeContext(frame);
}

/** The store of `key` in `frame`; `missing` if `frame` holds none. */
export function lookup(frame: Frame | undefined, key: object, missing: unknown): unknown {
	if (frame === undefined || !frame.has(key)) return missing;
	return frame.get(key);
}

/** A new frame: `frame` with `key` bound to `store`. */
export function withStore(frame: Frame | undefined, key: object, store: unknown): Frame {
	const next = frame === undefined ? new Frame() : new Frame(frame);
	next.set(key, store);
	return next;
}

/** Run `fn(...args)` in the context `frame`, then restore the current one. */
export function run<A extends unknown[], R>(frame: Frame | undefined, fn: (...args: A) => R, ...args: A): R {
	const prior = exchangeContext(frame);
	try {
		return fn(...args);
	} finally {
		setContext(prior);
	}
}

/** Wrap `fn` so that it runs in the async context current at bind time. */
export function bind<F extends (...args: never[]) => unknown>(fn: F): F {
	if (!enabled) return fn;
	const frame = getContext();
	const bound = function (this: unknown, ...args: unknown[]): unknown {
		const prior = exchangeContext(frame);
		try {
			return Reflect.apply(fn, this, args);
		} finally {
			setContext(prior);
		}
	};
	// bind(fn) and fn are the same callback for code that checks arity
	if (fn.length !== 0) Object.defineProperty(bound, "length", { value: fn.length, configurable: true });
	return bound as unknown as F;
}
