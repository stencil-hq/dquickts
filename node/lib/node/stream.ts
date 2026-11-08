// node:stream (Node's lib/stream.js). `require("stream")` is the legacy
// Stream class carrying every stream class and helper as a property.

import { Buffer } from "node:buffer";
import { Stream } from "internal:streams/legacy";
import { Readable } from "internal:streams/readable";
import { Writable } from "internal:streams/writable";
import { Duplex } from "internal:streams/duplex";
import { Transform } from "internal:streams/transform";
import { PassThrough } from "internal:streams/passthrough";
import { duplexPair } from "internal:streams/duplexpair";
import { pipeline } from "internal:streams/pipeline";
import { compose } from "internal:streams/compose";
import { addAbortSignal } from "internal:streams/add-abort-signal";
import { eos } from "internal:streams/end-of-stream";
import { destroyer } from "internal:streams/destroy";
import { getDefaultHighWaterMark, setDefaultHighWaterMark } from "internal:streams/state";
import { promiseReturningOperators, streamReturningOperators } from "internal:streams/operators";
import { isDestroyed, isDisturbed, isErrored, isReadable, isWritable } from "internal:streams/utils";
import { ERR_ILLEGAL_CONSTRUCTOR, isArrayBufferView, isUint8Array } from "internal:streams/shared";
import promises from "node:stream/promises";

type Operator = (this: unknown, ...args: unknown[]) => unknown;
type BufferCtor = { new (buffer?: ArrayBufferLike, byteOffset?: number, length?: number): Buffer };

const FastBuffer = (Buffer as unknown as { [Symbol.species]: BufferCtor })[Symbol.species];
const customPromisify = Symbol.for("nodejs.util.promisify.custom");

function defineOperator(key: string, op: Operator, wrap: (result: unknown) => unknown): void {
	const fn = function (this: unknown, ...args: unknown[]): unknown {
		if (new.target) throw new ERR_ILLEGAL_CONSTRUCTOR();
		return wrap(Reflect.apply(op, this, args));
	};
	Object.defineProperty(fn, "name", { value: op.name });
	Object.defineProperty(fn, "length", { value: op.length });
	Object.defineProperty(Readable.prototype, key, { value: fn, enumerable: false, configurable: true, writable: true });
}

const streamOps = streamReturningOperators as unknown as Record<string, Operator>;
for (const key in streamOps) defineOperator(key, streamOps[key], (result) => Readable.from(result));
const promiseOps = promiseReturningOperators as unknown as Record<string, Operator>;
for (const key in promiseOps) defineOperator(key, promiseOps[key], (result) => result);

function _uint8ArrayToBuffer(chunk: ArrayBufferView): Buffer {
	return new FastBuffer(chunk.buffer, chunk.byteOffset, chunk.byteLength);
}

const finished = eos;
const destroy = destroyer;

Object.defineProperty(pipeline, customPromisify, {
	enumerable: true,
	get() {
		return promises.pipeline;
	},
});

Object.defineProperty(eos, customPromisify, {
	enumerable: true,
	get() {
		return promises.finished;
	},
});

const S = Stream as unknown as Record<string, unknown>;
S.isDestroyed = isDestroyed;
S.isDisturbed = isDisturbed;
S.isErrored = isErrored;
S.isReadable = isReadable;
S.isWritable = isWritable;
S.Readable = Readable;
S.Writable = Writable;
S.Duplex = Duplex;
S.Transform = Transform;
S.PassThrough = PassThrough;
S.duplexPair = duplexPair;
S.pipeline = pipeline;
S.addAbortSignal = addAbortSignal;
S.finished = finished;
S.destroy = destroy;
S.compose = compose;
S.setDefaultHighWaterMark = setDefaultHighWaterMark;
S.getDefaultHighWaterMark = getDefaultHighWaterMark;
Object.defineProperty(Stream, "promises", {
	configurable: true,
	enumerable: true,
	get() {
		return promises;
	},
});
// Backwards-compat with node 0.4.x
S.Stream = Stream;
S._isArrayBufferView = isArrayBufferView;
S._isUint8Array = isUint8Array;
S._uint8ArrayToBuffer = _uint8ArrayToBuffer;

export {
	Stream,
	Readable,
	Writable,
	Duplex,
	Transform,
	PassThrough,
	duplexPair,
	pipeline,
	addAbortSignal,
	finished,
	destroy,
	compose,
	setDefaultHighWaterMark,
	getDefaultHighWaterMark,
	isDestroyed,
	isDisturbed,
	isErrored,
	isReadable,
	isWritable,
	promises,
	isArrayBufferView as _isArrayBufferView,
	isUint8Array as _isUint8Array,
	_uint8ArrayToBuffer,
};

export default Stream;
