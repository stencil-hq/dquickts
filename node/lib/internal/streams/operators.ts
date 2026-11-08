// Port of Node's lib/internal/streams/operators.js (attached to Readable.prototype by node:stream).
// `this` is a Readable (or any async-iterable stream); types are erased at load.
import {
	AbortError,
	ERR_MISSING_ARGS,
	ERR_OUT_OF_RANGE,
	validateAbortSignal,
	validateFunction,
	validateInteger,
	validateObject,
} from "internal:streams/shared";
import * as destroyImpl from "internal:streams/destroy";
import { finished } from "internal:streams/end-of-stream";

type StreamThis = AsyncIterable<unknown> & {
	once(ev: string, fn: () => void): unknown;
	destroy(err?: unknown): unknown;
};
interface OperatorOptions {
	signal?: AbortSignal;
	concurrency?: number;
	highWaterMark?: number;
}
type SignalOpt = { signal: AbortSignal };
type MapFn = (value: unknown, opts: SignalOpt) => unknown;

const kEmpty = Symbol("kEmpty");
const kEof = Symbol("kEof");

function validateOptionsAndSignal(options: OperatorOptions | null | undefined): void {
	if (options != null) {
		validateObject(options, "options");
	}
	if (options?.signal != null) {
		validateAbortSignal(options.signal, "options.signal");
	}
}

function map(this: StreamThis, fn: MapFn, options?: OperatorOptions): AsyncGenerator<unknown> {
	validateFunction(fn, "fn");
	validateOptionsAndSignal(options);

	let concurrency = 1;
	if (options?.concurrency != null) {
		concurrency = Math.floor(options.concurrency);
	}

	let highWaterMark = concurrency - 1;
	if (options?.highWaterMark != null) {
		highWaterMark = Math.floor(options.highWaterMark);
	}

	validateInteger(concurrency, "options.concurrency", 1);
	validateInteger(highWaterMark, "options.highWaterMark", 0);

	highWaterMark += concurrency;

	return async function* map(this: StreamThis): AsyncGenerator<unknown> {
		const signal = AbortSignal.any([options?.signal].filter(Boolean) as AbortSignal[]);
		const stream = this;
		const queue: (Promise<unknown> | typeof kEof)[] = [];
		const signalOpt = { signal };

		let next: (() => void) | null | undefined;
		let resume: (() => void) | null | undefined;
		let done = false;
		let cnt = 0;

		function onCatch() {
			done = true;
			afterItemProcessed();
		}

		function afterItemProcessed() {
			cnt -= 1;
			maybeResume();
		}

		function maybeResume() {
			if (resume && !done && cnt < concurrency && queue.length < highWaterMark) {
				resume();
				resume = null;
			}
		}

		async function pump() {
			try {
				for await (const item of stream) {
					if (done) {
						return;
					}

					if (signal.aborted) {
						throw new AbortError();
					}

					let val: Promise<unknown>;
					try {
						const res = fn(item, signalOpt);
						if (res === kEmpty) {
							continue;
						}
						val = Promise.resolve(res);
					} catch (err) {
						val = Promise.reject(err);
					}

					cnt += 1;

					val.then(afterItemProcessed, onCatch);

					queue.push(val);
					if (next) {
						next();
						next = null;
					}

					if (!done && (queue.length >= highWaterMark || cnt >= concurrency)) {
						const { promise, resolve } = Promise.withResolvers<void>();
						resume = resolve;
						await promise;
					}
				}
				queue.push(kEof);
			} catch (err) {
				const val = Promise.reject(err);
				val.then(afterItemProcessed, onCatch);
				queue.push(val);
			} finally {
				done = true;
				if (next) {
					next();
					next = null;
				}
			}
		}

		pump();

		try {
			while (true) {
				while (queue.length > 0) {
					const val = await queue[0];

					if (val === kEof) {
						return;
					}

					if (signal.aborted) {
						throw new AbortError();
					}

					if (val !== kEmpty) {
						yield val;
					}

					queue.shift();
					maybeResume();
				}

				const { promise, resolve } = Promise.withResolvers<void>();
				next = resolve;
				await promise;
			}
		} finally {
			done = true;
			if (resume) {
				resume();
				resume = null;
			}
			destroyImpl.destroyer(stream, null);
		}
	}.call(this);
}

async function some(this: StreamThis, fn: MapFn, options?: OperatorOptions): Promise<boolean> {
	for await (const _unused of filter.call(this, fn, options)) {
		return true;
	}
	return false;
}

async function every(this: StreamThis, fn: MapFn, options?: OperatorOptions): Promise<boolean> {
	validateFunction(fn, "fn");
	// https://en.wikipedia.org/wiki/De_Morgan%27s_laws
	return !(await some.call(
		this,
		async (value: unknown, opts: SignalOpt) => {
			return !(await fn(value, opts));
		},
		options,
	));
}

async function find(this: StreamThis, fn: MapFn, options?: OperatorOptions): Promise<unknown> {
	for await (const result of filter.call(this, fn, options)) {
		return result;
	}
	return undefined;
}

async function forEach(this: StreamThis, fn: MapFn, options?: OperatorOptions): Promise<void> {
	validateFunction(fn, "fn");
	async function forEachFn(value: unknown, opts: SignalOpt) {
		await fn(value, opts);
		return kEmpty;
	}
	for await (const _unused of map.call(this, forEachFn, options));
}

function filter(this: StreamThis, fn: MapFn, options?: OperatorOptions): AsyncGenerator<unknown> {
	validateFunction(fn, "fn");
	async function filterFn(value: unknown, opts: SignalOpt) {
		if (await fn(value, opts)) {
			return value;
		}
		return kEmpty;
	}
	return map.call(this, filterFn, options);
}

// Better error for reduce: the argument is only missing if the stream has no items, but the code still applies.
class ReduceAwareErrMissingArgs extends ERR_MISSING_ARGS {
	constructor() {
		super("reduce");
		this.message = "Reduce of an empty stream requires an initial value";
	}
}

async function reduce(
	this: StreamThis,
	reducer: (acc: unknown, value: unknown, opts: SignalOpt) => unknown,
	initialValue?: unknown,
	options?: OperatorOptions,
): Promise<unknown> {
	validateFunction(reducer, "reducer");
	validateOptionsAndSignal(options);

	let hasInitialValue = arguments.length > 1;
	if (options?.signal?.aborted) {
		const err = new AbortError(undefined, { cause: options.signal.reason });
		this.once("error", () => {}); // The error is already propagated
		await finished(this.destroy(err));
		throw err;
	}
	const ac = new AbortController();
	const signal = ac.signal;
	const outerSignal = options?.signal;
	const onAbort = () => ac.abort();
	if (outerSignal) {
		outerSignal.addEventListener("abort", onAbort, { once: true });
	}
	let gotAnyItemFromStream = false;
	try {
		for await (const value of this) {
			gotAnyItemFromStream = true;
			if (options?.signal?.aborted) {
				throw new AbortError();
			}
			if (!hasInitialValue) {
				initialValue = value;
				hasInitialValue = true;
			} else {
				initialValue = await reducer(initialValue, value, { signal });
			}
		}
		if (!gotAnyItemFromStream && !hasInitialValue) {
			throw new ReduceAwareErrMissingArgs();
		}
	} finally {
		// Node registers a weak listener; remove ours explicitly so the stream is not retained by the signal.
		outerSignal?.removeEventListener("abort", onAbort);
		ac.abort();
	}
	return initialValue;
}

async function toArray(this: StreamThis, options?: OperatorOptions): Promise<unknown[]> {
	validateOptionsAndSignal(options);

	const result: unknown[] = [];
	for await (const val of this) {
		if (options?.signal?.aborted) {
			throw new AbortError(undefined, { cause: options.signal.reason });
		}
		result.push(val);
	}
	return result;
}

function flatMap(this: StreamThis, fn: MapFn, options?: OperatorOptions): AsyncGenerator<unknown> {
	const values = map.call(this, fn, options);
	return async function* flatMap(this: StreamThis): AsyncGenerator<unknown> {
		for await (const val of values) {
			yield* val as AsyncIterable<unknown>;
		}
	}.call(this);
}

function toIntegerOrInfinity(input: unknown): number {
	// Coerce to align with the spec: https://github.com/tc39/proposal-iterator-helpers/issues/169
	const number = Number(input);
	if (Number.isNaN(number)) {
		return 0;
	}
	if (number < 0) {
		throw new ERR_OUT_OF_RANGE("number", ">= 0", number);
	}
	return number;
}

function drop(this: StreamThis, input: unknown, options?: OperatorOptions): AsyncGenerator<unknown> {
	validateOptionsAndSignal(options);

	let number = toIntegerOrInfinity(input);
	return async function* drop(this: StreamThis): AsyncGenerator<unknown> {
		if (options?.signal?.aborted) {
			throw new AbortError();
		}
		for await (const val of this) {
			if (options?.signal?.aborted) {
				throw new AbortError();
			}
			if (number-- <= 0) {
				yield val;
			}
		}
	}.call(this);
}

function take(this: StreamThis, input: unknown, options?: OperatorOptions): AsyncGenerator<unknown> {
	validateOptionsAndSignal(options);

	let number = toIntegerOrInfinity(input);
	return async function* take(this: StreamThis): AsyncGenerator<unknown> {
		if (options?.signal?.aborted) {
			throw new AbortError();
		}
		for await (const val of this) {
			if (options?.signal?.aborted) {
				throw new AbortError();
			}
			if (number-- > 0) {
				yield val;
			}

			// Don't get another item from iterator in case we reached the end
			if (number <= 0) {
				return;
			}
		}
	}.call(this);
}

export const streamReturningOperators = {
	drop,
	filter,
	flatMap,
	map,
	take,
};

export const promiseReturningOperators = {
	every,
	forEach,
	reduce,
	toArray,
	some,
	find,
};
