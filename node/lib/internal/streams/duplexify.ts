// Port of Node's lib/internal/streams/duplexify.js (implements Duplex.from). Values are duck-typed.
import {
	isReadable,
	isWritable,
	isIterable,
	isNodeStream,
	isReadableNodeStream,
	isWritableNodeStream,
	isDuplexNodeStream,
	isReadableStream,
	isWritableStream,
} from "internal:streams/utils";
import { eos } from "internal:streams/end-of-stream";
import { AbortError, ERR_INVALID_ARG_TYPE, ERR_INVALID_RETURN_VALUE } from "internal:streams/shared";
import { destroyer } from "internal:streams/destroy";
import { Duplex } from "internal:streams/duplex";
import { Readable } from "internal:streams/readable";
import { Writable } from "internal:streams/writable";
import { from } from "internal:streams/from";

type StreamLike = { [key: string | symbol]: unknown };
type Cb = (err?: unknown) => void;
interface Pair {
	readable?: StreamLike | false;
	writable?: StreamLike | false;
}
interface Signal {
	chunk?: unknown;
	done: boolean;
	cb: () => void;
}

// This is needed for pre node 17.
class Duplexify extends Duplex {
	constructor(options?: { readable?: boolean; writable?: boolean; [key: string]: unknown }) {
		super(options);

		// https://github.com/nodejs/node/pull/34385

		if (options?.readable === false) {
			this._readableState.readable = false;
			this._readableState.ended = true;
			this._readableState.endEmitted = true;
		}

		if (options?.writable === false) {
			this._writableState.writable = false;
			this._writableState.ending = true;
			this._writableState.ended = true;
			this._writableState.finished = true;
		}
	}
}

export function duplexify(body: unknown, name?: string): StreamLike {
	if (isDuplexNodeStream(body)) {
		return body;
	}

	if (isReadableNodeStream(body)) {
		return _duplexify({ readable: body });
	}

	if (isWritableNodeStream(body)) {
		return _duplexify({ writable: body });
	}

	if (isNodeStream(body)) {
		return _duplexify({ writable: false, readable: false });
	}

	if (isReadableStream(body)) {
		return _duplexify({ readable: Readable.fromWeb(body) });
	}

	if (isWritableStream(body)) {
		return _duplexify({ writable: Writable.fromWeb(body) });
	}

	if (typeof body === "function") {
		const { value, write, final, destroy } = fromAsyncGen(body);

		// Body might be a constructor function instead of an async generator function.
		if (isDuplexNodeStream(value)) {
			return value;
		}

		if (isIterable(value)) {
			return from(Duplexify, value, {
				objectMode: true,
				write,
				final,
				destroy,
			});
		}

		const then = value?.then;
		if (typeof then === "function") {
			let d: StreamLike;
			// Tracks whether writable final() has started.
			let finalized = false;

			const promise = then.call(
				value,
				(val: unknown) => {
					if (val != null) {
						throw new ERR_INVALID_RETURN_VALUE("nully", "body", val);
					}
					// The async function returned without (fully) consuming the input. Destroy the duplex so
					// that pipeline propagates destruction upstream (https://github.com/nodejs/node/issues/55077).
					if (!finalized) {
						destroyer(d);
					}
				},
				(err: unknown) => {
					destroyer(d, err);
				},
			);

			return (d = new Duplexify({
				objectMode: true,
				readable: false,
				write,
				final(cb: Cb) {
					finalized = true;
					final(async () => {
						try {
							await promise;
							process.nextTick(cb, null);
						} catch (err) {
							process.nextTick(cb, err);
						}
					});
				},
				destroy,
			}));
		}

		throw new ERR_INVALID_RETURN_VALUE("Iterable, AsyncIterable or AsyncFunction", name, value);
	}

	if (body instanceof Blob) {
		return duplexify(body.arrayBuffer());
	}

	if (isIterable(body)) {
		return from(Duplexify, body, {
			objectMode: true,
			writable: false,
		});
	}

	const b = body as StreamLike | null | undefined;

	if (isReadableStream(b?.readable) && isWritableStream(b?.writable)) {
		return Duplexify.fromWeb(body);
	}

	if (typeof b?.writable === "object" || typeof b?.readable === "object") {
		const readable = b?.readable
			? isReadableNodeStream(b?.readable)
				? b?.readable
				: duplexify(b.readable)
			: undefined;

		const writable = b?.writable
			? isWritableNodeStream(b?.writable)
				? b?.writable
				: duplexify(b.writable)
			: undefined;

		return _duplexify({ readable, writable });
	}

	const then = b?.then;
	if (typeof then === "function") {
		let d: StreamLike;

		then.call(
			body,
			(val: unknown) => {
				if (val != null) {
					d.push(val);
				}
				d.push(null);
			},
			(err: unknown) => {
				destroyer(d, err);
			},
		);

		return (d = new Duplexify({
			objectMode: true,
			writable: false,
			read() {},
		}));
	}

	throw new ERR_INVALID_ARG_TYPE(
		name,
		[
			"Blob",
			"ReadableStream",
			"WritableStream",
			"Stream",
			"Iterable",
			"AsyncIterable",
			"Function",
			"{ readable, writable } pair",
			"Promise",
		],
		body,
	);
}

function fromAsyncGen(fn: (source: AsyncGenerator<unknown>, opts: { signal: AbortSignal }) => unknown) {
	let { promise, resolve } = Promise.withResolvers<Signal>() as {
		promise: Promise<Signal> | null;
		resolve: ((v: Signal) => void) | null;
	};
	const ac = new AbortController();
	const signal = ac.signal;
	const value = fn(
		(async function* () {
			while (true) {
				const _promise = promise!;
				promise = null;
				const { chunk, done, cb } = await _promise;
				process.nextTick(cb);
				if (done) return;
				if (signal.aborted) throw new AbortError(undefined, { cause: signal.reason });
				({ promise, resolve } = Promise.withResolvers<Signal>());
				yield chunk;
			}
		})(),
		{ signal },
	) as StreamLike | null | undefined;

	return {
		value,
		write(chunk: unknown, _encoding: string, cb: () => void) {
			const _resolve = resolve!;
			resolve = null;
			_resolve({ chunk, done: false, cb });
		},
		final(cb: () => void) {
			const _resolve = resolve!;
			resolve = null;
			_resolve({ done: true, cb });
		},
		destroy(err: unknown, cb: Cb) {
			ac.abort(err);

			// If the source async iterator is waiting for the next write/final signal, unblock it so the
			// readable side can observe the abort and finish destroying.
			if (resolve !== null) {
				const _resolve = resolve;
				resolve = null;
				_resolve({ done: true, cb() {} });
			}

			cb(err);
		},
	};
}

function _duplexify(pair: Pair): StreamLike {
	const r = (
		pair.readable && typeof pair.readable.read !== "function" ? Readable.wrap(pair.readable) : pair.readable
	) as StreamLike;
	const w = pair.writable as StreamLike;

	let readable = !!isReadable(r);
	let writable = !!isWritable(w);

	let ondrain: Cb | null | undefined;
	let onfinish: Cb | null | undefined;
	let onreadable: (() => void) | null | undefined;
	// Starts `undefined` (not null) on purpose: _destroy before completion reports AbortError.
	let onclose: Cb | null | undefined;
	let d: StreamLike;

	function onfinished(err?: unknown) {
		const cb = onclose;
		onclose = null;

		if (cb) {
			cb(err);
		} else if (err) {
			d.destroy(err);
		}
	}

	// Implement Writable/Readable/Duplex traits (see https://github.com/nodejs/node/pull/33515).
	d = new Duplexify({
		readableObjectMode: !!r?.readableObjectMode,
		writableObjectMode: !!w?.writableObjectMode,
		readable,
		writable,
	});

	if (writable) {
		eos(w, (err: unknown) => {
			writable = false;
			if (err) {
				destroyer(r, err);
			}
			onfinished(err);
		});

		d._write = function (chunk: unknown, encoding: string, callback: Cb) {
			if (w.write(chunk, encoding)) {
				callback();
			} else {
				ondrain = callback;
			}
		};

		d._final = function (callback: Cb) {
			w.end();
			onfinish = callback;
		};

		w.on("drain", function () {
			if (ondrain) {
				const cb = ondrain;
				ondrain = null;
				cb();
			}
		});

		w.on("finish", function () {
			if (onfinish) {
				const cb = onfinish;
				onfinish = null;
				cb();
			}
		});
	}

	if (readable) {
		eos(r, (err: unknown) => {
			readable = false;
			if (err) {
				destroyer(w, err);
			}
			onfinished(err);
		});

		r.on("readable", function () {
			if (onreadable) {
				const cb = onreadable;
				onreadable = null;
				cb();
			}
		});

		r.on("end", function () {
			d.push(null);
		});

		d._read = function () {
			while (true) {
				const buf = r.read();

				if (buf === null) {
					onreadable = d._read;
					return;
				}

				if (!d.push(buf)) {
					return;
				}
			}
		};
	}

	d._destroy = function (err: unknown, callback: Cb) {
		if (!err && onclose !== null) {
			err = new AbortError();
		}

		onreadable = null;
		ondrain = null;
		onfinish = null;

		if (onclose === null) {
			callback(err);
		} else {
			onclose = callback;
			destroyer(w, err);
			destroyer(r, err);
		}
	};

	return d;
}
