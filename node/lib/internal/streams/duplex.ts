// Duplex streams: Readable with the Writable interface mixed in
// (Node's lib/internal/streams/duplex.js).
//
// Copyright Joyent, Inc. and other Node contributors. MIT license.

import { Stream } from "internal:streams/legacy";
import { Readable, type ReadableImpl, type ReadableOptions } from "internal:streams/readable";
import { Writable, type WritableImpl, type WritableOptions } from "internal:streams/writable";
import { addAbortSignal } from "internal:streams/add-abort-signal";
import * as destroyImpl from "internal:streams/destroy";
import { kOnConstructed } from "internal:streams/utils";
import {
	newStreamDuplexFromReadableWritablePair,
	newReadableWritablePairFromDuplex,
} from "internal:webstreams/adapters";
import type * as duplexifyModule from "internal:streams/duplexify";

export interface DuplexOptions extends ReadableOptions, WritableOptions {
	allowHalfOpen?: boolean;
	readable?: boolean;
	writable?: boolean;
	[key: string]: unknown;
}

export type DuplexImpl = ReadableImpl & WritableImpl & { allowHalfOpen: boolean };

export interface DuplexConstructor {
	new (options?: DuplexOptions): DuplexImpl;
	(this: unknown, options?: DuplexOptions): void;
	prototype: DuplexImpl;
	from(body: unknown): DuplexImpl;
	fromWeb(pair: unknown, options?: unknown): DuplexImpl;
	toWeb(duplex: unknown, options?: unknown): { readable: unknown; writable: unknown };
	// inherited statics of Readable
	[key: string]: unknown;
}

export const Duplex = function Duplex(this: DuplexImpl, options?: DuplexOptions): DuplexImpl | void {
	if (!(this instanceof Duplex)) return new Duplex(options);

	this._readableState = new Readable.ReadableState(options, this, true);
	this._writableState = new Writable.WritableState(options, this, true);

	if (options) {
		this.allowHalfOpen = options.allowHalfOpen !== false;

		if (options.readable === false) {
			this._readableState.readable = false;
			this._readableState.ended = true;
			this._readableState.endEmitted = true;
		}

		if (options.writable === false) {
			this._writableState.writable = false;
			this._writableState.ending = true;
			this._writableState.ended = true;
			this._writableState.finished = true;
		}

		if (typeof options.read === "function") this._read = options.read as DuplexImpl["_read"];
		if (typeof options.write === "function") this._write = options.write as DuplexImpl["_write"];
		if (typeof options.writev === "function") this._writev = options.writev as DuplexImpl["_writev"];
		if (typeof options.destroy === "function") this._destroy = options.destroy as DuplexImpl["_destroy"];
		if (typeof options.final === "function") this._final = options.final as DuplexImpl["_final"];
		if (typeof options.construct === "function") this._construct = options.construct as DuplexImpl["_construct"];
		if (options.signal) addAbortSignal(options.signal, this);
	} else {
		this.allowHalfOpen = true;
	}

	Stream.call(this, options);

	if (this._construct != null) {
		destroyImpl.construct(this as never, () => {
			this._readableState[kOnConstructed](this);
			this._writableState[kOnConstructed](this);
		});
	}
} as unknown as DuplexConstructor;

Object.setPrototypeOf(Duplex.prototype, Readable.prototype);
Object.setPrototypeOf(Duplex, Readable);

{
	const dproto = Duplex.prototype as unknown as Record<string, unknown>;
	const wproto = Writable.prototype as unknown as Record<string, unknown>;
	for (const method in wproto) {
		if (Object.hasOwn(wproto, method)) dproto[method] ||= wproto[method];
	}
	// Use the `destroy` method of `Writable`.
	dproto.destroy = wproto.destroy;
}

function writableDescriptor(name: string): PropertyDescriptor {
	return { ...Object.getOwnPropertyDescriptor(Writable.prototype, name) };
}

Object.defineProperties(Duplex.prototype, {
	writable: writableDescriptor("writable"),
	writableHighWaterMark: writableDescriptor("writableHighWaterMark"),
	writableObjectMode: writableDescriptor("writableObjectMode"),
	writableBuffer: writableDescriptor("writableBuffer"),
	writableLength: writableDescriptor("writableLength"),
	writableFinished: writableDescriptor("writableFinished"),
	writableCorked: writableDescriptor("writableCorked"),
	writableEnded: writableDescriptor("writableEnded"),
	writableNeedDrain: writableDescriptor("writableNeedDrain"),

	destroyed: {
		get(this: DuplexImpl) {
			if (this._readableState === undefined || this._writableState === undefined) return false;
			return this._readableState.destroyed && this._writableState.destroyed;
		},
		set(this: DuplexImpl, value: boolean) {
			// Backward compatibility, the user is explicitly managing destroyed.
			if (this._readableState && this._writableState) {
				this._readableState.destroyed = value;
				this._writableState.destroyed = value;
			}
		},
	},
});

Duplex.fromWeb = function (pair: unknown, options?: unknown): DuplexImpl {
	return newStreamDuplexFromReadableWritablePair(pair, options) as DuplexImpl;
};

Duplex.toWeb = function (duplex: unknown, options?: unknown): { readable: unknown; writable: unknown } {
	return newReadableWritablePairFromDuplex(duplex, options) as { readable: unknown; writable: unknown };
};

Duplex.from = function (body: unknown): DuplexImpl {
	// duplexify defines a subclass of Duplex during evaluation.
	const { duplexify } = require("internal:streams/duplexify") as typeof duplexifyModule;
	return duplexify(body, "body") as DuplexImpl;
};
