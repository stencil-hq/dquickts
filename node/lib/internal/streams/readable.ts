// Readable streams (Node's lib/internal/streams/readable.js).
//
// Copyright Joyent, Inc. and other Node contributors. MIT license.
//
// Ported without Node's flowSync()/fastChunk pipe fast path: flow() always
// drains through read(), which is the observable behavior of that path.

import EE from "node:events";
import { StringDecoder } from "node:string_decoder";
import type * as composeModule from "internal:streams/compose";
import type * as adaptersModule from "internal:webstreams/adapters";
import { Stream, prependListener } from "internal:streams/legacy";
import { addAbortSignal, addAbortSignalNoValidate } from "internal:streams/add-abort-signal";
import { eos } from "internal:streams/end-of-stream";
import * as destroyImpl from "internal:streams/destroy";
import { getDefaultHighWaterMark, getHighWaterMark } from "internal:streams/state";
import { from } from "internal:streams/from";
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
	aggregateTwoErrors,
	ERR_INVALID_ARG_TYPE,
	ERR_METHOD_NOT_IMPLEMENTED,
	ERR_OUT_OF_RANGE,
	ERR_STREAM_PUSH_AFTER_EOF,
	ERR_STREAM_UNSHIFT_AFTER_END_EVENT,
	ERR_UNKNOWN_ENCODING,
	nextTick,
	validateAbortSignal,
	validateObject,
} from "internal:streams/shared";

type Callback = (err?: unknown) => void;
type Listener = (...args: unknown[]) => void;
type Chunk = Buffer | string | unknown;
type BufferCtor = { new (buffer?: ArrayBufferLike, byteOffset?: number, length?: number): Buffer };

// Buffer[Symbol.species] is the plain Buffer subclass constructor (a view, no copy).
const FastBuffer = (Buffer as unknown as { [Symbol.species]: BufferCtor })[Symbol.species];

const { errorOrDestroy } = destroyImpl;

const nop = () => {};

const kErroredValue = Symbol("kErroredValue");
const kDefaultEncodingValue = Symbol("kDefaultEncodingValue");
const kDecoderValue = Symbol("kDecoderValue");
const kEncodingValue = Symbol("kEncodingValue");

const kEnded = 1 << 9;
const kEndEmitted = 1 << 10;
const kReading = 1 << 11;
const kSync = 1 << 12;
const kNeedReadable = 1 << 13;
const kEmittedReadable = 1 << 14;
const kReadableListening = 1 << 15;
const kResumeScheduled = 1 << 16;
const kMultiAwaitDrain = 1 << 17;
const kReadingMore = 1 << 18;
const kDataEmitted = 1 << 19;
const kDefaultUTF8Encoding = 1 << 20;
const kDecoder = 1 << 21;
const kEncoding = 1 << 22;
const kHasFlowing = 1 << 23;
const kFlowing = 1 << 24;
const kHasPaused = 1 << 25;
const kPaused = 1 << 26;
const kDataListening = 1 << 27;
const kEndScheduled = 1 << 28;
const kEofReadablePending = 1 << 29;

/** Writable side as seen by pipe(). */
interface PipeDest {
	write(chunk: unknown): boolean;
	end(): void;
	destroy(err?: unknown): void;
	on(ev: string, fn: Listener): unknown;
	once(ev: string, fn: Listener): unknown;
	emit(ev: string, ...args: unknown[]): boolean;
	removeListener(ev: string, fn: Listener): unknown;
	listenerCount(ev: string): number;
	writableNeedDrain?: boolean;
	_writableState?: { errorEmitted?: boolean } | null;
	_readableState?: { errorEmitted?: boolean } | null;
}

export interface ReadableState {
	[kState]: number;
	[kErroredValue]?: unknown;
	[kDefaultEncodingValue]?: string;
	[kDecoderValue]?: StringDecoder;
	[kEncodingValue]?: string;
	[kOnConstructed](stream: ReadableImpl): void;
	highWaterMark: number;
	buffer: Chunk[];
	bufferIndex: number;
	length: number;
	pipes: PipeDest[];
	awaitDrainWriters: PipeDest | Set<PipeDest> | null;
	readable?: boolean;
	// accessors over [kState]
	objectMode: boolean;
	ended: boolean;
	endEmitted: boolean;
	reading: boolean;
	constructed: boolean;
	sync: boolean;
	needReadable: boolean;
	emittedReadable: boolean;
	readableListening: boolean;
	resumeScheduled: boolean;
	errorEmitted: boolean;
	emitClose: boolean;
	autoDestroy: boolean;
	destroyed: boolean;
	closed: boolean;
	closeEmitted: boolean;
	multiAwaitDrain: boolean;
	readingMore: boolean;
	dataEmitted: boolean;
	errored: unknown;
	defaultEncoding: string;
	decoder: StringDecoder | null;
	encoding: string | null;
	flowing: boolean | null;
	paused: boolean;
	pipesCount: number;
}

export interface ReadableOptions {
	highWaterMark?: number;
	readableHighWaterMark?: number;
	encoding?: string | null;
	objectMode?: boolean;
	readableObjectMode?: boolean;
	emitClose?: boolean;
	autoDestroy?: boolean;
	defaultEncoding?: string | null;
	signal?: AbortSignal;
	read?(this: ReadableImpl, size: number): void;
	destroy?(this: ReadableImpl, err: Error | null, cb: Callback): void;
	construct?(this: ReadableImpl, cb: Callback): void;
	captureRejections?: boolean;
	[key: string]: unknown;
}

/** A Readable instance (also the readable half of a Duplex). */
export interface ReadableImpl extends EE {
	_readableState: ReadableState;
	_writableState?: { autoDestroy?: boolean; finished?: boolean; writable?: boolean } | null;
	_read(n: number): void;
	_destroy(err: Error | null, cb: Callback): void;
	_construct?: (cb: Callback) => void;
	push(chunk: unknown, encoding?: string): boolean;
	unshift(chunk: unknown, encoding?: string): boolean;
	read(n?: number): unknown;
	pause(): this;
	resume(): this;
	unpipe(dest?: PipeDest): this;
	destroy(err?: unknown, cb?: Callback): this;
	readonly destroyed: boolean;
	readonly readableEnded: boolean;
	writable?: boolean;
	writableEnded?: boolean;
	allowHalfOpen?: boolean;
	end?(): void;
	[key: string | symbol]: unknown;
}

function makeBitMapDescriptor(bit: number): PropertyDescriptor {
	return {
		enumerable: false,
		get(this: ReadableState) {
			return (this[kState] & bit) !== 0;
		},
		set(this: ReadableState, value: unknown) {
			if (value) this[kState] |= bit;
			else this[kState] &= ~bit;
		},
	};
}

export const ReadableState = function ReadableState(
	this: ReadableState,
	options: ReadableOptions | undefined | null,
	_stream: unknown,
	isDuplex?: boolean,
): void {
	// Bit map field to store ReadableState more efficiently with 1 bit per field.
	this[kState] = kEmitClose | kAutoDestroy | kConstructed | kSync;

	// Object stream flag. Used to make read(n) ignore n and to
	// make all the buffer merging and length checks go away.
	if (options?.objectMode) this[kState] |= kObjectMode;
	if (isDuplex && options?.readableObjectMode) this[kState] |= kObjectMode;

	// The point at which it stops calling _read() to fill the buffer.
	// Note: 0 is a valid value, means "don't call _read preemptively ever"
	this.highWaterMark = options
		? getHighWaterMark(this, options, "readableHighWaterMark", isDuplex)
		: getDefaultHighWaterMark(false);

	this.buffer = [];
	this.bufferIndex = 0;
	this.length = 0;
	this.pipes = [];

	// Should close be emitted on destroy. Defaults to true.
	if (options && options.emitClose === false) this[kState] &= ~kEmitClose;

	// Should .destroy() be called after 'end' (and potentially 'finish').
	if (options && options.autoDestroy === false) this[kState] &= ~kAutoDestroy;

	// Crypto is kind of old and crusty. Historically, its default string
	// encoding is 'binary' so we have to make this configurable.
	const defaultEncoding = options?.defaultEncoding;
	if (defaultEncoding == null || defaultEncoding === "utf8" || defaultEncoding === "utf-8") {
		this[kState] |= kDefaultUTF8Encoding;
	} else if (Buffer.isEncoding(defaultEncoding)) {
		this.defaultEncoding = defaultEncoding;
	} else {
		throw new ERR_UNKNOWN_ENCODING(defaultEncoding);
	}

	// Ref the piped dest which we need a drain event on it
	// type: null | Writable | Set<Writable>.
	this.awaitDrainWriters = null;

	if (options?.encoding) {
		this.decoder = new StringDecoder(options.encoding);
		this.encoding = options.encoding;
	}
} as unknown as {
	new (options: ReadableOptions | undefined | null, stream: unknown, isDuplex?: boolean): ReadableState;
	prototype: ReadableState;
};

Object.defineProperties(ReadableState.prototype, {
	objectMode: makeBitMapDescriptor(kObjectMode),
	ended: makeBitMapDescriptor(kEnded),
	endEmitted: makeBitMapDescriptor(kEndEmitted),
	reading: makeBitMapDescriptor(kReading),
	// Stream is still being constructed and cannot be destroyed until
	// construction finished or failed. Async construction is opt in,
	// therefore we start as constructed.
	constructed: makeBitMapDescriptor(kConstructed),
	// A flag to be able to tell if the event 'readable'/'data' is emitted
	// immediately, or on a later tick.
	sync: makeBitMapDescriptor(kSync),
	// Whenever we return null, then we set a flag to say
	// that we're awaiting a 'readable' event emission.
	needReadable: makeBitMapDescriptor(kNeedReadable),
	emittedReadable: makeBitMapDescriptor(kEmittedReadable),
	readableListening: makeBitMapDescriptor(kReadableListening),
	resumeScheduled: makeBitMapDescriptor(kResumeScheduled),
	// True if the error was already emitted and should not be thrown again.
	errorEmitted: makeBitMapDescriptor(kErrorEmitted),
	emitClose: makeBitMapDescriptor(kEmitClose),
	autoDestroy: makeBitMapDescriptor(kAutoDestroy),
	// Has it been destroyed.
	destroyed: makeBitMapDescriptor(kDestroyed),
	// Indicates whether the stream has finished destroying.
	closed: makeBitMapDescriptor(kClosed),
	// True if close has been emitted or would have been emitted depending on emitClose.
	closeEmitted: makeBitMapDescriptor(kCloseEmitted),
	multiAwaitDrain: makeBitMapDescriptor(kMultiAwaitDrain),
	// If true, a maybeReadMore has been scheduled.
	readingMore: makeBitMapDescriptor(kReadingMore),
	dataEmitted: makeBitMapDescriptor(kDataEmitted),

	// Indicates whether the stream has errored. When true no further
	// _read calls, 'data' or 'readable' events should occur.
	errored: {
		enumerable: false,
		get(this: ReadableState) {
			return (this[kState] & kErrored) !== 0 ? this[kErroredValue] : null;
		},
		set(this: ReadableState, value: unknown) {
			if (value) {
				this[kErroredValue] = value;
				this[kState] |= kErrored;
			} else {
				this[kState] &= ~kErrored;
			}
		},
	},

	defaultEncoding: {
		enumerable: false,
		get(this: ReadableState) {
			return (this[kState] & kDefaultUTF8Encoding) !== 0 ? "utf8" : this[kDefaultEncodingValue];
		},
		set(this: ReadableState, value: string) {
			if (value === "utf8" || value === "utf-8") {
				this[kState] |= kDefaultUTF8Encoding;
			} else {
				this[kState] &= ~kDefaultUTF8Encoding;
				this[kDefaultEncodingValue] = value;
			}
		},
	},

	decoder: {
		enumerable: false,
		get(this: ReadableState) {
			return (this[kState] & kDecoder) !== 0 ? this[kDecoderValue] : null;
		},
		set(this: ReadableState, value: StringDecoder | null) {
			if (value) {
				this[kDecoderValue] = value;
				this[kState] |= kDecoder;
			} else {
				this[kState] &= ~kDecoder;
			}
		},
	},

	encoding: {
		enumerable: false,
		get(this: ReadableState) {
			return (this[kState] & kEncoding) !== 0 ? this[kEncodingValue] : null;
		},
		set(this: ReadableState, value: string | null) {
			if (value) {
				this[kEncodingValue] = value;
				this[kState] |= kEncoding;
			} else {
				this[kState] &= ~kEncoding;
			}
		},
	},

	flowing: {
		enumerable: false,
		get(this: ReadableState) {
			return (this[kState] & kHasFlowing) !== 0 ? (this[kState] & kFlowing) !== 0 : null;
		},
		set(this: ReadableState, value: boolean | null) {
			if (value == null) {
				this[kState] &= ~(kHasFlowing | kFlowing);
			} else if (value) {
				this[kState] |= kHasFlowing | kFlowing;
			} else {
				this[kState] |= kHasFlowing;
				this[kState] &= ~kFlowing;
			}
		},
	},

	// Legacy getter for `pipesCount`.
	pipesCount: {
		get(this: ReadableState) {
			return this.pipes.length;
		},
	},

	// Legacy property for `paused`.
	paused: {
		get(this: ReadableState) {
			return (this[kState] & kPaused) !== 0;
		},
		set(this: ReadableState, value: boolean) {
			this[kState] |= kHasPaused;
			if (value) this[kState] |= kPaused;
			else this[kState] &= ~kPaused;
		},
	},
});

ReadableState.prototype[kOnConstructed] = function onConstructed(this: ReadableState, stream: ReadableImpl) {
	if ((this[kState] & kNeedReadable) !== 0) maybeReadMore(stream, this);
};

export interface ReadableConstructor {
	new (options?: ReadableOptions): ReadableImpl;
	(this: unknown, options?: ReadableOptions): void;
	prototype: ReadableImpl;
	ReadableState: typeof ReadableState;
	_fromList: typeof fromList;
	from(iterable: unknown, opts?: ReadableOptions): ReadableImpl;
	fromWeb(readableStream: unknown, options?: unknown): ReadableImpl;
	toWeb(streamReadable: unknown, options?: unknown): unknown;
	wrap(src: unknown, options?: ReadableOptions): ReadableImpl;
}

export const Readable = function Readable(this: ReadableImpl, options?: ReadableOptions): ReadableImpl | void {
	if (!(this instanceof Readable)) return new Readable(options);

	this._readableState = new ReadableState(options, this, false);

	if (options) {
		if (typeof options.read === "function") this._read = options.read;
		if (typeof options.destroy === "function") this._destroy = options.destroy;
		if (typeof options.construct === "function") this._construct = options.construct;
		if (options.signal) addAbortSignal(options.signal, this);
	}

	Stream.call(this, options);

	if (this._construct != null) {
		destroyImpl.construct(this as never, () => {
			this._readableState[kOnConstructed](this);
		});
	}
} as unknown as ReadableConstructor;

Object.setPrototypeOf(Readable.prototype, Stream.prototype);
Object.setPrototypeOf(Readable, Stream);

Readable.ReadableState = ReadableState;

const proto = Readable.prototype as unknown as Record<string | symbol, unknown>;

proto.destroy = destroyImpl.destroy;
proto._undestroy = destroyImpl.undestroy;
proto._destroy = function (this: ReadableImpl, err: Error | null, cb: Callback) {
	cb(err);
};

proto[(EE as unknown as { captureRejectionSymbol: symbol }).captureRejectionSymbol] = function (
	this: ReadableImpl,
	err: unknown,
) {
	this.destroy(err);
};

proto[Symbol.asyncDispose] = async function (this: ReadableImpl): Promise<void> {
	let error: unknown;
	if (!this.destroyed) {
		error = this.readableEnded ? null : new AbortError();
		this.destroy(error);
	}
	const { promise, resolve, reject } = Promise.withResolvers<null>();
	eos(this, (err) => (err && err !== error ? reject(err) : resolve(null)));
	await promise;
};

// Manually shove something into the read() buffer.
// This returns true if the highWaterMark has not been hit yet,
// similar to how Writable.write() returns true if you should
// write() some more.
proto.push = function push(this: ReadableImpl, chunk: unknown, encoding?: string): boolean {
	const state = this._readableState;
	return (state[kState] & kObjectMode) === 0
		? readableAddChunkPushByteMode(this, state, chunk, encoding)
		: readableAddChunkPushObjectMode(this, state, chunk, encoding);
};

// Unshift should *always* be something directly out of read().
proto.unshift = function unshift(this: ReadableImpl, chunk: unknown, encoding?: string): boolean {
	const state = this._readableState;
	return (state[kState] & kObjectMode) === 0
		? readableAddChunkUnshiftByteMode(this, state, chunk, encoding)
		: readableAddChunkUnshiftObjectMode(this, state, chunk);
};

function uint8ArrayToBuffer(chunk: ArrayBufferView): Buffer {
	return new FastBuffer(chunk.buffer, chunk.byteOffset, chunk.byteLength);
}

function readableAddChunkUnshiftByteMode(
	stream: ReadableImpl,
	state: ReadableState,
	chunk: unknown,
	encoding?: string,
): boolean {
	if (chunk === null) {
		state[kState] &= ~kReading;
		onEofChunk(stream, state);
		return false;
	}

	if (typeof chunk === "string") {
		encoding ||= state.defaultEncoding;
		if (state.encoding !== encoding) {
			if (state.encoding) {
				// When unshifting, if state.encoding is set, we have to save
				// the string in the BufferList with the state encoding.
				chunk = Buffer.from(chunk, encoding as BufferEncoding).toString(state.encoding as BufferEncoding);
			} else {
				chunk = Buffer.from(chunk, encoding as BufferEncoding);
			}
		}
	} else if (ArrayBuffer.isView(chunk)) {
		chunk = uint8ArrayToBuffer(chunk);
	} else if (chunk !== undefined && !(chunk instanceof Buffer)) {
		errorOrDestroy(
			stream as never,
			new ERR_INVALID_ARG_TYPE("chunk", ["string", "Buffer", "TypedArray", "DataView"], chunk),
		);
		return false;
	}

	if (!(chunk && (chunk as Buffer | string).length > 0)) return canPushMore(state);

	return readableAddChunkUnshiftValue(stream, state, chunk);
}

function readableAddChunkUnshiftObjectMode(stream: ReadableImpl, state: ReadableState, chunk: unknown): boolean {
	if (chunk === null) {
		state[kState] &= ~kReading;
		onEofChunk(stream, state);
		return false;
	}
	return readableAddChunkUnshiftValue(stream, state, chunk);
}

function readableAddChunkUnshiftValue(stream: ReadableImpl, state: ReadableState, chunk: unknown): boolean {
	if ((state[kState] & kEndEmitted) !== 0) errorOrDestroy(stream as never, new ERR_STREAM_UNSHIFT_AFTER_END_EVENT());
	else if ((state[kState] & (kDestroyed | kErrored)) !== 0) return false;
	else addChunk(stream, state, chunk, true);
	return canPushMore(state);
}

function readableAddChunkPushByteMode(
	stream: ReadableImpl,
	state: ReadableState,
	chunk: unknown,
	encoding?: string,
): boolean {
	if (chunk === null) {
		state[kState] &= ~kReading;
		onEofChunk(stream, state);
		return false;
	}

	if (typeof chunk === "string") {
		encoding ||= state.defaultEncoding;
		if (state.encoding !== encoding) {
			chunk = Buffer.from(chunk, encoding as BufferEncoding);
			encoding = "";
		}
	} else if (chunk instanceof Buffer) {
		encoding = "";
	} else if (ArrayBuffer.isView(chunk)) {
		chunk = uint8ArrayToBuffer(chunk);
		encoding = "";
	} else if (chunk !== undefined) {
		errorOrDestroy(
			stream as never,
			new ERR_INVALID_ARG_TYPE("chunk", ["string", "Buffer", "TypedArray", "DataView"], chunk),
		);
		return false;
	}

	if (!chunk || (chunk as Buffer | string).length <= 0) {
		state[kState] &= ~kReading;
		maybeReadMore(stream, state);
		return canPushMore(state);
	}

	if ((state[kState] & kEnded) !== 0) {
		errorOrDestroy(stream as never, new ERR_STREAM_PUSH_AFTER_EOF());
		return false;
	}

	if ((state[kState] & (kDestroyed | kErrored)) !== 0) return false;

	state[kState] &= ~kReading;
	if ((state[kState] & kDecoder) !== 0 && !encoding) {
		chunk = (state[kDecoderValue] as StringDecoder).write(chunk as Buffer);
		if ((chunk as string).length === 0) {
			maybeReadMore(stream, state);
			return canPushMore(state);
		}
	}

	addChunk(stream, state, chunk, false);
	return canPushMore(state);
}

function readableAddChunkPushObjectMode(
	stream: ReadableImpl,
	state: ReadableState,
	chunk: unknown,
	encoding?: string,
): boolean {
	if (chunk === null) {
		state[kState] &= ~kReading;
		onEofChunk(stream, state);
		return false;
	}

	if ((state[kState] & kEnded) !== 0) {
		errorOrDestroy(stream as never, new ERR_STREAM_PUSH_AFTER_EOF());
		return false;
	}

	if ((state[kState] & (kDestroyed | kErrored)) !== 0) return false;

	state[kState] &= ~kReading;

	if ((state[kState] & kDecoder) !== 0 && !encoding) {
		chunk = (state[kDecoderValue] as StringDecoder).write(chunk as Buffer);
	}

	addChunk(stream, state, chunk, false);
	return canPushMore(state);
}

function canPushMore(state: ReadableState): boolean {
	// We can push more data if we are below the highWaterMark.
	// Also, if we have no data yet, we can stand some more bytes.
	// This is to work around cases where hwm=0, such as the repl.
	return (state[kState] & kEnded) === 0 && (state.length < state.highWaterMark || state.length === 0);
}

function addChunk(stream: ReadableImpl, state: ReadableState, chunk: unknown, addToFront: boolean): void {
	if ((state[kState] & (kFlowing | kSync | kDataListening)) === (kFlowing | kDataListening) && state.length === 0) {
		// Use the guard to avoid creating `Set()` repeatedly when we have multiple pipes.
		if ((state[kState] & kMultiAwaitDrain) !== 0) (state.awaitDrainWriters as Set<PipeDest>).clear();
		else state.awaitDrainWriters = null;

		state[kState] |= kDataEmitted;
		stream.emit("data", chunk);
	} else {
		// Update the buffer info.
		state.length += (state[kState] & kObjectMode) !== 0 ? 1 : (chunk as Buffer | string).length;
		if (addToFront) {
			if (state.bufferIndex > 0) state.buffer[--state.bufferIndex] = chunk;
			else state.buffer.unshift(chunk); // Slow path
		} else {
			state.buffer.push(chunk);
		}

		if ((state[kState] & kNeedReadable) !== 0) emitReadable(stream);
	}
	maybeReadMore(stream, state);
}

proto.isPaused = function isPaused(this: ReadableImpl): boolean {
	const state = this._readableState;
	return (state[kState] & kPaused) !== 0 || (state[kState] & (kHasFlowing | kFlowing)) === kHasFlowing;
};

// Backwards compatibility.
proto.setEncoding = function setEncoding(this: ReadableImpl, enc?: string): ReadableImpl {
	const state = this._readableState;
	const decoder = new StringDecoder(enc);
	state.decoder = decoder;
	// If setEncoding(null), decoder.encoding equals utf8.
	state.encoding = (decoder as unknown as { encoding: string }).encoding;

	// Iterate over current buffer to convert already stored Buffers:
	let content = "";
	for (const data of state.buffer.slice(state.bufferIndex)) content += decoder.write(data as Buffer);
	if ((state[kState] & kEnded) !== 0) content += decoder.end();
	state.buffer.length = 0;
	state.bufferIndex = 0;

	if (content !== "") state.buffer.push(content);
	state.length = content.length;
	return this;
};

// Don't raise the hwm > 1GB.
const MAX_HWM = 0x40000000;
function computeNewHighWaterMark(n: number): number {
	if (n > MAX_HWM) throw new ERR_OUT_OF_RANGE("size", "<= 1GiB", n);
	// Get the next highest power of 2 to prevent increasing hwm excessively in tiny amounts.
	n--;
	n |= n >>> 1;
	n |= n >>> 2;
	n |= n >>> 4;
	n |= n >>> 8;
	n |= n >>> 16;
	n++;
	return n;
}

function howMuchToRead(n: number, state: ReadableState): number {
	if (n <= 0 || (state.length === 0 && (state[kState] & kEnded) !== 0)) return 0;
	if ((state[kState] & kObjectMode) !== 0) return 1;
	if (Number.isNaN(n)) {
		// Fast path for buffers.
		if ((state[kState] & kDecoder) === 0 && state.length) return (state.buffer[state.bufferIndex] as Buffer).length;
		// Only flow one buffer at a time.
		if ((state[kState] & kFlowing) !== 0 && state.length) return (state.buffer[state.bufferIndex] as Buffer).length;
		return state.length;
	}
	if (n <= state.length) return n;
	return (state[kState] & kEnded) !== 0 ? state.length : 0;
}

// You can override either this method, or the async _read(n) below.
proto.read = function read(this: ReadableImpl, n?: number | string): unknown {
	if (n === undefined) n = NaN;
	else if (!Number.isInteger(n)) n = Number.parseInt(n as string, 10);
	let num = n as number;
	const state = this._readableState;
	const nOrig = num;

	// If we're asking for more than the current hwm, then raise the hwm.
	if (num > state.highWaterMark) state.highWaterMark = computeNewHighWaterMark(num);

	if (num !== 0) state[kState] &= ~(kEmittedReadable | kEofReadablePending);

	// If we're doing read(0) to trigger a readable event, but we
	// already have a bunch of data in the buffer, then just trigger
	// the 'readable' event and move on.
	const stateLength = state.length;
	if (
		num === 0 &&
		(state[kState] & kNeedReadable) !== 0 &&
		((state.highWaterMark !== 0 ? stateLength >= state.highWaterMark : stateLength > 0) ||
			(state[kState] & kEnded) !== 0)
	) {
		if (stateLength === 0 && (state[kState] & kEnded) !== 0) endReadable(this);
		else emitReadable(this);
		return null;
	}

	num = howMuchToRead(num, state);

	// If we've ended, and we're now clear, then finish it up.
	if (num === 0 && (state[kState] & kEnded) !== 0) {
		if (state.length === 0) endReadable(this);
		return null;
	}

	// All the actual chunk generation logic needs to be *below* the call to
	// _read: _read may be synchronous and change the state of the buffer.

	// if we need a readable event, then we need to do some reading.
	let doRead = (state[kState] & kNeedReadable) !== 0;

	// If we currently have less than the highWaterMark, then also read some.
	if (state.length === 0 || state.length - num < state.highWaterMark) doRead = true;

	// However, if we've ended, then there's no point, if we're already
	// reading, then it's unnecessary, if we're constructing we have to wait,
	// and if we're destroyed or errored, then it's not allowed,
	if ((state[kState] & (kReading | kEnded | kDestroyed | kErrored | kConstructed)) !== kConstructed) {
		doRead = false;
	} else if (doRead) {
		state[kState] |= kReading | kSync;
		// If the length is currently zero, then we *need* a readable event.
		if (state.length === 0) state[kState] |= kNeedReadable;

		// Call internal read method
		try {
			this._read(state.highWaterMark);
		} catch (err) {
			errorOrDestroy(this as never, err);
		}
		state[kState] &= ~kSync;

		// If _read pushed data synchronously, then `reading` will be false,
		// and we need to re-evaluate how much data we can return to the user.
		if ((state[kState] & kReading) === 0) num = howMuchToRead(nOrig, state);
	}

	let ret: unknown;
	if (num > 0) ret = fromList(num, state);
	else ret = null;

	if (ret === null) {
		state[kState] |= state.length <= state.highWaterMark ? kNeedReadable : 0;
		num = 0;
	} else {
		state.length -= num;
		if ((state[kState] & kMultiAwaitDrain) !== 0) (state.awaitDrainWriters as Set<PipeDest>).clear();
		else state.awaitDrainWriters = null;
	}

	if (state.length === 0) {
		// If we have nothing in the buffer, then we want to know
		// as soon as we *do* get something into the buffer.
		if ((state[kState] & kEnded) === 0) state[kState] |= kNeedReadable;

		// If we tried to read() past the EOF, then emit end on the next tick.
		if (nOrig !== num && (state[kState] & kEnded) !== 0) endReadable(this);
	}

	if (ret !== null && (state[kState] & (kErrorEmitted | kCloseEmitted)) === 0) {
		state[kState] |= kDataEmitted;
		this.emit("data", ret);
	}

	return ret;
};

function onEofChunk(stream: ReadableImpl, state: ReadableState): void {
	if ((state[kState] & kEnded) !== 0) return;
	const decoder = (state[kState] & kDecoder) !== 0 ? (state[kDecoderValue] as StringDecoder) : null;
	if (decoder) {
		const chunk = decoder.end();
		if (chunk?.length) {
			state.buffer.push(chunk);
			state.length += (state[kState] & kObjectMode) !== 0 ? 1 : chunk.length;
		}
	}
	state[kState] |= kEnded;

	if ((state[kState] & kSync) !== 0) {
		// If we are sync, wait until next tick to emit the data.
		// Otherwise we risk emitting data in the flow()
		// the readable code triggers during a read() call.
		if ((state[kState] & (kFlowing | kNeedReadable)) !== 0 || stream.listenerCount("readable") > 0) {
			emitReadable(stream);
		} else {
			// Nobody is observing the stream: a 'readable' listener attached
			// later redeems it (see Readable.prototype.on); read() and resume()
			// reach the end of the stream on their own.
			state[kState] |= kEofReadablePending;
		}
	} else {
		// Emit 'readable' now to make sure it gets picked up.
		state[kState] &= ~kNeedReadable;
		state[kState] |= kEmittedReadable;
		// We have to emit readable now that we are EOF. Modules
		// in the ecosystem (e.g. dicer) rely on this event being sync.
		emitReadable_(stream);
	}
}

// Don't emit readable right away in sync mode, because this can trigger
// another read() call => stack overflow.
function emitReadable(stream: ReadableImpl): void {
	const state = stream._readableState;
	state[kState] &= ~kNeedReadable;
	if ((state[kState] & kEmittedReadable) === 0) {
		state[kState] |= kEmittedReadable;
		nextTick(emitReadable_ as Listener, stream);
	}
}

function emitReadable_(stream: ReadableImpl): void {
	const state = stream._readableState;
	if ((state[kState] & (kDestroyed | kErrored)) === 0 && (state.length || (state[kState] & kEnded) !== 0)) {
		stream.emit("readable");
		state[kState] &= ~kEmittedReadable;
	}

	// The stream needs another readable event if:
	// 1. It is not flowing, as the flow mechanism will take care of it.
	// 2. It is not ended.
	// 3. It is below the highWaterMark, so we can schedule another readable later.
	state[kState] |=
		(state[kState] & (kFlowing | kEnded)) === 0 && state.length <= state.highWaterMark ? kNeedReadable : 0;
	flow(stream);
}

// If we're not ended, or reading, and the length < hwm,
// then go ahead and try to read some more preemptively.
function maybeReadMore(stream: ReadableImpl, state: ReadableState): void {
	if ((state[kState] & (kReadingMore | kReading | kConstructed)) === kConstructed) {
		state[kState] |= kReadingMore;
		nextTick(maybeReadMore_ as Listener, stream, state);
	}
}

function maybeReadMore_(stream: ReadableImpl, state: ReadableState): void {
	// Keep reading while not enough data is buffered (or nothing is buffered
	// in flowing mode), the stream has not ended and no _read() is pending.
	while (
		(state[kState] & (kReading | kEnded)) === 0 &&
		(state.length < state.highWaterMark || ((state[kState] & kFlowing) !== 0 && state.length === 0))
	) {
		const len = state.length;
		stream.read(0);
		if (len === state.length) break; // Didn't get any data, stop spinning.
	}
	state[kState] &= ~kReadingMore;
}

// Abstract method. To be overridden in specific implementation classes.
proto._read = function _read(_n: number): void {
	throw new ERR_METHOD_NOT_IMPLEMENTED("_read()");
};

proto.pipe = function pipe<T>(this: ReadableImpl, destArg: T, pipeOpts?: { end?: boolean }): T {
	const src = this;
	const state = this._readableState;
	const dest = destArg as unknown as PipeDest;

	if (state.pipes.length === 1) {
		if ((state[kState] & kMultiAwaitDrain) === 0) {
			state[kState] |= kMultiAwaitDrain;
			state.awaitDrainWriters = new Set(state.awaitDrainWriters ? [state.awaitDrainWriters as PipeDest] : []);
		}
	}

	state.pipes.push(dest);

	const proc = (globalThis as unknown as { process: { stdout?: unknown; stderr?: unknown } }).process;
	const doEnd = (!pipeOpts || pipeOpts.end !== false) && dest !== proc.stdout && dest !== proc.stderr;

	const endFn = doEnd ? onend : unpipe;
	if ((state[kState] & kEndEmitted) !== 0) nextTick(endFn);
	else src.once("end", endFn);

	dest.on("unpipe", onunpipe as Listener);
	function onunpipe(readable: unknown, unpipeInfo?: { hasUnpiped: boolean }) {
		if (readable === src) {
			if (unpipeInfo && unpipeInfo.hasUnpiped === false) {
				unpipeInfo.hasUnpiped = true;
				cleanup();
			}
		}
	}

	function onend() {
		dest.end();
	}

	let ondrain: (() => void) | undefined;

	let cleanedUp = false;
	function cleanup() {
		// Cleanup event handlers once the pipe is broken.
		dest.removeListener("close", onclose);
		dest.removeListener("finish", onfinish);
		if (ondrain) dest.removeListener("drain", ondrain);
		dest.removeListener("error", onerror as Listener);
		dest.removeListener("unpipe", onunpipe as Listener);
		src.removeListener("end", onend);
		src.removeListener("end", unpipe);
		src.removeListener("data", ondata as Listener);

		cleanedUp = true;

		// If the reader is waiting for a drain event from this specific
		// writer, then it would cause it to never start flowing again.
		// pipeOnDrain only removes this destination from the awaiting-drain
		// set and resumes once nothing else awaits a drain, so it is safe
		// to call unconditionally.
		if (ondrain && state.awaitDrainWriters) ondrain();
	}

	function pause() {
		// If the user unpiped during `dest.write()`, it is possible
		// to get stuck in a permanently paused state if that write
		// also returned false.
		// => Check whether `dest` is still a piping destination.
		if (!cleanedUp) {
			if (state.pipes.length === 1 && state.pipes[0] === dest) {
				state.awaitDrainWriters = dest;
				state[kState] &= ~kMultiAwaitDrain;
			} else if (state.pipes.length > 1 && state.pipes.includes(dest)) {
				(state.awaitDrainWriters as Set<PipeDest>).add(dest);
			}
			src.pause();
		}
		if (!ondrain) {
			// When the dest drains, it reduces the awaitDrain counter on the source.
			ondrain = pipeOnDrain(src, dest);
			dest.on("drain", ondrain);
		}
	}

	src.on("data", ondata as Listener);
	function ondata(chunk: unknown) {
		try {
			const ret = dest.write(chunk);
			if (ret === false) pause();
		} catch (error) {
			dest.destroy(error);
		}
	}

	// If the dest has an error, then stop piping into it.
	// However, don't suppress the throwing behavior for this.
	function onerror(er: unknown) {
		unpipe();
		dest.removeListener("error", onerror as Listener);
		if (dest.listenerCount("error") === 0) {
			const s = dest._writableState || dest._readableState;
			if (s && !s.errorEmitted) {
				// User incorrectly emitted 'error' directly on the stream.
				errorOrDestroy(dest as never, er);
			} else {
				dest.emit("error", er);
			}
		}
	}

	// Make sure our error handler is attached before userland ones.
	prependListener(dest as never, "error", onerror as Listener);

	// Both close and finish should trigger unpipe, but only once.
	function onclose() {
		dest.removeListener("finish", onfinish);
		unpipe();
	}
	dest.once("close", onclose);
	function onfinish() {
		dest.removeListener("close", onclose);
		unpipe();
	}
	dest.once("finish", onfinish);

	function unpipe() {
		src.unpipe(dest);
	}

	// Tell the dest that it's being piped to.
	dest.emit("pipe", src);

	// Start the flow if it hasn't been started already.
	if (dest.writableNeedDrain === true) {
		pause();
	} else if ((state[kState] & kFlowing) === 0) {
		src.resume();
	}

	return destArg;
};

function pipeOnDrain(src: ReadableImpl, dest: PipeDest): () => void {
	return function pipeOnDrainFunctionResult() {
		const state = src._readableState;

		// `ondrain` will call directly, `this` maybe not a reference to dest,
		// so we use the real dest here.
		if (state.awaitDrainWriters === dest) {
			state.awaitDrainWriters = null;
		} else if ((state[kState] & kMultiAwaitDrain) !== 0) {
			(state.awaitDrainWriters as Set<PipeDest>).delete(dest);
		}

		if (
			(!state.awaitDrainWriters || (state.awaitDrainWriters as Set<PipeDest>).size === 0) &&
			(state[kState] & kDataListening) !== 0
		) {
			src.resume();
		}
	};
}

proto.unpipe = function unpipe(this: ReadableImpl, dest?: PipeDest): ReadableImpl {
	const state = this._readableState;
	const unpipeInfo = { hasUnpiped: false };

	// If we're not piping anywhere, then do nothing.
	if (state.pipes.length === 0) return this;

	if (!dest) {
		// remove all.
		const dests = state.pipes;
		state.pipes = [];
		this.pause();
		for (let i = 0; i < dests.length; i++) dests[i].emit("unpipe", this, { hasUnpiped: false });
		return this;
	}

	// Try to find the right one.
	const index = state.pipes.indexOf(dest);
	if (index === -1) return this;

	state.pipes.splice(index, 1);
	if (state.pipes.length === 0) this.pause();

	dest.emit("unpipe", this, unpipeInfo);
	return this;
};

const streamOn = Stream.prototype.on as (this: unknown, ev: string | symbol, fn: Listener) => unknown;
const streamRemoveListener = Stream.prototype.removeListener as (
	this: unknown,
	ev: string | symbol,
	fn: Listener,
) => unknown;
const streamRemoveAllListeners = Stream.prototype.removeAllListeners as (
	this: unknown,
	ev?: string | symbol,
) => unknown;

// Set up data events if they are asked for.
// Ensure readable listeners eventually get something.
proto.on = function on(this: ReadableImpl, ev: string | symbol, fn: Listener): unknown {
	const res = streamOn.call(this, ev, fn);
	const state = this._readableState;

	if (ev === "data") {
		state[kState] |= kDataListening;

		// Update readableListening so that resume() may be a no-op
		// a few lines down. This is needed to support once('readable').
		state[kState] |= this.listenerCount("readable") > 0 ? kReadableListening : 0;

		// Try start flowing on next tick if stream isn't explicitly paused.
		if ((state[kState] & (kHasFlowing | kFlowing)) !== kHasFlowing) this.resume();
	} else if (ev === "readable") {
		if ((state[kState] & (kEndEmitted | kReadableListening)) === 0) {
			state[kState] |= kReadableListening | kNeedReadable | kHasFlowing;
			state[kState] &= ~(kFlowing | kEmittedReadable);
			if (state.length) {
				emitReadable(this);
			} else {
				if ((state[kState] & kEofReadablePending) !== 0) {
					// The end-of-stream 'readable' emission was skipped because
					// nobody was listening when the stream ended: emit it now.
					state[kState] &= ~kEofReadablePending;
					emitReadable(this);
				}
				if ((state[kState] & kReading) === 0) nextTick(nReadingNextTick as Listener, this);
			}
		}
	}

	return res;
};
proto.addListener = proto.on;

proto.removeListener = function removeListener(this: ReadableImpl, ev: string | symbol, fn: Listener): unknown {
	const state = this._readableState;
	const res = streamRemoveListener.call(this, ev, fn);

	if (ev === "readable") {
		// We need to check if there is someone still listening to readable
		// and reset the state. However this needs to happen after readable has
		// been emitted but before I/O (nextTick) to support once('readable', fn)
		// cycles. This means that calling resume within the same tick will
		// have no effect.
		nextTick(updateReadableListening as Listener, this);
	} else if (ev === "data" && this.listenerCount("data") === 0) {
		state[kState] &= ~kDataListening;
	}

	return res;
};
proto.off = proto.removeListener;

proto.removeAllListeners = function removeAllListeners(this: ReadableImpl, ev?: string | symbol): unknown {
	const res = Reflect.apply(streamRemoveAllListeners, this, arguments);
	if (ev === "readable" || ev === undefined) nextTick(updateReadableListening as Listener, this);
	return res;
};

function updateReadableListening(self: ReadableImpl): void {
	const state = self._readableState;

	if (self.listenerCount("readable") > 0) state[kState] |= kReadableListening;
	else state[kState] &= ~kReadableListening;

	if ((state[kState] & (kHasPaused | kPaused | kResumeScheduled)) === (kHasPaused | kResumeScheduled)) {
		// Flowing needs to be set to true now, otherwise the upcoming resume will not flow.
		state[kState] |= kHasFlowing | kFlowing;
		// Crude way to check if we should resume.
	} else if ((state[kState] & kDataListening) !== 0) {
		self.resume();
	} else if ((state[kState] & kReadableListening) === 0) {
		state[kState] &= ~(kHasFlowing | kFlowing);
	}
}

function nReadingNextTick(self: ReadableImpl): void {
	self.read(0);
}

// pause() and resume() are remnants of the legacy readable stream API.
// If the user uses them, then switch into old mode.
proto.resume = function resume(this: ReadableImpl): ReadableImpl {
	const state = this._readableState;
	if ((state[kState] & kDestroyed) !== 0) return this;
	if ((state[kState] & kFlowing) === 0) {
		// We flow only if there is no one listening for readable,
		// but we still have to call resume().
		state[kState] |= kHasFlowing;
		if ((state[kState] & kReadableListening) === 0) state[kState] |= kFlowing;
		else state[kState] &= ~kFlowing;
		scheduleResume(this, state);
	}
	state[kState] |= kHasPaused;
	state[kState] &= ~kPaused;
	return this;
};

function scheduleResume(stream: ReadableImpl, state: ReadableState): void {
	if ((state[kState] & kResumeScheduled) === 0) {
		state[kState] |= kResumeScheduled;
		nextTick(resume_ as Listener, stream, state);
	}
}

function resume_(stream: ReadableImpl, state: ReadableState): void {
	if ((state[kState] & kReading) === 0) stream.read(0);

	state[kState] &= ~kResumeScheduled;
	stream.emit("resume");
	flow(stream);
	if ((state[kState] & (kFlowing | kReading)) === kFlowing) stream.read(0);
}

proto.pause = function pause(this: ReadableImpl): ReadableImpl {
	const state = this._readableState;
	if ((state[kState] & kDestroyed) !== 0) return this;
	if ((state[kState] & (kHasFlowing | kFlowing)) !== kHasFlowing) {
		state[kState] |= kHasFlowing;
		state[kState] &= ~kFlowing;
		this.emit("pause");
	}
	state[kState] |= kHasPaused | kPaused;
	return this;
};

function flow(stream: ReadableImpl): void {
	const state = stream._readableState;
	while ((state[kState] & kFlowing) !== 0 && stream.read() !== null);
}

// Wrap an old-style stream as the async data source.
// This is *not* part of the readable stream interface.
proto.wrap = function wrap(this: ReadableImpl, stream: EE & { pause?(): void; resume?(): void }): ReadableImpl {
	let paused = false;

	stream.on("data", (chunk: unknown) => {
		if (!this.push(chunk) && stream.pause) {
			paused = true;
			stream.pause();
		}
	});

	stream.on("end", () => {
		this.push(null);
	});

	stream.on("error", (err: unknown) => {
		errorOrDestroy(this as never, err);
	});

	stream.on("close", () => {
		this.destroy();
	});

	stream.on("destroy", () => {
		this.destroy();
	});

	this._read = () => {
		if (paused && stream.resume) {
			paused = false;
			stream.resume();
		}
	};

	// Proxy all the other methods. Important when wrapping filters and duplexes.
	const src = stream as unknown as Record<string, unknown>;
	for (const i in src) {
		if (Object.hasOwn(src, i) && this[i] === undefined && typeof src[i] === "function") {
			this[i] = (src[i] as (...a: unknown[]) => unknown).bind(stream);
		}
	}

	return this;
};

proto[Symbol.asyncIterator] = function (this: ReadableImpl): AsyncIterableIterator<unknown> {
	return streamToAsyncIterator(this);
};

proto.iterator = function iterator(
	this: ReadableImpl,
	options?: { destroyOnReturn?: boolean },
): AsyncIterableIterator<unknown> {
	if (options !== undefined) validateObject(options, "options");
	return streamToAsyncIterator(this, options);
};

function streamToAsyncIterator(
	stream: ReadableImpl,
	options?: { destroyOnReturn?: boolean },
): AsyncIterableIterator<unknown> {
	if (typeof stream.read !== "function") stream = Readable.wrap(stream, { objectMode: true });
	const iter = createAsyncIterator(stream, options) as AsyncIterableIterator<unknown> & { stream?: ReadableImpl };
	iter.stream = stream;
	return iter;
}

const AsyncIteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf(async function* () {}).prototype) as object;

type IterResult = IteratorResult<unknown>;
type Resolve = (v: IterResult) => void;
type Reject = (e: unknown) => void;
interface IterRequest {
	type: "next" | "return" | "throw";
	value: unknown;
	resolve: Resolve;
	reject: Reject;
}

// Async iterator over a Readable. Requests received while another is
// outstanding are queued and processed in order.
function createAsyncIterator(
	stream: ReadableImpl,
	options?: { destroyOnReturn?: boolean },
): AsyncIterableIterator<unknown> {
	let callback: () => void = nop;
	let error: unknown; // undefined: active, null: ended cleanly, else: Error
	let started = false;
	let completed = false;
	let inFlight = false; // An asynchronous request is outstanding
	let queue: IterRequest[] | null = null; // Requests received while inFlight
	let draining = false;
	let cleanup: () => void = nop;

	// The 'readable' listener: wakes up a pending pump().
	function onReadable() {
		callback();
		callback = nop;
	}

	// Resolves on the next 'readable' or end-of-stream.
	function nextWakeup(): Promise<void> {
		const { promise, resolve } = Promise.withResolvers<void>();
		callback = resolve as () => void;
		return promise;
	}

	function start() {
		started = true;
		stream.on("readable", onReadable);
		cleanup = eos(stream, { writable: false }, (err) => {
			error = err ? aggregateTwoErrors(error, err) : null;
			callback();
			callback = nop;
		});
	}

	// Complete the iterator and either destroy the stream or detach from it.
	function finalize() {
		completed = true;
		if ((error || options?.destroyOnReturn !== false) && (error === undefined || stream._readableState.autoDestroy)) {
			destroyImpl.destroyer(stream as never, null);
		} else {
			stream.off("readable", onReadable);
			cleanup();
		}
	}

	function settleError(err: unknown, reject: Reject) {
		error = aggregateTwoErrors(error, err);
		finalize();
		reject(error);
	}

	function drain() {
		// Requests settled synchronously call back into drain(); the guard
		// keeps a single loop going instead of recursing once per request.
		if (draining) return;
		draining = true;
		try {
			while (!inFlight && queue !== null && queue.length > 0) {
				const req = queue.shift() as IterRequest;
				if (req.type === "next") processNext(req.resolve, req.reject);
				else if (req.type === "return") processReturn(req.value, req.resolve);
				else processThrow(req.value, req.reject);
			}
		} finally {
			draining = false;
		}
	}

	// Thenable chunks are unwrapped before delivery; a rejection tears
	// down the iterator and the stream.
	function onChunkFulfilled(value: unknown): IterResult {
		inFlight = false;
		if (queue !== null) drain();
		return { value, done: false };
	}

	function onChunkRejected(err: unknown): never {
		inFlight = false;
		error = aggregateTwoErrors(error, err);
		finalize();
		if (queue !== null) drain();
		throw error;
	}

	// Runs with inFlight === true; settles the request and hands over to
	// any requests that queued up behind it.
	function pump(resolve: Resolve, reject: Reject) {
		const chunk = stream.destroyed ? null : stream.read();
		if (chunk !== null) {
			// Read `then` only once; `undefined` is a valid chunk value.
			const then = (chunk as { then?: unknown } | undefined)?.then;
			if (typeof then === "function") {
				then.call(
					chunk,
					(value: unknown) => {
						inFlight = false;
						resolve({ value, done: false });
						if (queue !== null) drain();
					},
					(err: unknown) => {
						inFlight = false;
						settleError(err, reject);
						if (queue !== null) drain();
					},
				);
				return;
			}
			inFlight = false;
			resolve({ value: chunk, done: false });
			if (queue !== null) drain();
		} else if (error) {
			inFlight = false;
			settleError(error, reject);
			if (queue !== null) drain();
		} else if (error === null) {
			inFlight = false;
			finalize();
			resolve({ value: undefined, done: true });
			if (queue !== null) drain();
		} else {
			// No data buffered yet; wait for 'readable' or end-of-stream and retry.
			nextWakeup().then(() => pump(resolve, reject));
		}
	}

	function processNext(resolve: Resolve, reject: Reject) {
		if (completed) {
			resolve({ value: undefined, done: true });
			return;
		}
		if (!started) start();
		inFlight = true;
		pump(resolve, reject);
	}

	function processReturn(value: unknown, resolve: Resolve) {
		if (!completed) {
			if (started) finalize();
			// Never started: complete without touching the stream.
			else completed = true;
		}
		resolve({ value, done: true });
	}

	function processThrow(err: unknown, reject: Reject) {
		if (completed || !started) {
			completed = true;
			reject(err);
			return;
		}
		settleError(err, reject);
	}

	function enqueue(type: IterRequest["type"], value: unknown): Promise<IterResult> {
		const { promise, resolve, reject } = Promise.withResolvers<IterResult>();
		(queue ??= []).push({ type, value, resolve, reject });
		return promise;
	}

	const iterator = {
		next(): Promise<IterResult> {
			if (!inFlight && !completed) {
				if (!started) start();
				// Fast path: a chunk is already buffered.
				const chunk = stream.destroyed ? null : stream.read();
				if (chunk !== null) {
					const then = (chunk as { then?: unknown } | undefined)?.then;
					if (typeof then === "function") {
						inFlight = true;
						return then.call(chunk, onChunkFulfilled, onChunkRejected) as Promise<IterResult>;
					}
					return Promise.resolve({ value: chunk, done: false });
				}
				if (error) {
					finalize();
					return Promise.reject(error);
				}
				if (error === null) {
					finalize();
					return Promise.resolve({ value: undefined, done: true });
				}
				// No data buffered yet; wait for 'readable' or end-of-stream.
				inFlight = true;
				const { promise, resolve, reject } = Promise.withResolvers<IterResult>();
				nextWakeup().then(() => pump(resolve, reject));
				return promise;
			}
			if (inFlight) return enqueue("next", undefined);
			return Promise.resolve({ value: undefined, done: true });
		},
		return(value?: unknown): Promise<IterResult> {
			if (inFlight) return enqueue("return", value);
			const { promise, resolve } = Promise.withResolvers<IterResult>();
			processReturn(value, resolve);
			return promise;
		},
		throw(err?: unknown): Promise<IterResult> {
			if (inFlight) return enqueue("throw", err);
			const { promise, reject } = Promise.withResolvers<IterResult>();
			processThrow(err, reject);
			return promise;
		},
	};
	Object.setPrototypeOf(iterator, AsyncIteratorPrototype);
	return iterator as unknown as AsyncIterableIterator<unknown>;
}

proto.compose = function compose(this: ReadableImpl, stream: unknown, options?: { signal?: AbortSignal }): unknown {
	if (options != null) validateObject(options, "options");
	if (options?.signal != null) validateAbortSignal(options.signal, "options.signal");

	// compose -> pipeline -> Duplex reads Readable.prototype during evaluation.
	const { compose } = require("internal:streams/compose") as typeof composeModule;
	const composedStream = compose(this, stream);

	// Not validating as we already validated before
	if (options?.signal) addAbortSignalNoValidate(options.signal, composedStream);

	return composedStream;
};

// Making it explicit these properties are not enumerable
// because otherwise some prototype manipulation in userland will fail.
Object.defineProperties(Readable.prototype, {
	readable: {
		get(this: ReadableImpl) {
			const r = this._readableState;
			// r.readable === false means that this is part of a Duplex stream
			// where the readable side was disabled upon construction.
			// Compat. The user might manually disable readable side through
			// deprecated setter.
			return !!r && r.readable !== false && !r.destroyed && !r.errorEmitted && !r.endEmitted;
		},
		set(this: ReadableImpl, val: unknown) {
			// Backwards compat.
			if (this._readableState) this._readableState.readable = !!val;
		},
	},

	readableDidRead: {
		enumerable: false,
		get(this: ReadableImpl) {
			return this._readableState.dataEmitted;
		},
	},

	readableAborted: {
		enumerable: false,
		get(this: ReadableImpl) {
			return !!(
				this._readableState.readable !== false &&
				(this._readableState.destroyed || this._readableState.errored) &&
				!this._readableState.endEmitted
			);
		},
	},

	readableHighWaterMark: {
		enumerable: false,
		get(this: ReadableImpl) {
			return this._readableState.highWaterMark;
		},
	},

	readableBuffer: {
		enumerable: false,
		get(this: ReadableImpl) {
			return this._readableState?.buffer;
		},
	},

	readableFlowing: {
		enumerable: false,
		get(this: ReadableImpl) {
			return this._readableState.flowing;
		},
		set(this: ReadableImpl, state: boolean | null) {
			if (this._readableState) this._readableState.flowing = state;
		},
	},

	readableLength: {
		enumerable: false,
		get(this: ReadableImpl) {
			return this._readableState.length;
		},
	},

	readableObjectMode: {
		enumerable: false,
		get(this: ReadableImpl) {
			return this._readableState ? this._readableState.objectMode : false;
		},
	},

	readableEncoding: {
		enumerable: false,
		get(this: ReadableImpl) {
			return this._readableState ? this._readableState.encoding : null;
		},
	},

	errored: {
		enumerable: false,
		get(this: ReadableImpl) {
			return this._readableState ? this._readableState.errored : null;
		},
	},

	closed: {
		get(this: ReadableImpl) {
			return this._readableState ? this._readableState.closed : false;
		},
	},

	destroyed: {
		enumerable: false,
		get(this: ReadableImpl) {
			return this._readableState ? this._readableState.destroyed : false;
		},
		set(this: ReadableImpl, value: boolean) {
			// We ignore the value if the stream has not been initialized yet.
			if (!this._readableState) return;
			// Backward compatibility, the user is explicitly managing destroyed.
			this._readableState.destroyed = value;
		},
	},

	readableEnded: {
		enumerable: false,
		get(this: ReadableImpl) {
			return this._readableState ? this._readableState.endEmitted : false;
		},
	},
});

// Exposed for testing purposes only.
Readable._fromList = fromList;

// Pluck off n bytes from an array of buffers.
// Length is the combined lengths of all the buffers in the list.
function fromList(n: number, state: ReadableState): unknown {
	const stateLength = state.length;

	// nothing buffered.
	if (stateLength === 0) return null;

	let idx = state.bufferIndex;
	let ret: unknown;

	const buf = state.buffer;
	const len = buf.length;

	if ((state[kState] & kObjectMode) !== 0) {
		ret = buf[idx];
		buf[idx++] = null;
	} else if (!n || n >= stateLength) {
		// Read it all, truncate the list.
		if ((state[kState] & kDecoder) !== 0) {
			let s = "";
			while (idx < len) {
				s += buf[idx] as string;
				buf[idx++] = null;
			}
			ret = s;
		} else if (len - idx === 0) {
			ret = new FastBuffer();
		} else if (len - idx === 1) {
			ret = buf[idx];
			buf[idx++] = null;
		} else {
			const out = Buffer.allocUnsafe(stateLength);
			let i = 0;
			while (idx < len) {
				const data = buf[idx] as Buffer;
				out.set(data, i);
				i += data.length;
				buf[idx++] = null;
			}
			ret = out;
		}
	} else {
		const first = buf[idx] as Buffer | string;
		const firstLength = first.length;
		if (n < firstLength) {
			// `slice` is the same for buffers and strings.
			ret = first.slice(0, n);
			buf[idx] = first.slice(n);
		} else if (n === firstLength) {
			// First chunk is a perfect match.
			ret = first;
			buf[idx++] = null;
		} else if ((state[kState] & kDecoder) !== 0) {
			let s = "";
			while (idx < len) {
				const str = buf[idx] as string;
				const strLength = str.length;
				if (n > strLength) {
					s += str;
					n -= strLength;
					buf[idx++] = null;
				} else {
					if (n === strLength) {
						s += str;
						buf[idx++] = null;
					} else {
						s += str.slice(0, n);
						buf[idx] = str.slice(n);
					}
					break;
				}
			}
			ret = s;
		} else {
			const out = Buffer.allocUnsafe(n);
			const retLen = n;
			while (idx < len) {
				const data = buf[idx] as Buffer;
				const dataLength = data.length;
				if (n > dataLength) {
					out.set(data, retLen - n);
					n -= dataLength;
					buf[idx++] = null;
				} else {
					if (n === dataLength) {
						out.set(data, retLen - n);
						buf[idx++] = null;
					} else {
						out.set(new FastBuffer(data.buffer, data.byteOffset, n), retLen - n);
						buf[idx] = new FastBuffer(data.buffer, data.byteOffset + n, dataLength - n);
					}
					break;
				}
			}
			ret = out;
		}
	}

	if (idx === len) {
		state.buffer.length = 0;
		state.bufferIndex = 0;
	} else if (idx > 1024) {
		state.buffer.splice(0, idx);
		state.bufferIndex = 0;
	} else {
		state.bufferIndex = idx;
	}

	return ret;
}

function endReadable(stream: ReadableImpl): void {
	const state = stream._readableState;
	if ((state[kState] & (kEndEmitted | kEndScheduled)) === 0) {
		state[kState] |= kEnded | kEndScheduled;
		nextTick(endReadableNT as Listener, state, stream);
	}
}

function endReadableNT(state: ReadableState, stream: ReadableImpl): void {
	// The scheduled tick is running; allow endReadable() to schedule again.
	state[kState] &= ~kEndScheduled;

	// Check that we didn't get one last unshift.
	if ((state[kState] & (kErrored | kCloseEmitted | kEndEmitted)) === 0 && state.length === 0) {
		state[kState] |= kEndEmitted;
		stream.emit("end");

		if (stream.writable && stream.allowHalfOpen === false) {
			nextTick(endWritableNT as Listener, stream);
		} else if (state.autoDestroy) {
			// In case of duplex streams we need a way to detect
			// if the writable side is ready for autoDestroy as well.
			const wState = stream._writableState;
			const autoDestroy =
				!wState ||
				(wState.autoDestroy &&
					// We don't expect the writable to ever 'finish'
					// if writable is explicitly set to false.
					(wState.finished || wState.writable === false));

			if (autoDestroy) stream.destroy();
		}
	}
}

function endWritableNT(stream: ReadableImpl): void {
	const writable = stream.writable && !stream.writableEnded && !stream.destroyed;
	if (writable) (stream.end as () => void)();
}

Readable.from = function (iterable: unknown, opts?: ReadableOptions): ReadableImpl {
	return from(Readable, iterable, opts) as ReadableImpl;
};

Readable.fromWeb = function (readableStream: unknown, options?: unknown): ReadableImpl {
	// adapters -> Duplex reads Readable.prototype during evaluation.
	const adapters = require("internal:webstreams/adapters") as typeof adaptersModule;
	return adapters.newStreamReadableFromReadableStream(readableStream, options) as ReadableImpl;
};

Readable.toWeb = function (streamReadable: unknown, options?: unknown): unknown {
	// adapters -> Duplex reads Readable.prototype during evaluation.
	const adapters = require("internal:webstreams/adapters") as typeof adaptersModule;
	return adapters.newReadableStreamFromStreamReadable(streamReadable, options);
};

Readable.wrap = function (src: unknown, options?: ReadableOptions): ReadableImpl {
	const s = src as { readableObjectMode?: boolean; objectMode?: boolean };
	const r = new Readable({
		objectMode: s.readableObjectMode ?? s.objectMode ?? true,
		...options,
		destroy(err: Error | null, callback: Callback) {
			destroyImpl.destroyer(src as never, err);
			callback(err);
		},
	});
	return (r as unknown as { wrap(s: unknown): ReadableImpl }).wrap(src);
};
