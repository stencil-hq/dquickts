// Port of Node's lib/internal/webstreams/adapters.js (Node stream <-> WHATWG stream adapters).
// The StreamBase adapters (newWritableStreamFromStreamBase, newReadableStreamFromStreamBase) are not ported, and
// async-context-frame propagation is dropped (callbacks run in the current context).
import { Buffer } from "node:buffer";
import {
	ReadableStream,
	isReadableStream,
	readableStreamDefaultReaderRead,
	kChunk,
	kClose,
	kError,
} from "internal:webstreams/readablestream";
import {
	WritableStream,
	isWritableStream,
	writableStreamDefaultWriterIsStarted,
	writableStreamDefaultWriterWriteFailure,
	writableStreamDefaultWriterWriteWithRequest,
} from "internal:webstreams/writablestream";
import { CountQueuingStrategy, ByteLengthQueuingStrategy } from "internal:webstreams/queuingstrategies";
import { kResolvedPromise } from "internal:webstreams/util";
import { Writable } from "internal:streams/writable";
import { Readable } from "internal:streams/readable";
import { Duplex } from "internal:streams/duplex";
import { destroyer as destroy } from "internal:streams/destroy";
import { isDestroyed, isReadable, isWritable, isWritableEnded } from "internal:streams/utils";
import { eos, kEosNodeSynchronousCallback } from "internal:streams/end-of-stream";
import {
	AbortError,
	ERR_INVALID_ARG_TYPE,
	ERR_INVALID_ARG_VALUE,
	ERR_STREAM_PREMATURE_CLOSE,
	isAnyArrayBuffer,
	kEmptyObject,
	validateBoolean,
	validateObject,
	validateOneOf,
} from "internal:streams/shared";

// Duck-typed Node stream / web stream values; types are erased at load.
type StreamLike = { [key: string | symbol]: unknown };
type Cb = (err?: unknown) => void;
type ErrLike = { code?: unknown } | null | undefined;

const encoder = new TextEncoder();

export const kValidateChunk = Symbol("kValidateChunk");
export const kDestroyOnSyncError = Symbol("kDestroyOnSyncError");

// All negative (error) zlib codes and Z_NEED_DICT, by name (as used for `err.code` by node:zlib).
const ZLIB_FAILURES = new Set([
	"Z_NEED_DICT",
	"Z_ERRNO",
	"Z_STREAM_ERROR",
	"Z_DATA_ERROR",
	"Z_MEM_ERROR",
	"Z_BUF_ERROR",
	"Z_VERSION_ERROR",
]);

function handleKnownInternalErrors(cause: unknown): unknown {
	const causeCode = (cause as ErrLike)?.code;
	if (causeCode === "ERR_STREAM_PREMATURE_CLOSE") {
		return new AbortError(undefined, { cause });
	}
	// Brotli decoder error codes are 'ERR_' + BrotliDecoderErrorString() ('_ERROR_FORMAT_...', ...),
	// so the resulting codes all start with 'ERR__ERROR_'.
	if (
		(typeof causeCode === "string" && ZLIB_FAILURES.has(causeCode)) ||
		(causeCode != null && String(causeCode).startsWith("ERR__ERROR_"))
	) {
		const error = new TypeError(undefined, { cause });
		Object.defineProperty(error, "code", { value: causeCode, enumerable: true, configurable: true, writable: true });
		return error;
	}
	return cause;
}

const noop = () => {};

// Node's internal/util normalizeEncoding, reduced to the utf8 check this module needs.
function isUtf8Encoding(enc: unknown): boolean {
	if (enc == null || enc === "utf8" || enc === "utf-8") return true;
	const lower = String(enc).toLowerCase();
	return lower === "utf8" || lower === "utf-8" || lower === "";
}

function toUint8Chunk(chunk: string, encoding: string): Uint8Array {
	if (isUtf8Encoding(encoding)) {
		return encoder.encode(chunk);
	}
	const buf = Buffer.from(chunk, encoding as BufferEncoding);
	return new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
}

interface WriteTracker {
	promise: null;
	pending: number;
	failed: boolean;
	failure: unknown;
	callback: Cb | undefined;
	error: unknown;
	hasError: boolean;
	dispatching: boolean;
	resolve(): void;
	reject(error: unknown): void;
	settle(): void;
}

// A single shared write request tracks every chunk written to the destination WritableStream, replacing the
// per-write promise pair that writer.ready + writer.write() would allocate. Only one write/writev batch is in
// flight at a time, so one callback slot is enough. `promise` is non-undefined so writable-stream queue entries
// are discriminated from kNilRequest by `promise === undefined`; `failed`/`failure` latch a write that could
// not proceed.
function createWriteTracker(getDestination: () => StreamLike): WriteTracker {
	return {
		promise: null,
		pending: 0,
		failed: false,
		failure: undefined,
		callback: undefined,
		error: undefined,
		hasError: false,
		dispatching: false,
		resolve() {
			this.pending--;
			if (!this.dispatching && this.pending === 0 && this.callback !== undefined) {
				this.settle();
			}
		},
		reject(error: unknown) {
			this.pending--;
			if (!this.hasError) {
				this.hasError = true;
				this.error = error;
			}
			if (!this.dispatching && this.pending === 0 && this.callback !== undefined) {
				this.settle();
			}
		},
		settle() {
			const callback = this.callback!;
			this.callback = undefined;
			let error: unknown;
			if (this.hasError) {
				error = this.error;
				this.hasError = false;
				this.error = undefined;
			} else if (this.failed) {
				error = this.failure;
			}
			this.failed = false;
			this.failure = undefined;
			if (error !== undefined) {
				// Deliver failures in a microtask: this can run while the writable-stream machinery is
				// mid-transition, and the callback typically destroys the stream, which re-enters it.
				kResolvedPromise.then(() => {
					try {
						callback(error);
					} catch (err) {
						process.nextTick(() => destroy(getDestination(), err));
					}
				});
				return;
			}
			try {
				callback(error);
			} catch (err) {
				// In a next tick: this can run within a promise context and must not become an unhandled rejection.
				process.nextTick(() => destroy(getDestination(), err));
			}
		},
	};
}

// A write issued before the destination has started is dispatched from a writer.ready reaction. The stream
// may have been closed or errored (e.g. by destroy()) by then.
function deferDispatch(writer: StreamLike, tracker: WriteTracker, dispatchNow: () => void): void {
	const dispatch = () => {
		const failure = writableStreamDefaultWriterWriteFailure(writer);
		if (failure !== undefined) {
			tracker.failed = true;
			tracker.failure = failure;
			tracker.settle();
			return;
		}
		dispatchNow();
	};
	(writer.ready as Promise<unknown>).then(dispatch, dispatch);
}

function dispatchWrite(writer: StreamLike, chunk: unknown, tracker: WriteTracker): void {
	if (!writableStreamDefaultWriterIsStarted(writer)) {
		deferDispatch(writer, tracker, () => dispatchWriteNow(writer, chunk, tracker));
		return;
	}
	dispatchWriteNow(writer, chunk, tracker);
}

function dispatchWriteNow(writer: StreamLike, chunk: unknown, tracker: WriteTracker): void {
	tracker.dispatching = true;
	writableStreamDefaultWriterWriteWithRequest(writer, chunk, tracker);
	tracker.dispatching = false;
	if (tracker.pending === 0) tracker.settle();
}

type WritevChunks = { chunk: unknown; encoding: string }[];

function dispatchWritev(writer: StreamLike, chunks: WritevChunks, tracker: WriteTracker): void {
	if (!writableStreamDefaultWriterIsStarted(writer)) {
		deferDispatch(writer, tracker, () => dispatchWritevNow(writer, chunks, tracker));
		return;
	}
	dispatchWritevNow(writer, chunks, tracker);
}

function dispatchWritevNow(writer: StreamLike, chunks: WritevChunks, tracker: WriteTracker): void {
	tracker.dispatching = true;
	for (let i = 0; i < chunks.length; i++) writableStreamDefaultWriterWriteWithRequest(writer, chunks[i].chunk, tracker);
	tracker.dispatching = false;
	if (tracker.pending === 0) tracker.settle();
}

interface WritableAdapterOptions {
	[kValidateChunk]?: (chunk: unknown) => void;
	[kDestroyOnSyncError]?: boolean;
}

export function newWritableStreamFromStreamWritable(
	streamWritable: StreamLike,
	options: WritableAdapterOptions = kEmptyObject,
): WritableStream {
	// Not using isWritableNodeStream: it returns false for a Duplex whose writable option is false. Such a
	// Duplex must pass this check and yield a closed WritableStream. Accepts stream.Writable or http.OutgoingMessage.
	const checkIfWritableOrOutgoingMessage =
		streamWritable && typeof streamWritable?.write === "function" && typeof streamWritable?.on === "function";
	if (!checkIfWritableOrOutgoingMessage) {
		throw new ERR_INVALID_ARG_TYPE("streamWritable", "stream.Writable", streamWritable);
	}

	if (isDestroyed(streamWritable) || !isWritable(streamWritable)) {
		const writable = new WritableStream();
		writable.close();
		return writable;
	}

	const highWaterMark = streamWritable.writableHighWaterMark as number;
	const strategy = streamWritable.writableObjectMode
		? new CountQueuingStrategy({ highWaterMark })
		: {
				highWaterMark,
				size(chunk: { byteLength?: number; length?: number } | null | undefined) {
					return chunk?.byteLength ?? chunk?.length ?? 1;
				},
			};

	let controller: { error(e: unknown): void } | undefined;
	let backpressurePromise: PromiseWithResolvers<void> | undefined;
	let closed: PromiseWithResolvers<void> | undefined;

	function onDrain() {
		backpressurePromise?.resolve();
	}

	const cleanup = eos(streamWritable, (err: unknown) => {
		const error = handleKnownInternalErrors(err);

		cleanup();
		// Protection against non-standard, legacy streams that emit an error event again after finished is called.
		streamWritable.on("error", () => {});
		if (error != null) {
			backpressurePromise?.reject(error);
			// If closed is not undefined, the error happens after the WritableStream close has already started.
			if (closed !== undefined) {
				closed.reject(error);
				closed = undefined;
			}
			controller?.error(error);
			controller = undefined;
			return;
		}

		if (closed !== undefined) {
			closed.resolve();
			closed = undefined;
			return;
		}
		controller?.error(new AbortError());
		controller = undefined;
	});

	streamWritable.on("drain", onDrain);

	return new WritableStream(
		{
			start(c: { error(e: unknown): void }) {
				controller = c;
			},

			write(chunk: unknown) {
				try {
					options[kValidateChunk]?.(chunk);
					if (!streamWritable.writableObjectMode && isAnyArrayBuffer(chunk)) {
						chunk = new Uint8Array(chunk);
					}
					if (streamWritable.writableNeedDrain || !streamWritable.write(chunk)) {
						backpressurePromise = Promise.withResolvers<void>();
						if (!streamWritable.writableNeedDrain) {
							backpressurePromise.resolve();
						}
						return backpressurePromise.promise.finally(() => {
							backpressurePromise = undefined;
						});
					}
				} catch (error) {
					// With kDestroyOnSyncError (e.g. CompressionStream) a sync throw also destroys the stream so the
					// readable side is errored too (TransformStream semantics), instead of hanging forever.
					if (options[kDestroyOnSyncError]) {
						destroy(streamWritable, error);
					}
					throw error;
				}
			},

			abort(reason: unknown) {
				destroy(streamWritable, reason);
			},

			close() {
				if (closed === undefined && !isWritableEnded(streamWritable)) {
					closed = Promise.withResolvers<void>();
					streamWritable.end();
					return closed.promise;
				}

				controller = undefined;
				return Promise.resolve();
			},
		},
		strategy,
	);
}

interface StreamWritableOptions {
	highWaterMark?: number;
	decodeStrings?: boolean;
	objectMode?: boolean;
	signal?: AbortSignal;
}

export function newStreamWritableFromWritableStream(
	writableStream: WritableStream,
	options: StreamWritableOptions = kEmptyObject,
): StreamLike {
	if (!isWritableStream(writableStream)) {
		throw new ERR_INVALID_ARG_TYPE("writableStream", "WritableStream", writableStream);
	}

	validateObject(options, "options");
	const { highWaterMark, decodeStrings = true, objectMode = false, signal } = options;

	validateBoolean(objectMode, "options.objectMode");
	validateBoolean(decodeStrings, "options.decodeStrings");

	const writer = writableStream.getWriter() as unknown as StreamLike;
	let closed = false;
	const writeTracker = createWriteTracker(() => writable);

	const writable: StreamLike = new Writable({
		highWaterMark,
		objectMode,
		decodeStrings,
		signal,

		writev(chunks: WritevChunks, callback: Cb) {
			writeTracker.callback = callback;
			dispatchWritev(writer, chunks, writeTracker);
		},

		write(chunk: unknown, encoding: string, callback: Cb) {
			if (typeof chunk === "string" && decodeStrings && !objectMode) {
				chunk = toUint8Chunk(chunk, encoding);
			}

			writeTracker.callback = callback;
			dispatchWrite(writer, chunk, writeTracker);
		},

		destroy(error: unknown, callback: Cb) {
			function done() {
				try {
					callback(error);
				} catch (err) {
					// Escape the promise context so the error is not an unhandled rejection.
					process.nextTick(() => {
						throw err;
					});
				}
			}

			if (!closed) {
				if (error != null) {
					(writer.abort as (e: unknown) => Promise<void>)(error).then(done, done);
				} else {
					(writer.close as () => Promise<void>)().then(done, done);
				}
				return;
			}

			done();
		},

		final(callback: Cb) {
			function done(error?: unknown) {
				try {
					callback(error);
				} catch (err) {
					// Escape the promise context so the error is not an unhandled rejection.
					process.nextTick(() => destroy(writable, err));
				}
			}

			if (!closed) {
				(writer.close as () => Promise<void>)().then(done, done);
			}
		},
	});

	(writer.closed as Promise<void>).then(
		() => {
			// The WritableStream closed before the stream.Writable was ended: signal an error.
			closed = true;
			if (!isWritableEnded(writable)) destroy(writable, new ERR_STREAM_PREMATURE_CLOSE());
		},
		(error: unknown) => {
			// The WritableStream errored before the stream.Writable was destroyed: signal the error.
			closed = true;
			destroy(writable, error);
		},
	);

	return writable;
}

const kErrorSentinelAttached = Symbol("kErrorSentinelAttached");

interface ReadableStreamAdapterOptions {
	strategy?: QueuingStrategy;
	type?: "bytes";
}

interface AdapterController {
	error(e: unknown): void;
	close(): void;
	enqueue(chunk: unknown): void;
	desiredSize: number | null;
	byobRequest?: { respond(n: number): void } | null;
}

export function newReadableStreamFromStreamReadable(
	streamReadable: StreamLike,
	options: ReadableStreamAdapterOptions = kEmptyObject,
): ReadableStream {
	// Not using isReadableNodeStream: it returns false for a Duplex whose readable option is false. Such a
	// Duplex must pass this check and yield a closed ReadableStream.
	if (typeof streamReadable?._readableState !== "object") {
		throw new ERR_INVALID_ARG_TYPE("streamReadable", "stream.Readable", streamReadable);
	}
	validateObject(options, "options");
	if (options.type !== undefined) {
		validateOneOf(options.type, "options.type", ["bytes", undefined]);
	}

	const isBYOB = options.type === "bytes";
	let controller: AdapterController;
	let wasCanceled = false;
	let strategy: QueuingStrategy | undefined;

	const underlyingSource: {
		type: "bytes" | undefined;
		start(c: AdapterController): void;
		cancel(reason: unknown): void;
		pull?: () => void;
	} = {
		type: isBYOB ? "bytes" : undefined,
		start(c: AdapterController) {
			controller = c;
		},
		cancel(reason: unknown) {
			wasCanceled = true;
			destroy(streamReadable, reason);
		},
	};

	const readable = isReadable(streamReadable);
	const objectMode = streamReadable.readableObjectMode;
	if (readable) {
		underlyingSource.pull = function pull() {
			streamReadable.resume();
		};

		const highWaterMark = streamReadable.readableHighWaterMark as number;
		strategy = isBYOB
			? { highWaterMark }
			: (options.strategy ?? new (objectMode ? CountQueuingStrategy : ByteLengthQueuingStrategy)({ highWaterMark }));
	}
	const readableStream = new ReadableStream(underlyingSource, strategy);

	// When adapting a Duplex as a ReadableStream, readable completion should not wait for a half-open
	// writable side to finish as well.
	let cleanup: () => void = noop;
	cleanup = eos(
		streamReadable,
		{
			writable: false,
			[kEosNodeSynchronousCallback]: true,
		},
		(err: unknown) => {
			const error = handleKnownInternalErrors(err);

			// If eos calls the callback synchronously, cleanup is still a no-op here.
			cleanup();

			if (!(kErrorSentinelAttached in streamReadable)) {
				// Protection against non-standard, legacy streams that emit an error event again after finished.
				streamReadable.on("error", noop);
				streamReadable[kErrorSentinelAttached] = true;
			}
			if (wasCanceled) {
				return;
			}
			wasCanceled = true;
			if (error) {
				controller.error(error);
				return;
			}
			controller.close();
			if (isBYOB) controller.byobRequest?.respond(0);
		},
	);

	if (wasCanceled) {
		// `eos` called the callback synchronously
		cleanup();
	} else if (readable) {
		streamReadable.pause();

		streamReadable.on("data", function onData(chunk: unknown) {
			if (wasCanceled) return;
			// Copy the Buffer to detach it from the pool.
			if (Buffer.isBuffer(chunk) && !objectMode) chunk = new Uint8Array(chunk);
			controller.enqueue(chunk);
			if ((controller.desiredSize as number) <= 0) streamReadable.pause();
		});
	}

	return readableStream;
}

interface StreamReadableOptions {
	highWaterMark?: number;
	encoding?: string;
	objectMode?: boolean;
	signal?: AbortSignal;
}

interface ReadRequest {
	[kChunk](chunk: unknown): void;
	[kClose](): void;
	[kError](error: unknown): void;
}

export function newStreamReadableFromReadableStream(
	readableStream: ReadableStream,
	options: StreamReadableOptions = kEmptyObject,
): StreamLike {
	if (!isReadableStream(readableStream)) {
		throw new ERR_INVALID_ARG_TYPE("readableStream", "ReadableStream", readableStream);
	}

	validateObject(options, "options");
	const { highWaterMark, encoding, objectMode = false, signal } = options;

	if (encoding !== undefined && !Buffer.isEncoding(encoding))
		throw new ERR_INVALID_ARG_VALUE("options.encoding", encoding);
	validateBoolean(objectMode, "options.objectMode");

	const reader = readableStream.getReader();
	let closed = false;
	let readRequest: ReadRequest | undefined;
	let pendingChunk: unknown;

	function forwardChunk() {
		const chunk = pendingChunk;
		pendingChunk = undefined;
		readable.push(chunk);
	}

	const readable: StreamLike = new Readable({
		objectMode,
		highWaterMark,
		encoding,
		signal,

		read() {
			// At most one read is in flight (Readable does not call read() again before push()), so a single
			// reused read request replaces the per-chunk promise/result/closures of reader.read(). The chunk is
			// forwarded in a microtask so an error or destroy can still beat it, as with reader.read().
			readRequest ??= {
				[kChunk](chunk: unknown) {
					pendingChunk = chunk;
					kResolvedPromise.then(forwardChunk);
				},
				[kClose]() {
					readable.push(null);
				},
				[kError](error: unknown) {
					destroy(readable, error);
				},
			};
			readableStreamDefaultReaderRead(reader, readRequest);
		},

		destroy(error: unknown, callback: Cb) {
			function done() {
				try {
					callback(error);
				} catch (err) {
					// Escape the promise context so the error is not an unhandled rejection.
					process.nextTick(() => {
						throw err;
					});
				}
			}

			if (!closed) {
				reader.cancel(error).then(done, done);
				return;
			}
			done();
		},
	});

	reader.closed.then(
		() => {
			closed = true;
		},
		(error: unknown) => {
			closed = true;
			destroy(readable, error);
		},
	);

	return readable;
}

let dep0201Emitted = false;
function emitDEP0201(): void {
	if (dep0201Emitted) return;
	dep0201Emitted = true;
	process.emitWarning(
		"Passing 'options.type' to Duplex.toWeb() is deprecated. To specify the ReadableStream type, use 'options.readableType'.",
		"DeprecationWarning",
		"DEP0201",
	);
}

interface DuplexToWebOptions extends WritableAdapterOptions {
	readableType?: "bytes";
	type?: "bytes";
}

export function newReadableWritablePairFromDuplex(
	duplex: StreamLike,
	options: DuplexToWebOptions = kEmptyObject,
): { readable: ReadableStream; writable: WritableStream } {
	// Not using isWritableNodeStream / isReadableNodeStream: they return false if the duplex was created with
	// writable or readable set to false. Check the state afterwards and return closed streams as necessary.
	if (typeof duplex?._writableState !== "object" || typeof duplex?._readableState !== "object") {
		throw new ERR_INVALID_ARG_TYPE("duplex", "stream.Duplex", duplex);
	}

	validateObject(options, "options");

	const readableOptions: ReadableStreamAdapterOptions = {
		type: options.readableType,
	};

	if (options.readableType == null && options.type != null) {
		// 'options.type' is a deprecated alias for 'options.readableType'
		emitDEP0201();
		readableOptions.type = options.type;
	}

	if (isDestroyed(duplex)) {
		const writable = new WritableStream();
		const readable = new ReadableStream({ type: readableOptions.type });
		writable.close();
		readable.cancel();
		return { readable, writable };
	}

	const writableOptions: WritableAdapterOptions = {
		[kValidateChunk]: options[kValidateChunk],
		[kDestroyOnSyncError]: options[kDestroyOnSyncError],
	};

	const writable = isWritable(duplex)
		? newWritableStreamFromStreamWritable(duplex, writableOptions)
		: new WritableStream();

	if (!isWritable(duplex)) writable.close();

	const readable = isReadable(duplex)
		? newReadableStreamFromStreamReadable(duplex, readableOptions)
		: new ReadableStream({ type: readableOptions.type });

	if (!isReadable(duplex)) readable.cancel();

	return { writable, readable };
}

interface StreamDuplexOptions {
	allowHalfOpen?: boolean;
	decodeStrings?: boolean;
	encoding?: string;
	highWaterMark?: number;
	objectMode?: boolean;
	signal?: AbortSignal;
}

export function newStreamDuplexFromReadableWritablePair(
	pair: { readable?: ReadableStream; writable?: WritableStream } = kEmptyObject,
	options: StreamDuplexOptions = kEmptyObject,
): StreamLike {
	validateObject(pair, "pair");
	const { readable: readableStream, writable: writableStream } = pair;

	if (!isReadableStream(readableStream)) {
		throw new ERR_INVALID_ARG_TYPE("pair.readable", "ReadableStream", readableStream);
	}
	if (!isWritableStream(writableStream)) {
		throw new ERR_INVALID_ARG_TYPE("pair.writable", "WritableStream", writableStream);
	}

	validateObject(options, "options");
	const { allowHalfOpen = false, objectMode = false, encoding, decodeStrings = true, highWaterMark, signal } = options;

	validateBoolean(objectMode, "options.objectMode");
	if (encoding !== undefined && !Buffer.isEncoding(encoding))
		throw new ERR_INVALID_ARG_VALUE("options.encoding", encoding);

	const writer = writableStream!.getWriter() as unknown as StreamLike;
	const reader = readableStream!.getReader();
	let writableClosed = false;
	let readableClosed = false;
	let readRequest: ReadRequest | undefined;
	let pendingChunk: unknown;
	const writeTracker = createWriteTracker(() => duplex);

	function forwardChunk() {
		const chunk = pendingChunk;
		pendingChunk = undefined;
		duplex.push(chunk);
	}

	const duplex: StreamLike = new Duplex({
		allowHalfOpen,
		highWaterMark,
		objectMode,
		encoding,
		decodeStrings,
		signal,

		writev(chunks: WritevChunks, callback: Cb) {
			writeTracker.callback = callback;
			dispatchWritev(writer, chunks, writeTracker);
		},

		write(chunk: unknown, enc: string, callback: Cb) {
			if (typeof chunk === "string" && decodeStrings && !objectMode) {
				chunk = toUint8Chunk(chunk, enc);
			}

			writeTracker.callback = callback;
			dispatchWrite(writer, chunk, writeTracker);
		},

		final(callback: Cb) {
			function done(error?: unknown) {
				try {
					callback(error);
				} catch (err) {
					// Escape the promise context so the error is not an unhandled rejection.
					process.nextTick(() => destroy(duplex, err));
				}
			}

			if (!writableClosed) {
				(writer.close as () => Promise<void>)().then(done, done);
			}
		},

		read() {
			// Same single-in-flight reused read request as newStreamReadableFromReadableStream().
			readRequest ??= {
				[kChunk](chunk: unknown) {
					pendingChunk = chunk;
					kResolvedPromise.then(forwardChunk);
				},
				[kClose]() {
					duplex.push(null);
				},
				[kError](error: unknown) {
					destroy(duplex, error);
				},
			};
			readableStreamDefaultReaderRead(reader, readRequest);
		},

		destroy(error: unknown, callback: Cb) {
			function done() {
				try {
					callback(error);
				} catch (err) {
					// Escape the promise context so the error is not an unhandled rejection.
					process.nextTick(() => {
						throw err;
					});
				}
			}

			async function closeWriter() {
				if (!writableClosed) await (writer.abort as (e: unknown) => Promise<void>)(error);
			}

			async function closeReader() {
				if (!readableClosed) await reader.cancel(error);
			}

			if (!writableClosed || !readableClosed) {
				Promise.all([closeWriter(), closeReader()]).then(done, done);
				return;
			}

			done();
		},
	});

	(writer.closed as Promise<void>).then(
		() => {
			writableClosed = true;
			if (!isWritableEnded(duplex)) destroy(duplex, new ERR_STREAM_PREMATURE_CLOSE());
		},
		(error: unknown) => {
			writableClosed = true;
			readableClosed = true;
			destroy(duplex, error);
		},
	);

	reader.closed.then(
		() => {
			readableClosed = true;
		},
		(error: unknown) => {
			writableClosed = true;
			readableClosed = true;
			destroy(duplex, error);
		},
	);

	return duplex;
}
