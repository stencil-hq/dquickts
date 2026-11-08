// Port of Node's lib/internal/webstreams/encoding.js, using the global
// TextEncoder/TextDecoder, with WebIDL brand checks on every accessor.

import {
	ERR_INVALID_ARG_TYPE,
	ERR_INVALID_THIS,
	customInspectSymbol as kInspect,
	kEmptyObject,
	kEnumerableProperty,
} from "internal:streams/shared";
import { customInspect, getNonWritablePropertyDescriptor } from "internal:webstreams/util";
import { TransformStream, TransformStreamDefaultController } from "internal:webstreams/transformstream";
import type { ReadableStream } from "internal:webstreams/readablestream";
import type { WritableStream } from "internal:webstreams/writablestream";

// Shared per-chunk decode options; decode() only reads the flag.
const kDecodeStreamingOptions = { __proto__: null, stream: true } as TextDecodeOptions;

type InspectOptions = { depth?: number | null };

export class TextEncoderStream {
	#pendingHighSurrogate: string | null = null;
	#handle: TextEncoder;
	#transform: TransformStream;

	constructor() {
		this.#handle = new TextEncoder();
		this.#transform = new TransformStream({
			transform: (chunk: unknown, controller: TransformStreamDefaultController) => {
				// https://encoding.spec.whatwg.org/#encode-and-enqueue-a-chunk
				// The only cross-chunk state is a trailing high surrogate;
				// encode() replaces interior lone surrogates with U+FFFD.
				let text = String(chunk);
				if (text.length === 0) return;
				if (this.#pendingHighSurrogate !== null) {
					text = this.#pendingHighSurrogate + text;
					this.#pendingHighSurrogate = null;
				}
				const lastCodeUnit = text.charCodeAt(text.length - 1);
				if (0xd800 <= lastCodeUnit && lastCodeUnit <= 0xdbff) {
					this.#pendingHighSurrogate = text.slice(-1);
					text = text.slice(0, -1);
					if (text.length === 0) return;
				}
				controller.enqueue(this.#handle.encode(text));
			},
			flush: (controller: TransformStreamDefaultController) => {
				// https://encoding.spec.whatwg.org/#encode-and-flush
				if (this.#pendingHighSurrogate !== null) {
					controller.enqueue(new Uint8Array([0xef, 0xbf, 0xbd]));
				}
			},
		});
	}

	get encoding(): string {
		if (!(#handle in this)) throw new ERR_INVALID_THIS("TextEncoderStream");
		return this.#handle.encoding;
	}

	get readable(): ReadableStream {
		if (!(#handle in this)) throw new ERR_INVALID_THIS("TextEncoderStream");
		return this.#transform.readable;
	}

	get writable(): WritableStream {
		if (!(#handle in this)) throw new ERR_INVALID_THIS("TextEncoderStream");
		return this.#transform.writable;
	}

	[kInspect](depth: number, options: InspectOptions): unknown {
		if (this == null || !(#handle in this)) throw new ERR_INVALID_THIS("TextEncoderStream");
		return customInspect.call(this, depth, options, "TextEncoderStream", {
			encoding: this.#handle.encoding,
			readable: this.#transform.readable,
			writable: this.#transform.writable,
		});
	}
}

export class TextDecoderStream {
	#handle: TextDecoder;
	#transform: TransformStream;

	constructor(encoding = "utf-8", options: TextDecoderOptions = kEmptyObject) {
		this.#handle = new TextDecoder(encoding, options);
		this.#transform = new TransformStream({
			transform: (chunk: unknown, controller: TransformStreamDefaultController) => {
				if (chunk === undefined) {
					throw new ERR_INVALID_ARG_TYPE("chunk", "string", chunk);
				}
				const value = this.#handle.decode(chunk as BufferSource, kDecodeStreamingOptions);
				if (value) controller.enqueue(value);
			},
			flush: (controller: TransformStreamDefaultController) => {
				const value = this.#handle.decode();
				if (value) controller.enqueue(value);
				controller.terminate();
			},
		});
	}

	get encoding(): string {
		if (!(#handle in this)) throw new ERR_INVALID_THIS("TextDecoderStream");
		return this.#handle.encoding;
	}

	get fatal(): boolean {
		if (!(#handle in this)) throw new ERR_INVALID_THIS("TextDecoderStream");
		return this.#handle.fatal;
	}

	get ignoreBOM(): boolean {
		if (!(#handle in this)) throw new ERR_INVALID_THIS("TextDecoderStream");
		return this.#handle.ignoreBOM;
	}

	get readable(): ReadableStream {
		if (!(#handle in this)) throw new ERR_INVALID_THIS("TextDecoderStream");
		return this.#transform.readable;
	}

	get writable(): WritableStream {
		if (!(#handle in this)) throw new ERR_INVALID_THIS("TextDecoderStream");
		return this.#transform.writable;
	}

	[kInspect](depth: number, options: InspectOptions): unknown {
		if (this == null || !(#handle in this)) throw new ERR_INVALID_THIS("TextDecoderStream");
		return customInspect.call(this, depth, options, "TextDecoderStream", {
			encoding: this.#handle.encoding,
			fatal: this.#handle.fatal,
			ignoreBOM: this.#handle.ignoreBOM,
			readable: this.#transform.readable,
			writable: this.#transform.writable,
		});
	}
}

Object.defineProperties(TextEncoderStream.prototype, {
	encoding: kEnumerableProperty,
	readable: kEnumerableProperty,
	writable: kEnumerableProperty,
	[Symbol.toStringTag]: getNonWritablePropertyDescriptor("TextEncoderStream"),
});

Object.defineProperties(TextDecoderStream.prototype, {
	encoding: kEnumerableProperty,
	fatal: kEnumerableProperty,
	ignoreBOM: kEnumerableProperty,
	readable: kEnumerableProperty,
	writable: kEnumerableProperty,
	[Symbol.toStringTag]: getNonWritablePropertyDescriptor("TextDecoderStream"),
});
