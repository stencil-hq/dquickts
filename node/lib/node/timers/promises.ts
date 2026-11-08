// node:timers/promises — promise and async-iterator timers with AbortSignal.

import { abortError } from "internal:errors";
import {
	clearImmediate,
	clearTimeout,
	setImmediate as setImmediateCb,
	setInterval as setIntervalCb,
	setTimeout as setTimeoutCb,
} from "node:timers";

interface TimerOptions {
	signal?: AbortSignal;
	ref?: boolean;
}

export function setTimeout<T = void>(delay?: number, value?: T, options: TimerOptions = {}): Promise<T> {
	const { signal, ref = true } = options;
	if (signal?.aborted) return Promise.reject(abortError(signal.reason));
	const { promise, resolve, reject } = Promise.withResolvers<T>();
	const onAbort = () => {
		clearTimeout(timer);
		reject(abortError(signal!.reason));
	};
	const timer = setTimeoutCb(() => {
		signal?.removeEventListener("abort", onAbort);
		resolve(value as T);
	}, delay);
	if (!ref) timer.unref();
	signal?.addEventListener("abort", onAbort, { once: true });
	return promise;
}

export function setImmediate<T = void>(value?: T, options: TimerOptions = {}): Promise<T> {
	const { signal, ref = true } = options;
	if (signal?.aborted) return Promise.reject(abortError(signal.reason));
	const { promise, resolve, reject } = Promise.withResolvers<T>();
	const onAbort = () => {
		clearImmediate(imm);
		reject(abortError(signal!.reason));
	};
	const imm = setImmediateCb(() => {
		signal?.removeEventListener("abort", onAbort);
		resolve(value as T);
	});
	if (!ref) imm.unref();
	signal?.addEventListener("abort", onAbort, { once: true });
	return promise;
}

export async function* setInterval<T = void>(delay?: number, value?: T, options: TimerOptions = {}): AsyncGenerator<T> {
	const { signal, ref = true } = options;
	if (signal?.aborted) throw abortError(signal.reason);
	let pending = 0;
	let wake: (() => void) | null = null;
	let aborted = false;
	const timer = setIntervalCb(() => {
		pending++;
		wake?.();
		wake = null;
	}, delay);
	if (!ref) timer.unref();
	const onAbort = () => {
		aborted = true;
		wake?.();
		wake = null;
	};
	signal?.addEventListener("abort", onAbort, { once: true });
	try {
		for (;;) {
			if (aborted) throw abortError(signal!.reason);
			if (pending === 0) {
				const { promise, resolve } = Promise.withResolvers<void>();
				wake = resolve;
				await promise;
				continue;
			}
			pending--;
			yield value as T;
		}
	} finally {
		clearTimeout(timer);
		signal?.removeEventListener("abort", onAbort);
	}
}

export const scheduler = {
	wait(delay: number, options?: TimerOptions): Promise<void> {
		return setTimeout(delay, undefined, options);
	},
	yield(): Promise<void> {
		return setImmediate();
	},
};

export default { setTimeout, setImmediate, setInterval, scheduler };
