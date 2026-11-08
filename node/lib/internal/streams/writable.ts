// Writable streams (Node's lib/internal/streams/writable.js).
//
// Copyright Joyent, Inc. and other Node contributors. MIT license.
//
// A bit simpler than readable streams. Implement an async
// ._write(chunk, encoding, cb), and it'll handle all the drain event
// emission and buffering.

import EE from "node:events";
import type * as adaptersModule from "internal:webstreams/adapters";
import { Stream } from "internal:streams/legacy";
import * as destroyImpl from "internal:streams/destroy";
import { eos } from "internal:streams/end-of-stream";
import { addAbortSignal } from "internal:streams/add-abort-signal";
import { getDefaultHighWaterMark, getHighWaterMark } from "internal:streams/state";
import {
	kAutoDestroy,
	kCloseEmitted,
	kClosed,
	kConstructed,
	kDestroyed,
	kEmitClose,
	kErrorEmitted,
	kErrored,
	kObjectMode,
	kOnConstructed,
	kState,
} from "internal:streams/utils";
import {
	AbortError,
	ERR_INVALID_ARG_TYPE,
	ERR_METHOD_NOT_IMPLEMENTED,
	ERR_MULTIPLE_CALLBACK,
	ERR_STREAM_ALREADY_FINISHED,
	ERR_STREAM_CANNOT_PIPE,
	ERR_STREAM_DESTROYED,
	ERR_STREAM_NULL_VALUES,
	ERR_STREAM_WRITE_AFTER_END,
	ERR_UNKNOWN_ENCODING,
	nextTick,
} from "internal:streams/shared";

type Callback = (err?: unknown) => void;
type Listener = (...args: unknown[]) => void;
type BufferCtor = { new (buffer?: ArrayBufferLike, byteOffset?: number, length?: number): Buffer };

const FastBuffer = (Buffer as unknown as { [Symbol.species]: BufferCtor })[Symbol.species];

const { errorOrDestroy } = destroyImpl;

function nop(): void {}

const kOnFinishedValue = Symbol("kOnFinishedValue");
const kErroredValue = Symbol("kErroredValue");
const kDefaultEncodingValue = Symbol("kDefaultEncodingValue");
const kWriteCbValue = Symbol("kWriteCbValue");
const kAfterWriteTickInfoValue = Symbol("kAfterWriteTickInfoValue");
const kBufferedValue = Symbol("kBufferedValue");

const kSync = 1 << 9;
const kFinalCalled = 1 << 10;
const kNeedDrain = 1 << 11;
const kEnding = 1 << 12;
const kFinished = 1 << 13;
const kDecodeStrings = 1 << 14;
const kWriting = 1 << 15;
const kBufferProcessing = 1 << 16;
const kPrefinished = 1 << 17;
const kAllBuffers = 1 << 18;
const kAllNoop = 1 << 19;
const kOnFinished = 1 << 20;
const kHasWritable = 1 << 21;
const kWritable = 1 << 22;
const kCorked = 1 << 23;
const kDefaultUTF8Encoding = 1 << 24;
const kWriteCb = 1 << 25;
const kExpectWriteCb = 1 << 26;
const kAfterWriteTickInfo = 1 << 27;
const kAfterWritePending = 1 << 28;
const kBuffered = 1 << 29;
const kEnded = 1 << 30;

interface BufferedRequest {
	chunk: unknown;
	encoding: string;
	callback: Callback;
}

interface AfterWriteTickInfo {
	count: number;
	cb: Callback;
	stream: WritableImpl;
	state: WritableState;
}

export interface WritableState {
	[kState]: number;
	[kErroredValue]?: unknown;
	[kDefaultEncodingValue]?: string;
	[kWriteCbValue]?: Callback | null;
	[kAfterWriteTickInfoValue]?: AfterWriteTickInfo | null;
	[kBufferedValue]: BufferedRequest[] | null;
	[kOnFinishedValue]?: Callback[] | null;
	[kOnConstructed](stream: WritableImpl): void;
	highWaterMark: number;
	length: number;
	corked: number;
	onwrite: (er?: unknown) => void;
	writelen: number;
	bufferedIndex: number;
	pendingcb: number;
	getBuffer(): BufferedRequest[];
	readonly bufferedRequestCount: number;
	// accessors over [kState]
	objectMode: boolean;
	finalCalled: boolean;
	needDrain: boolean;
	ending: boolean;
	ended: boolean;
	finished: boolean;
	destroyed: boolean;
	decodeStrings: boolean;
	writing: boolean;
	sync: boolean;
	bufferProcessing: boolean;
	constructed: boolean;
	prefinished: boolean;
	errorEmitted: boolean;
	emitClose: boolean;
	autoDestroy: boolean;
	closed: boolean;
	closeEmitted: boolean;
	allBuffers: boolean;
	allNoop: boolean;
	errored: unknown;
	writable: boolean | undefined;
	defaultEncoding: string;
	writecb: Callback | null;
	afterWriteTickInfo: AfterWriteTickInfo | null;
	buffered: BufferedRequest[];
}

export interface WritableOptions {
	highWaterMark?: number;
	writableHighWaterMark?: number;
	objectMode?: boolean;
	writableObjectMode?: boolean;
	decodeStrings?: boolean;
	defaultEncoding?: string | null;
	emitClose?: boolean;
	autoDestroy?: boolean;
	signal?: AbortSignal;
	write?(this: WritableImpl, chunk: unknown, encoding: string, cb: Callback): void;
	writev?(this: WritableImpl, chunks: { chunk: unknown; encoding: string }[], cb: Callback): void;
	destroy?(this: WritableImpl, err: Error | null, cb: Callback): void;
	final?(this: WritableImpl, cb: Callback): void;
	construct?(this: WritableImpl, cb: Callback): void;
	captureRejections?: boolean;
	[key: string]: unknown;
}

/** A Writable instance (also the writable half of a Duplex). */
export interface WritableImpl extends EE {
	_writableState: WritableState;
	_readableState?: { errored?: unknown; autoDestroy?: boolean; endEmitted?: boolean; readable?: boolean } | null;
	_write(chunk: unknown, encoding: string, cb: Callback): void;
	_writev: ((chunks: { chunk: unknown; encoding: string }[] & { allBuffers?: boolean }, cb: Callback) => void) | null;
	_final?: (cb: Callback) => void;
	_destroy(err: Error | null, cb: Callback): void;
	_construct?: (cb: Callback) => void;
	write(chunk: unknown, encoding?: string | Callback | null, cb?: Callback): boolean;
	end(chunk?: unknown, encoding?: string | Callback | null, cb?: Callback): this;
	uncork(): void;
	destroy(err?: unknown, cb?: Callback): this;
	readonly destroyed: boolean;
	readonly writableFinished: boolean;
	[key: string | symbol]: unknown;
}

function makeBitMapDescriptor(bit: number): PropertyDescriptor {
	return {
		enumerable: false,
		get(this: WritableState) {
			return (this[kState] & bit) !== 0;
		},
		set(this: WritableState, value: unknown) {
			if (value) this[kState] |= bit;
			else this[kState] &= ~bit;
		},
	};
}

export const WritableState = function WritableState(
	this: WritableState,
	options: WritableOptions | undefined | null,
	stream: WritableImpl,
	isDuplex?: boolean,
): void {
	// Bit map field to store WritableState more efficiently with 1 bit per field.
	this[kState] = kSync | kConstructed | kEmitClose | kAutoDestroy;

	if (options?.objectMode) this[kState] |= kObjectMode;
	if (isDuplex && options?.writableObjectMode) this[kState] |= kObjectMode;

	// The point at which write() starts returning false.
	// Note: 0 is a valid value, means that we always return false if
	// the entire buffer is not flushed immediately on write().
	this.highWaterMark = options
		? getHighWaterMark(this, options, "writableHighWaterMark", isDuplex)
		: getDefaultHighWaterMark(false);

	if (!options || options.decodeStrings !== false) this[kState] |= kDecodeStrings;

	// Should close be emitted on destroy. Defaults to true.
	if (options && options.emitClose === false) this[kState] &= ~kEmitClose;

	// Should .destroy() be called after 'end' (and potentially 'finish').
	if (options && options.autoDestroy === false) this[kState] &= ~kAutoDestroy;

	// Crypto is kind of old and crusty. Historically, its default string
	// encoding is 'binary' so we have to make this configurable.
	const defaultEncoding = options ? options.defaultEncoding : null;
	if (defaultEncoding == null || defaultEncoding === "utf8" || defaultEncoding === "utf-8") {
		this[kState] |= kDefaultUTF8Encoding;
	} else if (Buffer.isEncoding(defaultEncoding)) {
		this[kState] &= ~kDefaultUTF8Encoding;
		this[kDefaultEncodingValue] = defaultEncoding;
	} else {
		throw new ERR_UNKNOWN_ENCODING(defaultEncoding);
	}

	// Not an actual buffer we keep track of, but a measurement of how much
	// we're waiting to get pushed to some underlying socket or file.
	this.length = 0;

	// When true all writes will be buffered until .uncork() call.
	this.corked = 0;

	// The callback that's passed to _write(chunk, cb).
	this.onwrite = (er?: unknown) => onwrite(stream, er);

	// The amount that is being written when _write is called.
	this.writelen = 0;

	resetBuffer(this);

	// Number of pending user-supplied write callbacks;
	// this must be 0 before 'finish' can be emitted.
	this.pendingcb = 0;
} as unknown as {
	new (options: WritableOptions | undefined | null, stream: unknown, isDuplex?: boolean): WritableState;
	prototype: WritableState;
};

Object.defineProperties(WritableState.prototype, {
	// Object stream flag to indicate whether or not this stream contains buffers or objects.
	objectMode: makeBitMapDescriptor(kObjectMode),
	// if _final has been called.
	finalCalled: makeBitMapDescriptor(kFinalCalled),
	// drain event flag.
	needDrain: makeBitMapDescriptor(kNeedDrain),
	// At the start of calling end()
	ending: makeBitMapDescriptor(kEnding),
	// When end() has been called, and returned.
	ended: makeBitMapDescriptor(kEnded),
	// When 'finish' is emitted.
	finished: makeBitMapDescriptor(kFinished),
	// Has it been destroyed.
	destroyed: makeBitMapDescriptor(kDestroyed),
	// Should we decode strings into buffers before passing to _write?
	decodeStrings: makeBitMapDescriptor(kDecodeStrings),
	// A flag to see when we're in the middle of a write.
	writing: makeBitMapDescriptor(kWriting),
	// A flag to be able to tell if the onwrite cb is called immediately, or on a later tick.
	sync: makeBitMapDescriptor(kSync),
	// A flag to know if we're processing previously buffered items.
	bufferProcessing: makeBitMapDescriptor(kBufferProcessing),
	// Stream is still being constructed and cannot be destroyed until
	// construction finished or failed.
	constructed: makeBitMapDescriptor(kConstructed),
	// Emit prefinish if the only thing we're waiting for is _write cbs.
	prefinished: makeBitMapDescriptor(kPrefinished),
	// True if the error was already emitted and should not be thrown again.
	errorEmitted: makeBitMapDescriptor(kErrorEmitted),
	// Should close be emitted on destroy. Defaults to true.
	emitClose: makeBitMapDescriptor(kEmitClose),
	// Should .destroy() be called after 'finish' (and potentially 'end').
	autoDestroy: makeBitMapDescriptor(kAutoDestroy),
	// Indicates whether the stream has finished destroying.
	closed: makeBitMapDescriptor(kClosed),
	// True if close has been emitted or would have been emitted depending on emitClose.
	closeEmitted: makeBitMapDescriptor(kCloseEmitted),
	allBuffers: makeBitMapDescriptor(kAllBuffers),
	allNoop: makeBitMapDescriptor(kAllNoop),

	// Indicates whether the stream has errored. When true all write() calls
	// should return false.
	errored: {
		enumerable: false,
		get(this: WritableState) {
			return (this[kState] & kErrored) !== 0 ? this[kErroredValue] : null;
		},
		set(this: WritableState, value: unknown) {
			if (value) {
				this[kErroredValue] = value;
				this[kState] |= kErrored;
			} else {
				this[kState] &= ~kErrored;
			}
		},
	},

	writable: {
		enumerable: false,
		get(this: WritableState) {
			return (this[kState] & kHasWritable) !== 0 ? (this[kState] & kWritable) !== 0 : undefined;
		},
		set(this: WritableState, value: boolean | null | undefined) {
			if (value == null) {
				this[kState] &= ~(kHasWritable | kWritable);
			} else if (value) {
				this[kState] |= kHasWritable | kWritable;
			} else {
				this[kState] |= kHasWritable;
				this[kState] &= ~kWritable;
			}
		},
	},

	defaultEncoding: {
		enumerable: false,
		get(this: WritableState) {
			return (this[kState] & kDefaultUTF8Encoding) !== 0 ? "utf8" : this[kDefaultEncodingValue];
		},
		set(this: WritableState, value: string) {
			if (value === "utf8" || value === "utf-8") {
				this[kState] |= kDefaultUTF8Encoding;
			} else {
				this[kState] &= ~kDefaultUTF8Encoding;
				this[kDefaultEncodingValue] = value;
			}
		},
	},

	// The callback that the user supplies to write(chunk, encoding, cb).
	writecb: {
		enumerable: false,
		get(this: WritableState) {
			return (this[kState] & kWriteCb) !== 0 ? this[kWriteCbValue] : nop;
		},
		set(this: WritableState, value: Callback | null) {
			this[kWriteCbValue] = value;
			if (value) this[kState] |= kWriteCb;
			else this[kState] &= ~kWriteCb;
		},
	},

	// Storage for data passed to the afterWrite() callback in case of
	// synchronous _write() completion.
	afterWriteTickInfo: {
		enumerable: false,
		get(this: WritableState) {
			return (this[kState] & kAfterWriteTickInfo) !== 0 ? this[kAfterWriteTickInfoValue] : null;
		},
		set(this: WritableState, value: AfterWriteTickInfo | null) {
			this[kAfterWriteTickInfoValue] = value;
			if (value) this[kState] |= kAfterWriteTickInfo;
			else this[kState] &= ~kAfterWriteTickInfo;
		},
	},

	buffered: {
		enumerable: false,
		get(this: WritableState) {
			return (this[kState] & kBuffered) !== 0 ? this[kBufferedValue] : [];
		},
		set(this: WritableState, value: BufferedRequest[] | null) {
			this[kBufferedValue] = value;
			if (value) this[kState] |= kBuffered;
			else this[kState] &= ~kBuffered;
		},
	},

	bufferedRequestCount: {
		get(this: WritableState) {
			return (this[kState] & kBuffered) === 0
				? 0
				: (this[kBufferedValue] as BufferedRequest[]).length - this.bufferedIndex;
		},
	},
});

function resetBuffer(state: WritableState): void {
	state[kBufferedValue] = null;
	state.bufferedIndex = 0;
	state[kState] |= kAllBuffers | kAllNoop;
	state[kState] &= ~kBuffered;
}

WritableState.prototype.getBuffer = function getBuffer(this: WritableState): BufferedRequest[] {
	return (this[kState] & kBuffered) === 0 ? [] : this.buffered.slice(this.bufferedIndex);
};

WritableState.prototype[kOnConstructed] = function onConstructed(this: WritableState, stream: WritableImpl) {
	if ((this[kState] & kWriting) === 0) clearBuffer(stream, this);
	if ((this[kState] & kEnding) !== 0) finishMaybe(stream, this);
};

export interface WritableConstructor {
	new (options?: WritableOptions): WritableImpl;
	(this: unknown, options?: WritableOptions): void;
	prototype: WritableImpl;
	WritableState: typeof WritableState;
	fromWeb(writableStream: unknown, options?: unknown): WritableImpl;
	toWeb(streamWritable: unknown): unknown;
}

export const Writable = function Writable(this: WritableImpl, options?: WritableOptions): WritableImpl | void {
	if (!(this instanceof Writable)) return new Writable(options);

	this._writableState = new WritableState(options, this, false);

	if (options) {
		if (typeof options.write === "function") this._write = options.write;
		if (typeof options.writev === "function") this._writev = options.writev;
		if (typeof options.destroy === "function") this._destroy = options.destroy;
		if (typeof options.final === "function") this._final = options.final;
		if (typeof options.construct === "function") this._construct = options.construct;
		if (options.signal) addAbortSignal(options.signal, this);
	}

	Stream.call(this, options);

	if (this._construct != null) {
		destroyImpl.construct(this as never, () => {
			this._writableState[kOnConstructed](this);
		});
	}
} as unknown as WritableConstructor;

Object.setPrototypeOf(Writable.prototype, Stream.prototype);
Object.setPrototypeOf(Writable, Stream);

Writable.WritableState = WritableState;

const functionHasInstance = Function.prototype[Symbol.hasInstance];

Object.defineProperty(Writable, Symbol.hasInstance, {
	value: {
		[Symbol.hasInstance](this: unknown, instance: unknown): boolean {
			if (functionHasInstance.call(this, instance)) return true;
			if (this !== Writable) return false;
			return !!instance && (instance as { _writableState?: unknown })._writableState instanceof WritableState;
		},
	}[Symbol.hasInstance],
});

const proto = Writable.prototype as unknown as Record<string | symbol, unknown>;

// Otherwise people can pipe Writable streams, which is just wrong.
proto.pipe = function pipe(this: WritableImpl): void {
	errorOrDestroy(this as never, new ERR_STREAM_CANNOT_PIPE());
};

function _write(
	stream: WritableImpl,
	chunk: unknown,
	encoding: string | null | undefined,
	cb?: Callback | null,
): boolean | Error {
	const state = stream._writableState;

	if (cb == null || typeof cb !== "function") cb = nop;

	if (chunk === null) throw new ERR_STREAM_NULL_VALUES();

	if ((state[kState] & kObjectMode) === 0) {
		if (!encoding) {
			encoding = (state[kState] & kDefaultUTF8Encoding) !== 0 ? "utf8" : state.defaultEncoding;
		} else if (encoding !== "buffer" && !Buffer.isEncoding(encoding)) {
			throw new ERR_UNKNOWN_ENCODING(encoding);
		}

		if (typeof chunk === "string") {
			if (encoding === "buffer") throw new ERR_UNKNOWN_ENCODING(encoding);
			if ((state[kState] & kDecodeStrings) !== 0) {
				chunk = Buffer.from(chunk, encoding as BufferEncoding);
				encoding = "buffer";
			}
		} else if (chunk instanceof Buffer) {
			encoding = "buffer";
		} else if (ArrayBuffer.isView(chunk)) {
			chunk = new FastBuffer(chunk.buffer, chunk.byteOffset, chunk.byteLength);
			encoding = "buffer";
		} else {
			throw new ERR_INVALID_ARG_TYPE("chunk", ["string", "Buffer", "TypedArray", "DataView"], chunk);
		}
	}

	let err: Error | undefined;
	if ((state[kState] & kEnding) !== 0) err = new ERR_STREAM_WRITE_AFTER_END();
	else if ((state[kState] & kDestroyed) !== 0) err = new ERR_STREAM_DESTROYED("write");

	if (err) {
		nextTick(cb as Listener, err);
		errorOrDestroy(stream as never, err, true);
		return err;
	}

	state.pendingcb++;
	return writeOrBuffer(stream, state, chunk, encoding as string, cb);
}

proto.write = function write(
	this: WritableImpl,
	chunk: unknown,
	encoding?: string | Callback | null,
	cb?: Callback,
): boolean {
	if (encoding != null && typeof encoding === "function") {
		cb = encoding;
		encoding = null;
	}
	return _write(this, chunk, encoding as string | null | undefined, cb) === true;
};

proto.cork = function cork(this: WritableImpl): void {
	const state = this._writableState;
	state[kState] |= kCorked;
	state.corked++;
};

proto.uncork = function uncork(this: WritableImpl): void {
	const state = this._writableState;
	if (state.corked) {
		state.corked--;
		if (!state.corked) state[kState] &= ~kCorked;
		if ((state[kState] & kWriting) === 0) clearBuffer(this, state);
	}
};

proto.setDefaultEncoding = function setDefaultEncoding(this: WritableImpl, encoding: unknown): WritableImpl {
	// node::ParseEncoding() requires lower case.
	if (typeof encoding === "string") encoding = encoding.toLowerCase();
	if (!Buffer.isEncoding(encoding as string)) throw new ERR_UNKNOWN_ENCODING(encoding);
	this._writableState.defaultEncoding = encoding as string;
	return this;
};

// If we're already writing something, then just put this in the queue, and
// wait our turn. Otherwise, call _write. If we return false, then we need a
// drain event, so set that flag.
function writeOrBuffer(
	stream: WritableImpl,
	state: WritableState,
	chunk: unknown,
	encoding: string,
	callback: Callback,
): boolean {
	const len = (state[kState] & kObjectMode) !== 0 ? 1 : (chunk as Buffer | string).length;

	state.length += len;

	if ((state[kState] & (kWriting | kErrored | kCorked | kConstructed)) !== kConstructed) {
		if ((state[kState] & kBuffered) === 0) {
			state[kState] |= kBuffered;
			state[kBufferedValue] = [];
		}

		(state[kBufferedValue] as BufferedRequest[]).push({ chunk, encoding, callback });
		if ((state[kState] & kAllBuffers) !== 0 && encoding !== "buffer") state[kState] &= ~kAllBuffers;
		if ((state[kState] & kAllNoop) !== 0 && callback !== nop) state[kState] &= ~kAllNoop;
	} else {
		state.writelen = len;
		if (callback !== nop) state.writecb = callback;
		state[kState] |= kWriting | kSync | kExpectWriteCb;
		stream._write(chunk, encoding, state.onwrite);
		state[kState] &= ~kSync;
	}

	const ret = state.length < state.highWaterMark || state.length === 0;

	if (!ret) state[kState] |= kNeedDrain;

	// Return false if errored or destroyed in order to break
	// any synchronous while(stream.write(data)) loops.
	return ret && (state[kState] & (kDestroyed | kErrored)) === 0;
}

function doWrite(
	stream: WritableImpl,
	state: WritableState,
	writev: boolean,
	len: number,
	chunk: unknown,
	encoding: string,
	cb: Callback,
): void {
	state.writelen = len;
	if (cb !== nop) state.writecb = cb;
	state[kState] |= kWriting | kSync | kExpectWriteCb;
	if ((state[kState] & kDestroyed) !== 0) state.onwrite(new ERR_STREAM_DESTROYED("write"));
	else if (writev) (stream._writev as NonNullable<WritableImpl["_writev"]>)(chunk as BufferedRequest[], state.onwrite);
	else stream._write(chunk, encoding, state.onwrite);
	state[kState] &= ~kSync;
}

function onwriteError(stream: WritableImpl, state: WritableState, er: unknown, cb: Callback): void {
	--state.pendingcb;

	cb(er);
	// Ensure callbacks are invoked even when autoDestroy is not enabled.
	// Passing `er` here doesn't make sense since it's related to one
	// specific write, not to the buffered writes.
	errorBuffer(state);
	// This can emit error, but error must always follow cb.
	errorOrDestroy(stream as never, er);
}

function onwrite(stream: WritableImpl, er?: unknown): void {
	const state = stream._writableState;

	if ((state[kState] & kExpectWriteCb) === 0) {
		errorOrDestroy(stream as never, new ERR_MULTIPLE_CALLBACK());
		return;
	}

	const sync = (state[kState] & kSync) !== 0;
	const cb = (state[kState] & kWriteCb) !== 0 ? (state[kWriteCbValue] as Callback) : nop;

	state.writecb = null;
	state[kState] &= ~(kWriting | kExpectWriteCb);
	state.length -= state.writelen;
	state.writelen = 0;

	if (er) {
		if ((state[kState] & kErrored) === 0) {
			state[kErroredValue] = er;
			state[kState] |= kErrored;
		}

		// In case of duplex streams we need to notify the readable side of the error.
		if (stream._readableState && !stream._readableState.errored) stream._readableState.errored = er;

		if (sync) nextTick(onwriteError as Listener, stream, state, er, cb);
		else onwriteError(stream, state, er, cb);
	} else {
		if ((state[kState] & kBuffered) !== 0) clearBuffer(stream, state);

		if (sync) {
			const needDrain = (state[kState] & kNeedDrain) !== 0 && state.length === 0;
			// Node evaluates `state[kState] & kDestroyed !== 0` here, which by
			// operator precedence tests bit 0 (kObjectMode); kept for parity.
			const needTick = needDrain || (state[kState] & kObjectMode) !== 0 || cb !== nop;

			// It is a common case that the callback passed to .write() is always
			// the same. In that case, we do not schedule a new nextTick(), but
			// rather just increase a counter.
			if (cb === nop) {
				if ((state[kState] & kAfterWritePending) === 0 && needTick) {
					nextTick(afterWrite as Listener, stream, state, 1, cb);
					state[kState] |= kAfterWritePending;
				} else {
					state.pendingcb--;
					if ((state[kState] & kEnding) !== 0) finishMaybe(stream, state, true);
				}
			} else if (
				(state[kState] & kAfterWriteTickInfo) !== 0 &&
				(state[kAfterWriteTickInfoValue] as AfterWriteTickInfo).cb === cb
			) {
				(state[kAfterWriteTickInfoValue] as AfterWriteTickInfo).count++;
			} else if (needTick) {
				state[kAfterWriteTickInfoValue] = { count: 1, cb, stream, state };
				nextTick(afterWriteTick as Listener, state[kAfterWriteTickInfoValue]);
				state[kState] |= kAfterWritePending | kAfterWriteTickInfo;
			} else {
				state.pendingcb--;
				if ((state[kState] & kEnding) !== 0) finishMaybe(stream, state, true);
			}
		} else {
			afterWrite(stream, state, 1, cb);
		}
	}
}

function afterWriteTick({ stream, state, count, cb }: AfterWriteTickInfo): void {
	state[kState] &= ~kAfterWriteTickInfo;
	state[kAfterWriteTickInfoValue] = null;
	afterWrite(stream, state, count, cb);
}

function afterWrite(stream: WritableImpl, state: WritableState, count: number, cb: Callback): void {
	state[kState] &= ~kAfterWritePending;

	const needDrain = (state[kState] & (kEnding | kNeedDrain | kDestroyed)) === kNeedDrain && state.length === 0;
	if (needDrain) {
		state[kState] &= ~kNeedDrain;
		stream.emit("drain");
	}

	while (count-- > 0) {
		state.pendingcb--;
		cb(null);
	}

	if ((state[kState] & kDestroyed) !== 0) errorBuffer(state);

	if ((state[kState] & kEnding) !== 0) finishMaybe(stream, state, true);
}

// If there's something in the buffer waiting, then invoke callbacks.
function errorBuffer(state: WritableState): void {
	if ((state[kState] & kWriting) !== 0) return;

	if ((state[kState] & kBuffered) !== 0) {
		const buffered = state[kBufferedValue] as BufferedRequest[];
		for (let n = state.bufferedIndex; n < buffered.length; ++n) {
			const { chunk, callback } = buffered[n];
			const len = (state[kState] & kObjectMode) !== 0 ? 1 : (chunk as Buffer | string).length;
			state.length -= len;
			callback(state.errored ?? new ERR_STREAM_DESTROYED("write"));
		}
	}

	callFinishedCallbacks(state, state.errored ?? new ERR_STREAM_DESTROYED("end"));

	resetBuffer(state);
}

// If there's something in the buffer waiting, then process it.
function clearBuffer(stream: WritableImpl, state: WritableState): void {
	if (
		(state[kState] & (kDestroyed | kBufferProcessing | kCorked | kBuffered | kConstructed)) !==
		(kBuffered | kConstructed)
	) {
		return;
	}

	const objectMode = (state[kState] & kObjectMode) !== 0;
	const buffered = state[kBufferedValue] as (BufferedRequest | null)[];
	const bufferedIndex = state.bufferedIndex;
	const bufferedLength = buffered.length - bufferedIndex;

	if (!bufferedLength) return;

	let i = bufferedIndex;

	state[kState] |= kBufferProcessing;
	if (bufferedLength > 1 && stream._writev) {
		state.pendingcb -= bufferedLength - 1;

		const callback =
			(state[kState] & kAllNoop) !== 0
				? nop
				: (err?: unknown) => {
						for (let n = i; n < buffered.length; ++n) (buffered[n] as BufferedRequest).callback(err);
					};
		// Make a copy of `buffered` if it's going to be used by `callback` above,
		// since `doWrite` will mutate the array.
		const chunks = ((state[kState] & kAllNoop) !== 0 && i === 0 ? buffered : buffered.slice(i)) as BufferedRequest[] & {
			allBuffers?: boolean;
		};
		chunks.allBuffers = (state[kState] & kAllBuffers) !== 0;

		doWrite(stream, state, true, state.length, chunks, "", callback);

		resetBuffer(state);
	} else {
		do {
			const { chunk, encoding, callback } = buffered[i] as BufferedRequest;
			buffered[i++] = null;
			const len = objectMode ? 1 : (chunk as Buffer | string).length;
			doWrite(stream, state, false, len, chunk, encoding, callback);
		} while (i < buffered.length && (state[kState] & kWriting) === 0);

		if (i === buffered.length) {
			resetBuffer(state);
		} else if (i > 256 && i * 2 >= buffered.length) {
			buffered.splice(0, i);
			state.bufferedIndex = 0;
		} else {
			state.bufferedIndex = i;
		}
	}
	state[kState] &= ~kBufferProcessing;
}

proto._write = function _write(this: WritableImpl, chunk: unknown, encoding: string, cb: Callback): void {
	if (this._writev) this._writev([{ chunk, encoding }], cb);
	else throw new ERR_METHOD_NOT_IMPLEMENTED("_write()");
};

proto._writev = null;

proto.end = function end(
	this: WritableImpl,
	chunk?: unknown,
	encoding?: string | Callback | null,
	cb?: Callback,
): WritableImpl {
	const state = this._writableState;

	if (typeof chunk === "function") {
		cb = chunk as Callback;
		chunk = null;
		encoding = null;
	} else if (typeof encoding === "function") {
		cb = encoding;
		encoding = null;
	}

	let err: unknown;

	if (chunk != null) {
		const ret = _write(this, chunk, encoding as string | null);
		if (ret instanceof Error) err = ret;
	}

	// .end() fully uncorks.
	if ((state[kState] & kCorked) !== 0) {
		state.corked = 1;
		this.uncork();
	}

	if (err) {
		// Do nothing...
	} else if ((state[kState] & (kEnding | kErrored)) === 0) {
		// This is forgiving in terms of unnecessary calls to end() and can hide
		// logic errors. However, usually such errors are harmless and causing a
		// hard error can be disproportionately destructive.
		state[kState] |= kEnding;
		finishMaybe(this, state, true);
		state[kState] |= kEnded;
	} else if ((state[kState] & kFinished) !== 0) {
		err = new ERR_STREAM_ALREADY_FINISHED("end");
	} else if ((state[kState] & kDestroyed) !== 0) {
		err = new ERR_STREAM_DESTROYED("end");
	}

	if (typeof cb === "function") {
		if (err) {
			nextTick(cb as Listener, err);
		} else if ((state[kState] & kErrored) !== 0) {
			nextTick(cb as Listener, state[kErroredValue]);
		} else if ((state[kState] & kFinished) !== 0) {
			nextTick(cb as Listener, null);
		} else {
			state[kState] |= kOnFinished;
			state[kOnFinishedValue] ??= [];
			state[kOnFinishedValue].push(cb);
		}
	}

	return this;
};

function needFinish(state: WritableState): boolean {
	// State is ended && constructed but not destroyed, finished, writing, errorEmitted or closedEmitted
	return (
		(state[kState] &
			(kEnding |
				kDestroyed |
				kConstructed |
				kFinished |
				kWriting |
				kErrorEmitted |
				kCloseEmitted |
				kErrored |
				kBuffered)) ===
			(kEnding | kConstructed) && state.length === 0
	);
}

function onFinish(stream: WritableImpl, state: WritableState, err?: unknown): void {
	if ((state[kState] & kPrefinished) !== 0) {
		errorOrDestroy(stream as never, err ?? new ERR_MULTIPLE_CALLBACK());
		return;
	}
	state.pendingcb--;
	if (err) {
		callFinishedCallbacks(state, err);
		errorOrDestroy(stream as never, err, (state[kState] & kSync) !== 0);
	} else if (needFinish(state)) {
		state[kState] |= kPrefinished;
		stream.emit("prefinish");
		// Backwards compat. Don't check state.sync here. Some streams assume
		// 'finish' will be emitted asynchronously relative to _final callback.
		state.pendingcb++;
		nextTick(finish as Listener, stream, state);
	}
}

function prefinish(stream: WritableImpl, state: WritableState): void {
	if ((state[kState] & (kPrefinished | kFinalCalled)) !== 0) return;

	if (typeof stream._final === "function" && (state[kState] & kDestroyed) === 0) {
		state[kState] |= kFinalCalled | kSync;
		state.pendingcb++;

		try {
			stream._final((err?: unknown) => onFinish(stream, state, err));
		} catch (err) {
			onFinish(stream, state, err);
		}

		state[kState] &= ~kSync;
	} else {
		state[kState] |= kFinalCalled | kPrefinished;
		stream.emit("prefinish");
	}
}

function finishMaybeNT(stream: WritableImpl, state: WritableState): void {
	if (needFinish(state)) finish(stream, state);
	else state.pendingcb--;
}

function finishMaybe(stream: WritableImpl, state: WritableState, sync?: boolean): void {
	if (needFinish(state)) {
		prefinish(stream, state);
		if (state.pendingcb === 0) {
			if (sync) {
				state.pendingcb++;
				nextTick(finishMaybeNT as Listener, stream, state);
			} else if (needFinish(state)) {
				state.pendingcb++;
				finish(stream, state);
			}
		}
	}
}

function finish(stream: WritableImpl, state: WritableState): void {
	state.pendingcb--;
	state[kState] |= kFinished;

	callFinishedCallbacks(state, null);

	stream.emit("finish");

	if ((state[kState] & kAutoDestroy) !== 0) {
		// In case of duplex streams we need a way to detect
		// if the readable side is ready for autoDestroy as well.
		const rState = stream._readableState;
		const autoDestroy =
			!rState ||
			(rState.autoDestroy &&
				// We don't expect the readable to ever 'end'
				// if readable is explicitly set to false.
				(rState.endEmitted || rState.readable === false));
		if (autoDestroy) stream.destroy();
	}
}

function callFinishedCallbacks(state: WritableState, err: unknown): void {
	if ((state[kState] & kOnFinished) === 0) return;

	const onfinishCallbacks = state[kOnFinishedValue] as Callback[];
	state[kOnFinishedValue] = null;
	state[kState] &= ~kOnFinished;
	for (let i = 0; i < onfinishCallbacks.length; i++) onfinishCallbacks[i](err);
}

Object.defineProperties(Writable.prototype, {
	closed: {
		get(this: WritableImpl) {
			return this._writableState ? (this._writableState[kState] & kClosed) !== 0 : false;
		},
	},

	destroyed: {
		get(this: WritableImpl) {
			return this._writableState ? (this._writableState[kState] & kDestroyed) !== 0 : false;
		},
		set(this: WritableImpl, value: boolean) {
			// Backward compatibility, the user is explicitly managing destroyed.
			if (!this._writableState) return;
			if (value) this._writableState[kState] |= kDestroyed;
			else this._writableState[kState] &= ~kDestroyed;
		},
	},

	writable: {
		get(this: WritableImpl) {
			const w = this._writableState;
			// w.writable === false means that this is part of a Duplex stream
			// where the writable side was disabled upon construction.
			return !!w && w.writable !== false && (w[kState] & (kEnding | kEnded | kDestroyed | kErrored)) === 0;
		},
		set(this: WritableImpl, val: unknown) {
			// Backwards compatible.
			if (this._writableState) this._writableState.writable = !!val;
		},
	},

	writableFinished: {
		get(this: WritableImpl) {
			const state = this._writableState;
			return state ? (state[kState] & kFinished) !== 0 : false;
		},
	},

	writableObjectMode: {
		get(this: WritableImpl) {
			const state = this._writableState;
			return state ? (state[kState] & kObjectMode) !== 0 : false;
		},
	},

	writableBuffer: {
		get(this: WritableImpl) {
			const state = this._writableState;
			return state && state.getBuffer();
		},
	},

	writableEnded: {
		get(this: WritableImpl) {
			const state = this._writableState;
			return state ? (state[kState] & kEnding) !== 0 : false;
		},
	},

	writableNeedDrain: {
		get(this: WritableImpl) {
			const state = this._writableState;
			return state ? (state[kState] & (kDestroyed | kEnding | kNeedDrain)) === kNeedDrain : false;
		},
	},

	writableHighWaterMark: {
		get(this: WritableImpl) {
			return this._writableState?.highWaterMark;
		},
	},

	writableCorked: {
		get(this: WritableImpl) {
			const state = this._writableState;
			return state ? state.corked : 0;
		},
	},

	writableLength: {
		get(this: WritableImpl) {
			return this._writableState?.length;
		},
	},

	errored: {
		enumerable: false,
		get(this: WritableImpl) {
			const state = this._writableState;
			return state ? state.errored : null;
		},
	},

	writableAborted: {
		get(this: WritableImpl) {
			const state = this._writableState;
			return (
				(state[kState] & (kHasWritable | kWritable)) !== kHasWritable &&
				(state[kState] & (kDestroyed | kErrored)) !== 0 &&
				(state[kState] & kFinished) === 0
			);
		},
	},
});

const destroy = destroyImpl.destroy;
proto.destroy = function (this: WritableImpl, err?: unknown, cb?: Callback): WritableImpl {
	const state = this._writableState;

	// Invoke pending callbacks.
	if ((state[kState] & (kBuffered | kOnFinished)) !== 0 && (state[kState] & kDestroyed) === 0) {
		nextTick(errorBuffer as Listener, state);
	}

	destroy.call(this as never, err, cb);
	return this;
};

proto._undestroy = destroyImpl.undestroy;
proto._destroy = function (this: WritableImpl, err: Error | null, cb: Callback): void {
	cb(err);
};

proto[(EE as unknown as { captureRejectionSymbol: symbol }).captureRejectionSymbol] = function (
	this: WritableImpl,
	err: unknown,
) {
	this.destroy(err);
};

Writable.fromWeb = function (writableStream: unknown, options?: unknown): WritableImpl {
	// adapters -> Duplex copies Writable.prototype during evaluation.
	const adapters = require("internal:webstreams/adapters") as typeof adaptersModule;
	return adapters.newStreamWritableFromWritableStream(writableStream, options) as WritableImpl;
};

Writable.toWeb = function (streamWritable: unknown): unknown {
	// adapters -> Duplex copies Writable.prototype during evaluation.
	const adapters = require("internal:webstreams/adapters") as typeof adaptersModule;
	return adapters.newWritableStreamFromStreamWritable(streamWritable);
};

proto[Symbol.asyncDispose] = async function (this: WritableImpl): Promise<void> {
	let error: unknown;
	if (!this.destroyed) {
		error = this.writableFinished ? null : new AbortError();
		this.destroy(error);
	}
	const { promise, resolve, reject } = Promise.withResolvers<null>();
	eos(this, (err) => (err && (err as Error).name !== "AbortError" ? reject(err) : resolve(null)));
	await promise;
};
