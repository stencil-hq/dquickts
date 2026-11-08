// Port of Node's lib/internal/webstreams/compression.js, plus Bun's 'zstd'
// format and the Bun-only second argument `{ highWaterMark?, level? }`.

import * as zlib from "node:zlib";
import * as adapters from "internal:webstreams/adapters";
import {
	ERR_INVALID_ARG_TYPE,
	ERR_INVALID_THIS,
	ERR_OUT_OF_RANGE,
	customInspectSymbol as kInspect,
	isArrayBufferView,
	isSharedArrayBuffer,
	kEnumerableProperty,
	validateInteger,
	validateObject,
	kValidateObjectAllowObjectsAndNull,
} from "internal:streams/shared";
import { createEnumConverter, customInspect } from "internal:webstreams/util";
import type { ReadableStream } from "internal:webstreams/readablestream";
import type { WritableStream } from "internal:webstreams/writablestream";

type ReadableWritablePair = { readable: ReadableStream; writable: WritableStream };
type InspectOptions = { depth?: number | null };
type Format = "deflate" | "deflate-raw" | "gzip" | "brotli" | "zstd";

/** Bun extension: second constructor argument. */
interface CompressionOptions {
	highWaterMark?: number;
	level?: number;
}

// Per the Compression Streams spec, chunks must be BufferSource
// (ArrayBuffer or ArrayBufferView not backed by SharedArrayBuffer).
function validateBufferSourceChunk(chunk: unknown): void {
	if (isSharedArrayBuffer(isArrayBufferView(chunk) ? chunk.buffer : chunk)) {
		throw new ERR_INVALID_ARG_TYPE("chunk", ["ArrayBuffer", "Buffer", "TypedArray", "DataView"], chunk);
	}
}

const formatConverter = createEnumConverter("CompressionFormat", ["deflate", "deflate-raw", "gzip", "brotli", "zstd"]);

const kLevelRange: Record<Format, [number, number]> = {
	deflate: [0, 9],
	"deflate-raw": [0, 9],
	gzip: [0, 9],
	brotli: [0, 11],
	zstd: [1, 22],
};

interface ParsedOptions {
	highWaterMark: number | undefined;
	level: number | undefined;
}

function parseOptions(
	format: Format,
	options: CompressionOptions | null | undefined,
	withLevel: boolean,
): ParsedOptions {
	if (options === undefined || options === null) return { highWaterMark: undefined, level: undefined };
	validateObject(options, "options", kValidateObjectAllowObjectsAndNull);
	const highWaterMark = options.highWaterMark;
	if (highWaterMark !== undefined) validateInteger(highWaterMark, "options.highWaterMark", 1);
	const level = withLevel ? options.level : undefined;
	if (level !== undefined) {
		const [min, max] = kLevelRange[format];
		if (typeof level !== "number" || !Number.isInteger(level) || level < min || level > max)
			throw new ERR_OUT_OF_RANGE("options.level", `>= ${min} and <= ${max}`, level);
	}
	return { highWaterMark, level };
}

type ZlibOptions = Record<string, unknown>;

function compressorFor(format: Format, parsed: ParsedOptions): unknown {
	const opts: ZlibOptions = {};
	if (parsed.highWaterMark !== undefined) opts.chunkSize = parsed.highWaterMark;
	const { level } = parsed;
	switch (format) {
		case "deflate":
			if (level !== undefined) opts.level = level;
			return zlib.createDeflate(opts);
		case "deflate-raw":
			if (level !== undefined) opts.level = level;
			return zlib.createDeflateRaw(opts);
		case "gzip":
			if (level !== undefined) opts.level = level;
			return zlib.createGzip(opts);
		case "brotli":
			if (level !== undefined) opts.params = { [zlib.constants.BROTLI_PARAM_QUALITY]: level };
			return zlib.createBrotliCompress(opts);
		case "zstd":
			if (level !== undefined) opts.params = { [zlib.constants.ZSTD_c_compressionLevel]: level };
			return zlib.createZstdCompress(opts);
	}
}

function decompressorFor(format: Format, parsed: ParsedOptions): unknown {
	const opts: ZlibOptions = { rejectGarbageAfterEnd: true };
	if (parsed.highWaterMark !== undefined) opts.chunkSize = parsed.highWaterMark;
	switch (format) {
		case "deflate":
			return zlib.createInflate(opts);
		case "deflate-raw":
			return zlib.createInflateRaw(opts);
		case "gzip":
			return zlib.createGunzip(opts);
		case "brotli":
			return zlib.createBrotliDecompress(opts);
		case "zstd":
			return zlib.createZstdDecompress(opts);
	}
}

function pairFromDuplex(handle: unknown, highWaterMark: number | undefined): ReadableWritablePair {
	const options: Record<PropertyKey, unknown> = {
		[adapters.kValidateChunk]: validateBufferSourceChunk,
		[adapters.kDestroyOnSyncError]: true,
	};
	if (highWaterMark !== undefined) {
		options.readableHighWaterMark = highWaterMark;
		options.writableHighWaterMark = highWaterMark;
	}
	return adapters.newReadableWritablePairFromDuplex(
		handle as never,
		options as never,
	) as unknown as ReadableWritablePair;
}

export class CompressionStream {
	#handle: unknown;
	#transform: ReadableWritablePair;

	constructor(format: unknown, options?: CompressionOptions) {
		const fmt = formatConverter(format, {
			prefix: "Failed to construct 'CompressionStream'",
			context: "1st argument",
		}) as Format;
		const parsed = parseOptions(fmt, options, true);
		this.#handle = compressorFor(fmt, parsed);
		this.#transform = pairFromDuplex(this.#handle, parsed.highWaterMark);
	}

	get readable(): ReadableStream {
		if (!(#transform in this)) throw new ERR_INVALID_THIS("CompressionStream");
		return this.#transform.readable;
	}

	get writable(): WritableStream {
		if (!(#transform in this)) throw new ERR_INVALID_THIS("CompressionStream");
		return this.#transform.writable;
	}

	[kInspect](depth: number, options: InspectOptions): unknown {
		if (this == null || !(#transform in this)) throw new ERR_INVALID_THIS("CompressionStream");
		return customInspect.call(this, depth, options, "CompressionStream", {
			readable: this.#transform.readable,
			writable: this.#transform.writable,
		});
	}
}

export class DecompressionStream {
	#handle: unknown;
	#transform: ReadableWritablePair;

	constructor(format: unknown, options?: CompressionOptions) {
		const fmt = formatConverter(format, {
			prefix: "Failed to construct 'DecompressionStream'",
			context: "1st argument",
		}) as Format;
		const parsed = parseOptions(fmt, options, false);
		this.#handle = decompressorFor(fmt, parsed);
		this.#transform = pairFromDuplex(this.#handle, parsed.highWaterMark);
	}

	get readable(): ReadableStream {
		if (!(#transform in this)) throw new ERR_INVALID_THIS("DecompressionStream");
		return this.#transform.readable;
	}

	get writable(): WritableStream {
		if (!(#transform in this)) throw new ERR_INVALID_THIS("DecompressionStream");
		return this.#transform.writable;
	}

	[kInspect](depth: number, options: InspectOptions): unknown {
		if (this == null || !(#transform in this)) throw new ERR_INVALID_THIS("DecompressionStream");
		return customInspect.call(this, depth, options, "DecompressionStream", {
			readable: this.#transform.readable,
			writable: this.#transform.writable,
		});
	}
}

Object.defineProperties(CompressionStream.prototype, {
	readable: kEnumerableProperty,
	writable: kEnumerableProperty,
	[Symbol.toStringTag]: {
		__proto__: null,
		configurable: true,
		value: "CompressionStream",
	} as PropertyDescriptor,
});

Object.defineProperties(DecompressionStream.prototype, {
	readable: kEnumerableProperty,
	writable: kEnumerableProperty,
	[Symbol.toStringTag]: {
		__proto__: null,
		configurable: true,
		value: "DecompressionStream",
	} as PropertyDescriptor,
});
