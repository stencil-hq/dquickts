// Port of Node's lib/internal/streams/from.js. `Readable` is passed in as a parameter
// (avoids the from -> readable import cycle).
import { Buffer } from "node:buffer";
import { aggregateTwoErrors, ERR_INVALID_ARG_TYPE, ERR_STREAM_NULL_VALUES } from "internal:streams/shared";

type Cb = (err?: unknown) => void;
interface ReadableLike {
	push(chunk: unknown): boolean;
	destroy(err?: unknown): unknown;
	_read: (this: ReadableLike, n?: number) => void;
	_destroy: (this: ReadableLike, err: unknown, cb: Cb) => void;
}
type ReadableCtor = new (opts: object) => ReadableLike;
interface IterResult {
	value: unknown;
	done?: boolean;
}
interface AnyIterator {
	next(): IterResult | Promise<IterResult>;
	throw?(err: unknown): IterResult | Promise<IterResult>;
	return?(): IterResult | Promise<IterResult>;
}

function isThenable(value: unknown): value is PromiseLike<unknown> {
	return (
		(typeof value === "object" || typeof value === "function") &&
		value !== null &&
		"then" in value &&
		typeof value.then === "function"
	);
}

export function from(Readable: ReadableCtor, iterable: unknown, opts?: object): ReadableLike {
	let iterator: AnyIterator;
	if (typeof iterable === "string" || iterable instanceof Buffer) {
		return new Readable({
			objectMode: true,
			...opts,
			read(this: ReadableLike) {
				this.push(iterable);
				this.push(null);
			},
		});
	}

	let isAsync: boolean;
	const it = iterable as
		| { [Symbol.asyncIterator]?: () => AnyIterator; [Symbol.iterator]?: () => AnyIterator }
		| null
		| undefined;
	if (it?.[Symbol.asyncIterator]) {
		isAsync = true;
		iterator = it[Symbol.asyncIterator]!();
	} else if (it?.[Symbol.iterator]) {
		isAsync = false;
		iterator = it[Symbol.iterator]!();
	} else {
		throw new ERR_INVALID_ARG_TYPE("iterable", ["Iterable"], iterable);
	}

	const readable = new Readable({
		objectMode: true,
		highWaterMark: 1,
		...opts,
	});
	const originalDestroy = readable._destroy;

	// Flag to protect against _read being called before last iteration completion.
	let reading = false;
	let isAsyncValues = false;

	readable._read = function () {
		if (!reading) {
			reading = true;
			if (isAsync) {
				nextAsync();
			} else if (isAsyncValues) {
				nextSyncWithAsyncValues();
			} else {
				nextSyncWithSyncValues();
			}
		}
	};

	readable._destroy = function (error: unknown, cb: Cb) {
		originalDestroy.call(this, error, (destroyError?: unknown) => {
			const combinedError = destroyError || error;
			close(combinedError).then(
				// nextTick is here in case cb throws
				() => process.nextTick(cb, combinedError),
				(closeError: unknown) => process.nextTick(cb, aggregateTwoErrors(combinedError, closeError)),
			);
		});
	};

	async function close(error: unknown): Promise<void> {
		const hadError = error !== undefined && error !== null;
		const hasThrow = typeof iterator.throw === "function";
		if (hadError && hasThrow) {
			const { value, done } = await iterator.throw!(error);
			await value;
			if (done) {
				return;
			}
		}
		if (typeof iterator.return === "function") {
			const { value } = await iterator.return();
			await value;
		}
	}

	// Duplication on purpose for performance: avoid await when not needed.

	function nextSyncWithSyncValues(): void {
		for (;;) {
			try {
				const { value, done } = iterator.next() as IterResult;
				if (done) {
					readable.push(null);
					return;
				}
				if (isThenable(value)) {
					changeToAsyncValues(value);
					return;
				}
				if (value === null) {
					reading = false;
					throw new ERR_STREAM_NULL_VALUES();
				}
				if (readable.push(value)) {
					continue;
				}
				reading = false;
			} catch (err) {
				readable.destroy(err);
			}
			break;
		}
	}

	async function changeToAsyncValues(value: PromiseLike<unknown>): Promise<void> {
		isAsyncValues = true;
		try {
			const res = await value;
			if (res === null) {
				reading = false;
				throw new ERR_STREAM_NULL_VALUES();
			}
			if (readable.push(res)) {
				nextSyncWithAsyncValues();
				return;
			}
			reading = false;
		} catch (err) {
			readable.destroy(err);
		}
	}

	async function nextSyncWithAsyncValues(): Promise<void> {
		for (;;) {
			try {
				const { value, done } = iterator.next() as IterResult;
				if (done) {
					readable.push(null);
					return;
				}
				const res = isThenable(value) ? await value : value;
				if (res === null) {
					reading = false;
					throw new ERR_STREAM_NULL_VALUES();
				}
				if (readable.push(res)) {
					continue;
				}
				reading = false;
			} catch (err) {
				readable.destroy(err);
			}
			break;
		}
	}

	async function nextAsync(): Promise<void> {
		for (;;) {
			try {
				const { value, done } = await iterator.next();
				if (done) {
					readable.push(null);
					return;
				}
				if (value === null) {
					reading = false;
					throw new ERR_STREAM_NULL_VALUES();
				}
				if (readable.push(value)) {
					continue;
				}
				reading = false;
			} catch (err) {
				readable.destroy(err);
			}
			break;
		}
	}
	return readable;
}
