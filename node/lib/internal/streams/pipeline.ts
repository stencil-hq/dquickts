// Port of Node's lib/internal/streams/pipeline.js
// (originally from https://github.com/mafintosh/pump, with permission from Mathias Buus).
// Streams here are duck-typed (Node streams, web streams, iterables); types are erased at load.
import { addAbortListener } from "node:events";
import { eos } from "internal:streams/end-of-stream";
import * as destroyImpl from "internal:streams/destroy";
import { Duplex } from "internal:streams/duplex";
import { Readable } from "internal:streams/readable";
import { PassThrough } from "internal:streams/passthrough";
import {
	AbortError,
	aggregateTwoErrors,
	ERR_INVALID_ARG_TYPE,
	ERR_INVALID_RETURN_VALUE,
	ERR_MISSING_ARGS,
	ERR_STREAM_DESTROYED,
	ERR_STREAM_PREMATURE_CLOSE,
	ERR_STREAM_UNABLE_TO_PIPE,
	once,
	validateAbortSignal,
	validateFunction,
} from "internal:streams/shared";
import {
	isIterable,
	isReadable,
	isReadableFinished,
	isReadableNodeStream,
	isReadableStream,
	isNodeStream,
	isTransformStream,
	isWebStream,
} from "internal:streams/utils";

// biome-ignore lint: duck-typed stream values
type StreamLike = { [key: string | symbol]: unknown };
type ErrLike = { code?: string; name?: string } | null | undefined;
type Callback = (err?: unknown, value?: unknown) => void;
type Finish = (err?: unknown) => void;

function destroyer(stream: StreamLike, reading: boolean, writing: boolean) {
	let finished = false;
	stream.on("close", () => {
		finished = true;
	});

	const cleanup = eos(stream, { readable: reading, writable: writing }, (err: unknown) => {
		finished = !err;
	});

	return {
		destroy: (err?: unknown) => {
			if (finished) return;
			finished = true;
			destroyImpl.destroyer(stream, err || new ERR_STREAM_DESTROYED("pipe"));
		},
		cleanup,
	};
}

function popCallback(streams: unknown[]): Callback {
	// Streams always contains at least a single stream; optimize for the average case.
	validateFunction(streams[streams.length - 1], "streams[stream.length - 1]");
	return streams.pop() as Callback;
}

function makeAsyncIterable(val: unknown): AsyncIterable<unknown> | Iterable<unknown> {
	if (isIterable(val)) {
		return val;
	} else if (isReadableNodeStream(val)) {
		// Legacy streams are not Iterable.
		return fromReadable(val);
	}
	throw new ERR_INVALID_ARG_TYPE("val", ["Readable", "Iterable", "AsyncIterable"], val);
}

async function* fromReadable(val: unknown): AsyncGenerator<unknown> {
	yield* Readable.prototype[Symbol.asyncIterator].call(val);
}

async function pumpToNode(
	iterable: AsyncIterable<unknown>,
	writable: StreamLike,
	finish: Finish,
	{ end }: { end: boolean },
): Promise<void> {
	let error: unknown;
	let onresolve: (() => void) | null = null;

	const resume = (err?: unknown) => {
		if (err) {
			error = err;
		}
		if (onresolve) {
			const callback = onresolve;
			onresolve = null;
			callback();
		}
	};

	const wait = (): Promise<void> => {
		const { promise, resolve, reject } = Promise.withResolvers<void>();
		if (error) {
			reject(error);
		} else {
			onresolve = () => {
				if (error) {
					reject(error);
				} else {
					resolve();
				}
			};
		}
		return promise;
	};

	writable.on("drain", resume);
	const cleanup = eos(writable, { readable: false }, resume);

	try {
		if (writable.writableNeedDrain) {
			await wait();
		}

		for await (const chunk of iterable) {
			if (!writable.write(chunk)) {
				await wait();
			}
		}

		if (end) {
			writable.end();
			await wait();
		}

		finish();
	} catch (err) {
		finish(error !== err ? aggregateTwoErrors(error, err) : err);
	} finally {
		cleanup();
		writable.off("drain", resume);
	}
}

async function pumpToWeb(
	readable: AsyncIterable<unknown>,
	writable: StreamLike,
	finish: Finish,
	{ end }: { end: boolean },
): Promise<void> {
	if (isTransformStream(writable)) {
		writable = writable.writable;
	}
	// https://streams.spec.whatwg.org/#example-manual-write-with-backpressure
	const writer = writable.getWriter();
	try {
		for await (const chunk of readable) {
			await writer.ready;
			writer.write(chunk).catch(() => {});
		}

		await writer.ready;

		if (end) {
			await writer.close();
		}

		finish();
	} catch (err) {
		try {
			await writer.abort(err);
			finish(err);
		} catch (err2) {
			finish(err2);
		}
	}
}

export function pipeline(...streams: unknown[]): unknown {
	return pipelineImpl(streams, once(popCallback(streams)) as Callback);
}

export function pipelineImpl(
	streams: unknown[],
	callback: Callback,
	opts?: { signal?: AbortSignal; end?: boolean },
): StreamLike {
	if (streams.length === 1 && Array.isArray(streams[0])) {
		streams = streams[0];
	}

	if (streams.length < 2) {
		throw new ERR_MISSING_ARGS("streams");
	}

	const ac = new AbortController();
	const signal = ac.signal;
	const outerSignal = opts?.signal;

	// Need to cleanup event listeners if last stream is readable
	// https://github.com/nodejs/node/issues/35452
	const lastStreamCleanup: (() => void)[] = [];

	validateAbortSignal(outerSignal, "options.signal");

	function abort() {
		finishImpl(new AbortError(undefined, { cause: outerSignal?.reason }));
	}

	let disposable: Disposable | undefined;
	if (outerSignal) {
		disposable = addAbortListener(outerSignal, abort);
	}

	let error: unknown;
	let value: unknown;
	const destroys: ((err?: unknown) => void)[] = [];

	let finishCount = 0;

	function finish(err?: unknown) {
		finishImpl(err, --finishCount === 0);
	}

	function finishOnlyHandleError(err?: unknown) {
		finishImpl(err, false);
	}

	function finishImpl(err: unknown, final?: boolean) {
		const prev = error as ErrLike;
		if (err && (!prev || prev.code === "ERR_STREAM_PREMATURE_CLOSE" || prev.name === "AbortError")) {
			error = err;
		}

		if (!error && !final) {
			return;
		}

		while (destroys.length) {
			destroys.shift()!(error);
		}

		disposable?.[Symbol.dispose]();
		ac.abort();

		if (final) {
			if (!error) {
				lastStreamCleanup.forEach((fn) => fn());
			}
			process.nextTick(callback, error, value);
		}
	}

	let ret: StreamLike;
	for (let i = 0; i < streams.length; i++) {
		const stream = streams[i] as StreamLike;
		const reading = i < streams.length - 1;
		const writing = i > 0;
		const next = (i + 1 < streams.length ? streams[i + 1] : null) as StreamLike | null;
		const end = reading || opts?.end !== false;
		const isLastStream = i === streams.length - 1;

		if (isNodeStream(stream)) {
			if (next !== null && (next?.closed || next?.destroyed)) {
				throw new ERR_STREAM_UNABLE_TO_PIPE();
			}

			if (end) {
				const { destroy, cleanup } = destroyer(stream, reading, writing);
				destroys.push(destroy);

				if (isReadable(stream) && isLastStream) {
					lastStreamCleanup.push(cleanup);
				}
			}

			// Catch stream errors that occur after pipe/pump has completed.
			const onError = (err: ErrLike) => {
				if (err && err.name !== "AbortError" && err.code !== "ERR_STREAM_PREMATURE_CLOSE") {
					finishOnlyHandleError(err);
				}
			};
			stream.on("error", onError);
			if (isReadable(stream) && isLastStream) {
				lastStreamCleanup.push(() => {
					stream.removeListener("error", onError);
				});
			}
		}

		if (i === 0) {
			if (typeof stream === "function") {
				ret = stream({ signal });
				if (!isIterable(ret)) {
					throw new ERR_INVALID_RETURN_VALUE("Iterable, AsyncIterable or Stream", "source", ret);
				}
			} else if (isIterable(stream) || isReadableNodeStream(stream) || isTransformStream(stream)) {
				ret = stream;
			} else {
				ret = Duplex.from(stream);
			}
		} else if (typeof stream === "function") {
			if (isTransformStream(ret!)) {
				ret = makeAsyncIterable(ret?.readable);
			} else {
				ret = makeAsyncIterable(ret!);
			}
			ret = stream(ret, { signal });

			if (reading) {
				if (!isIterable(ret, true)) {
					throw new ERR_INVALID_RETURN_VALUE("AsyncIterable", `transform[${i - 1}]`, ret);
				}
			} else {
				// If the last argument to pipeline is not a stream we must create a proxy stream so that
				// pipeline(...) always returns a stream which can be further composed through `.pipe(stream)`.
				const pt = new PassThrough({
					objectMode: true,
				});

				// Handle Promises/A+ spec, `then` could be a getter that throws on second use.
				const then = ret?.then;
				if (typeof then === "function") {
					finishCount++;
					then.call(
						ret,
						(val: unknown) => {
							value = val;
							if (val != null) {
								pt.write(val);
							}
							if (end) {
								pt.end();
							}
							process.nextTick(finish);
						},
						(err: unknown) => {
							pt.destroy(err);
							process.nextTick(finish, err);
						},
					);
				} else if (isIterable(ret, true)) {
					finishCount++;
					pumpToNode(ret, pt, finish, { end });
				} else if (isReadableStream(ret) || isTransformStream(ret)) {
					const toRead = ret.readable || ret;
					finishCount++;
					pumpToNode(toRead, pt, finish, { end });
				} else {
					throw new ERR_INVALID_RETURN_VALUE("AsyncIterable or Promise", "destination", ret);
				}

				ret = pt;

				const { destroy, cleanup } = destroyer(ret, false, true);
				destroys.push(destroy);
				if (isLastStream) {
					lastStreamCleanup.push(cleanup);
				}
			}
		} else if (isNodeStream(stream)) {
			if (isReadableNodeStream(ret!)) {
				finishCount += 2;
				const cleanup = pipe(ret, stream, finish, finishOnlyHandleError, { end });
				if (isReadable(stream) && isLastStream) {
					lastStreamCleanup.push(cleanup);
				}
			} else if (isTransformStream(ret!) || isReadableStream(ret!)) {
				const toRead = ret.readable || ret;
				finishCount++;
				pumpToNode(toRead, stream, finish, { end });
			} else if (isIterable(ret!)) {
				finishCount++;
				pumpToNode(ret, stream, finish, { end });
			} else {
				throw new ERR_INVALID_ARG_TYPE(
					"val",
					["Readable", "Iterable", "AsyncIterable", "ReadableStream", "TransformStream"],
					ret!,
				);
			}
			ret = stream;
		} else if (isWebStream(stream)) {
			if (isReadableNodeStream(ret!)) {
				finishCount++;
				pumpToWeb(makeAsyncIterable(ret), stream, finish, { end });
			} else if (isReadableStream(ret!) || isIterable(ret!)) {
				finishCount++;
				pumpToWeb(ret, stream, finish, { end });
			} else if (isTransformStream(ret!)) {
				finishCount++;
				pumpToWeb(ret.readable, stream, finish, { end });
			} else {
				throw new ERR_INVALID_ARG_TYPE(
					"val",
					["Readable", "Iterable", "AsyncIterable", "ReadableStream", "TransformStream"],
					ret!,
				);
			}
			ret = stream;
		} else {
			ret = Duplex.from(stream);
		}
	}

	if (signal?.aborted || outerSignal?.aborted) {
		process.nextTick(abort);
	}

	return ret!;
}

function pipe(
	src: StreamLike,
	dst: StreamLike,
	finish: Finish,
	finishOnlyHandleError: Finish,
	{ end }: { end: boolean },
): () => void {
	let ended = false;
	dst.on("close", () => {
		if (!ended) {
			// Finish if the destination closes before the source has completed.
			finishOnlyHandleError(new ERR_STREAM_PREMATURE_CLOSE());
		}
	});

	src.pipe(dst, { end: false }); // If end is true we already will have a listener to end dst.

	if (end) {
		// Compat: stdio destinations are ended but "secretly" don't close the underlying fd.
		const endFn = () => {
			ended = true;
			dst.end();
		};

		if (isReadableFinished(src)) {
			// End the destination if the source has already ended.
			process.nextTick(endFn);
		} else {
			src.once("end", endFn);
		}
	} else {
		finish();
	}

	eos(src, { readable: true, writable: false }, (err: ErrLike) => {
		const rState = src._readableState as { ended?: boolean; errored?: unknown; errorEmitted?: boolean } | undefined;
		if (err && err.code === "ERR_STREAM_PREMATURE_CLOSE" && rState?.ended && !rState.errored && !rState.errorEmitted) {
			// Some readable streams emit 'close' before 'end'; 'end' should still be emitted if the stream has
			// ended without error (backwards compatibility). eos only fails with premature close on the reading
			// side for duplex streams.
			src.once("end", finish).once("error", finish);
		} else {
			finish(err);
		}
	});
	return eos(dst, { readable: false, writable: true }, finish);
}
