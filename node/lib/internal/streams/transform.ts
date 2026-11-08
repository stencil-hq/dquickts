// Transform streams (Node's lib/internal/streams/transform.js).
//
// Copyright Joyent, Inc. and other Node contributors. MIT license.
//
// A transform stream is a readable/writable stream where you do something
// with the data. Implement _transform(chunk, encoding, callback) and,
// optionally, _flush(callback).

import { Duplex, type DuplexImpl, type DuplexOptions } from "internal:streams/duplex";
import { getHighWaterMark } from "internal:streams/state";
import { ERR_METHOD_NOT_IMPLEMENTED, nextTick } from "internal:streams/shared";

type Callback = (err?: unknown, data?: unknown) => void;

export interface TransformOptions extends DuplexOptions {
	transform?(this: TransformImpl, chunk: unknown, encoding: string, callback: Callback): void;
	flush?(this: TransformImpl, callback: Callback): void;
}

const kCallback = Symbol("kCallback");

export type TransformImpl = DuplexImpl & {
	_transform(chunk: unknown, encoding: string, callback: Callback): void;
	_flush?: (callback: Callback) => void;
	[kCallback]: (() => void) | null;
};

export interface TransformConstructor {
	new (options?: TransformOptions): TransformImpl;
	(this: unknown, options?: TransformOptions): void;
	prototype: TransformImpl;
	[key: string]: unknown;
}

export const Transform = function Transform(this: TransformImpl, options?: TransformOptions): TransformImpl | void {
	if (!(this instanceof Transform)) return new Transform(options);

	// A Duplex will buffer both on the writable and readable side while
	// a Transform just wants to buffer hwm number of elements. To avoid
	// buffering twice we disable buffering on the writable side when the
	// readable highWaterMark is 0.
	const readableHighWaterMark = options ? getHighWaterMark(this, options, "readableHighWaterMark", true) : null;
	if (readableHighWaterMark === 0) {
		options = {
			...options,
			highWaterMark: null as unknown as number,
			readableHighWaterMark,
			writableHighWaterMark: (options as TransformOptions).writableHighWaterMark || 0,
		};
	}

	Duplex.call(this, options);

	// We have implemented the _read method, and done the other things
	// that Readable wants before the first _read call, so unset the
	// sync guard flag.
	this._readableState.sync = false;

	this[kCallback] = null;

	if (options) {
		if (typeof options.transform === "function") this._transform = options.transform;
		if (typeof options.flush === "function") this._flush = options.flush;
	}

	// When the writable side finishes, then flush out anything remaining.
	// Backwards compat. Some Transform streams incorrectly implement _final
	// instead of or in addition to _flush. By using 'prefinish' instead of
	// implementing _final we continue supporting this unfortunate use case.
	this.on("prefinish", prefinish);
} as unknown as TransformConstructor;

Object.setPrototypeOf(Transform.prototype, Duplex.prototype);
Object.setPrototypeOf(Transform, Duplex);

function final(this: TransformImpl, cb?: (err?: unknown) => void): void {
	if (typeof this._flush === "function" && !this.destroyed) {
		this._flush((er, data) => {
			if (er) {
				if (cb) cb(er);
				else this.destroy(er);
				return;
			}
			if (data != null) this.push(data);
			this.push(null);
			if (cb) cb();
		});
	} else {
		this.push(null);
		if (cb) cb();
	}
}

function prefinish(this: TransformImpl): void {
	if (this._final !== final) final.call(this);
}

const proto = Transform.prototype as unknown as Record<string | symbol, unknown>;

proto._final = final;

proto._transform = function (_chunk: unknown, _encoding: string, _callback: Callback): void {
	throw new ERR_METHOD_NOT_IMPLEMENTED("_transform()");
};

proto._write = function (
	this: TransformImpl,
	chunk: unknown,
	encoding: string,
	callback: (err?: unknown) => void,
): void {
	const rState = this._readableState;
	const wState = this._writableState;
	const length = rState.length;

	this._transform(chunk, encoding, (err, val) => {
		if (err) {
			callback(err);
			return;
		}

		if (val != null) this.push(val);

		if (rState.ended) {
			// If user has called this.push(null) we have to
			// delay the callback to properly propagate the new state.
			nextTick(callback);
		} else if (
			wState.ended || // Backwards compat.
			length === rState.length || // Backwards compat.
			rState.length < rState.highWaterMark
		) {
			callback();
		} else {
			this[kCallback] = callback;
		}
	});
};

proto._read = function (this: TransformImpl): void {
	if (this[kCallback]) {
		const callback = this[kCallback];
		this[kCallback] = null;
		callback();
	}
};
