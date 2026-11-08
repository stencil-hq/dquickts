// node:zlib — bounded native engines, with Transform-controlled backpressure.
import * as binding from "qbun:zlib";
import { Buffer, kMaxLength } from "node:buffer";
import { Transform } from "node:stream";
import { nextTick } from "node:process";
import { bind } from "internal:async-context";
import { codedError, invalidArgType, outOfRange } from "internal:errors";

type Output = Buffer;
export type InputType = string | ArrayBuffer | SharedArrayBuffer | ArrayBufferView;
export interface ZlibOptions {
	flush?: number;
	finishFlush?: number;
	chunkSize?: number;
	windowBits?: number;
	level?: number;
	memLevel?: number;
	strategy?: number;
	dictionary?: ArrayBuffer | SharedArrayBuffer | ArrayBufferView;
	maxOutputLength?: number;
	info?: boolean;
	params?: Record<number, number | boolean>;
	pledgedSrcSize?: number;
	rejectGarbageAfterEnd?: boolean;
	highWaterMark?: number;
	readableHighWaterMark?: number;
	writableHighWaterMark?: number;
	encoding?: string | null;
	defaultEncoding?: string;
	objectMode?: boolean;
	readableObjectMode?: boolean;
	writableObjectMode?: boolean;
	decodeStrings?: boolean;
	autoDestroy?: boolean;
	emitClose?: boolean;
	signal?: AbortSignal;
}
export type BrotliOptions = ZlibOptions;
export type ZstdOptions = ZlibOptions;
interface Step {
	output: Output;
	consumed: number;
	done: boolean;
	pending: boolean;
}
interface NativeEngine {
	processSync(input: Uint8Array | null, flush: number): Step;
	process(input: Uint8Array | null, flush: number, cb: (error: Error | null, result?: Step) => void): void;
	params(level: number, strategy: number): Output;
	reset(): void;
	close(): void;
}
interface Native {
	constants: Record<string, number>;
	createEngine(mode: number, options?: ZlibOptions): NativeEngine;
	crc32(data: string | ArrayBufferView, value: number): number;
	deflateSync(input: Uint8Array, options: ZlibOptions): Output;
	inflateSync(input: Uint8Array, options: ZlibOptions): Output;
	gzipSync(input: Uint8Array, options: ZlibOptions): Output;
	gunzipSync(input: Uint8Array, options: ZlibOptions): Output;
	deflateRawSync(input: Uint8Array, options: ZlibOptions): Output;
	inflateRawSync(input: Uint8Array, options: ZlibOptions): Output;
	unzipSync(input: Uint8Array, options: ZlibOptions): Output;
	brotliCompress(input: Uint8Array, options: ZlibOptions): Output;
	brotliDecompress(input: Uint8Array, options: ZlibOptions): Output;
	zstdCompress(input: Uint8Array, options: ZlibOptions): Output;
	zstdDecompress(input: Uint8Array, options: ZlibOptions): Output;
}
const native = binding as unknown as Native;
export const constants = Object.freeze(native.constants);
export const codes: Readonly<Record<string | number, string | number>> = (() => {
	const result: Record<string | number, string | number> = {};
	for (const name of [
		"Z_OK",
		"Z_STREAM_END",
		"Z_NEED_DICT",
		"Z_ERRNO",
		"Z_STREAM_ERROR",
		"Z_DATA_ERROR",
		"Z_MEM_ERROR",
		"Z_BUF_ERROR",
		"Z_VERSION_ERROR",
	]) {
		result[name] = constants[name];
		result[constants[name]] = name;
	}
	return Object.freeze(result);
})();

const DEFLATE = 1,
	INFLATE = 2,
	GZIP = 3,
	GUNZIP = 4,
	DEFLATERAW = 5,
	INFLATERAW = 6,
	UNZIP = 7,
	BROTLI_DECODE = 8,
	BROTLI_ENCODE = 9,
	ZSTD_COMPRESS = 10,
	ZSTD_DECOMPRESS = 11;
const empty = Buffer.alloc(0);
const kFlush = Symbol("zlib.flush");
type FlushChunk = Uint8Array & { [kFlush]?: number };
type StreamCallback = (error?: Error | null) => void;
export interface ZlibResult {
	buffer: Output;
	engine: ZlibBase;
}
export type CompressCallback = (error: Error | null, result?: Output | ZlibResult) => void;

function integer(value: unknown, name: string, min: number, max: number): number {
	if (typeof value !== "number") throw invalidArgType(name, "of type number", value);
	if (!Number.isInteger(value) || value < min || value > max) throw outOfRange(name, `>= ${min} and <= ${max}`, value);
	return value;
}
function numericOption(value: unknown, name: string, min: number, max: number, fallback: number): number {
	if (value === undefined || (typeof value === "number" && Number.isNaN(value))) return fallback;
	return integer(value, `options.${name}`, min, max);
}
function inputBytes(input: InputType): Uint8Array {
	if (typeof input === "string") return Buffer.from(input);
	if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
	if (input instanceof ArrayBuffer || (typeof SharedArrayBuffer !== "undefined" && input instanceof SharedArrayBuffer))
		return new Uint8Array(input);
	throw invalidArgType("buffer", "a string or an instance of Buffer, TypedArray, DataView, or ArrayBuffer", input);
}
function optionsFor(mode: number, options: ZlibOptions = {}): ZlibOptions {
	if (options === null || typeof options !== "object") throw invalidArgType("options", "of type object", options);
	const opts = { ...options };
	opts.chunkSize = numericOption(opts.chunkSize, "chunkSize", 64, 0xffffffff, 16384);
	opts.maxOutputLength = numericOption(opts.maxOutputLength, "maxOutputLength", 1, kMaxLength, kMaxLength);
	const maxFlush = mode <= UNZIP ? 5 : mode <= BROTLI_ENCODE ? 3 : 2;
	opts.flush = numericOption(opts.flush, "flush", 0, maxFlush, 0);
	opts.finishFlush = numericOption(opts.finishFlush, "finishFlush", 0, maxFlush, mode <= UNZIP ? 4 : 2);
	if (opts.rejectGarbageAfterEnd !== undefined && typeof opts.rejectGarbageAfterEnd !== "boolean")
		throw invalidArgType("options.rejectGarbageAfterEnd", "of type boolean", opts.rejectGarbageAfterEnd);
	if (opts.dictionary !== undefined) {
		if (typeof opts.dictionary === "string")
			throw invalidArgType(
				"options.dictionary",
				"an instance of Buffer, TypedArray, DataView, or ArrayBuffer",
				opts.dictionary,
			);
		opts.dictionary = inputBytes(opts.dictionary);
	}
	if (mode <= UNZIP) {
		const zeroBits = mode === INFLATE || mode === GUNZIP || mode === UNZIP;
		opts.windowBits = numericOption(
			opts.windowBits,
			"windowBits",
			zeroBits && opts.windowBits === 0 ? 0 : mode === GZIP ? 9 : 8,
			15,
			15,
		);
		if (opts.windowBits === 8 && mode === DEFLATERAW) opts.windowBits = 9;
		opts.level = numericOption(opts.level, "level", -1, 9, -1);
		opts.memLevel = numericOption(opts.memLevel, "memLevel", 1, 9, 8);
		opts.strategy = numericOption(opts.strategy, "strategy", 0, 4, 0);
	} else {
		if (opts.params !== undefined) {
			if (opts.params === null || typeof opts.params !== "object")
				throw invalidArgType("options.params", "of type object", opts.params);
			const params: Record<number, number | boolean> = {};
			const seen = new Set<number>();
			for (const key in opts.params) {
				if (!Object.prototype.hasOwnProperty.call(opts.params, key)) continue;
				const id = Number(key);
				const max = mode === BROTLI_ENCODE ? 9 : mode === BROTLI_DECODE ? 1 : mode === ZSTD_COMPRESS ? 402 : 100;
				if (!Number.isInteger(id) || id < 0 || id > max || seen.has(id))
					throw codedError(
						mode <= BROTLI_ENCODE ? "ERR_BROTLI_INVALID_PARAM" : "ERR_ZSTD_INVALID_PARAM",
						`${key} is not a valid compression parameter`,
						RangeError,
					);
				seen.add(id);
				const value = (opts.params as Record<string, number | boolean>)[key];
				if (typeof value !== "number" && typeof value !== "boolean")
					throw invalidArgType("options.params[key]", "of type number", value);
				params[id] = value;
			}
			opts.params = params;
		}
		if (opts.pledgedSrcSize !== undefined)
			integer(opts.pledgedSrcSize, "options.pledgedSrcSize", 0, Number.MAX_SAFE_INTEGER);
	}
	return opts;
}
function toError(error: unknown): Error {
	return error instanceof Error ? error : new Error(String(error));
}

/** Shared implementation: every native step produces at most chunkSize bytes. */
class ZlibBase extends Transform {
	bytesWritten = 0;
	_handle: NativeEngine | null;
	readonly _mode: number;
	readonly _options: ZlibOptions;
	readonly _defaultFlushFlag: number;
	readonly _finishFlushFlag: number;
	readonly _defaultFullFlushFlag: number;
	private _resumeOutput: (() => void) | null = null;
	private _activeCallback: StreamCallback | null = null;
	constructor(mode: number, options?: ZlibOptions) {
		const opts = optionsFor(mode, options);
		super({
			...opts,
			encoding: null,
			objectMode: false,
			readableObjectMode: false,
			writableObjectMode: false,
			decodeStrings: true,
			autoDestroy: opts.autoDestroy ?? true,
		});
		this._mode = mode;
		this._options = opts;
		this._defaultFlushFlag = opts.flush!;
		this._finishFlushFlag = opts.finishFlush!;
		this._defaultFullFlushFlag = mode <= UNZIP ? 3 : 1;
		this._handle = native.createEngine(mode, opts);
	}
	get bytesRead(): number {
		return this.bytesWritten;
	}
	set bytesRead(value: number) {
		this.bytesWritten = value;
	}
	get _closed(): boolean {
		return this._handle === null;
	}
	reset(): void {
		if (!this._handle) throw codedError("ERR_STREAM_DESTROYED", "Cannot call reset after a stream was destroyed");
		this._handle.reset();
	}
	_read(size: number): void {
		const resume = this._resumeOutput;
		if (resume) {
			this._resumeOutput = null;
			resume();
		} else super._read(size);
	}
	_transform(chunk: FlushChunk | string, encoding: string, callback: StreamCallback): void {
		let data: Uint8Array;
		try {
			data = typeof chunk === "string" ? Buffer.from(chunk, encoding) : inputBytes(chunk);
		} catch (error) {
			callback(toError(error));
			return;
		}
		const flag = typeof chunk === "object" ? chunk[kFlush] : undefined;
		this._process(data, flag ?? this._defaultFlushFlag, callback);
	}
	_flush(callback: StreamCallback): void {
		this._process(empty, this._finishFlushFlag, callback);
	}
	private _process(data: Uint8Array, flush: number, callback: StreamCallback): void {
		let offset = 0;
		let completed = false;
		const finish = (error?: Error | null) => {
			if (completed) return;
			completed = true;
			this._activeCallback = null;
			this._resumeOutput = null;
			callback(error);
		};
		this._activeCallback = finish;
		const step = () => {
			if (completed) return;
			const handle = this._handle;
			if (!handle || this.destroyed) {
				finish(codedError("ERR_STREAM_DESTROYED", "Cannot call write after a stream was destroyed"));
				return;
			}
			try {
				handle.process(data.subarray(offset), flush, (error, result) => {
					if (completed) return;
					if (error) {
						finish(error);
						return;
					}
					if (this.destroyed) {
						finish(codedError("ERR_STREAM_DESTROYED", "Stream was destroyed"));
						return;
					}
					const r = result!;
					offset += r.consumed;
					this.bytesWritten += r.consumed;
					const more = r.pending || (offset < data.length && !(r.done && r.consumed === 0 && r.output.length === 0));
					const advance = () => {
						if (more) step();
						else finish();
					};
					if (r.output.length && !this.push(r.output)) this._resumeOutput = advance;
					else advance();
				});
			} catch (error) {
				finish(toError(error));
			}
		};
		step();
	}
	_destroy(error: Error | null, callback: StreamCallback): void {
		const handle = this._handle;
		this._handle = null;
		this._resumeOutput = null;
		if (handle) handle.close();
		const active = this._activeCallback;
		if (active) active(error ?? codedError("ERR_STREAM_DESTROYED", "Stream was destroyed"));
		callback(error);
	}
	flush(kind?: number | StreamCallback, callback?: StreamCallback): void {
		if (typeof kind === "function") {
			callback = kind;
			kind = undefined;
		}
		kind = kind ?? this._defaultFullFlushFlag;
		integer(kind, "kind", 0, this._mode <= UNZIP ? 5 : this._mode <= BROTLI_ENCODE ? 3 : 2);
		if (callback !== undefined && typeof callback !== "function")
			throw invalidArgType("callback", "of type function", callback);
		if (this.writableFinished || this.destroyed) {
			if (callback) nextTick(bind(callback));
			return;
		}
		if (this.writableEnded) {
			if (callback) this.once("end", bind(callback));
			return;
		}
		const chunk = Buffer.alloc(0) as FlushChunk;
		chunk[kFlush] = kind;
		this.write(chunk, callback ? bind(callback) : undefined);
	}
	close(callback?: () => void): void {
		if (callback !== undefined && typeof callback !== "function")
			throw invalidArgType("callback", "of type function", callback);
		if (callback) {
			if (this.closed) nextTick(bind(callback));
			else this.once("close", bind(callback));
		}
		this.destroy();
	}
	_processChunk(chunk: InputType, flush: number, callback?: StreamCallback): Output | void {
		const data = inputBytes(chunk);
		if (callback) {
			this._process(data, flush, callback);
			return;
		}
		if (!this._handle) throw codedError("ERR_STREAM_DESTROYED", "Compression binding is closed");
		const chunks: Uint8Array[] = [];
		let offset = 0,
			total = 0;
		try {
			for (;;) {
				const r = this._handle.processSync(data.subarray(offset), flush);
				offset += r.consumed;
				this.bytesWritten += r.consumed;
				if (r.output.length) {
					chunks.push(r.output);
					total += r.output.length;
				}
				if (!r.pending && (offset === data.length || (r.done && !r.consumed && !r.output.length))) break;
			}
			return chunks.length === 1 ? (chunks[0] as Output) : Buffer.concat(chunks, total);
		} finally {
			this._handle.close();
			this._handle = null;
		}
	}
}
class Zlib extends ZlibBase {
	params(level: number, strategy: number, callback?: StreamCallback): void {
		integer(level, "level", -1, 9);
		integer(strategy, "strategy", 0, 4);
		if (callback !== undefined && typeof callback !== "function")
			throw invalidArgType("callback", "of type function", callback);
		const cb = callback
			? bind(callback)
			: (error?: Error | null) => {
					if (error) this.destroy(error);
				};
		this.flush(2, (error?: Error | null) => {
			if (error) {
				cb(error);
				return;
			}
			let output: Output;
			try {
				if (!this._handle) throw codedError("ERR_STREAM_DESTROYED", "Compression binding is closed");
				output = this._handle.params(level, strategy);
			} catch (error) {
				cb(toError(error));
				return;
			}
			if (output.length) this.push(output);
			cb(null);
		});
	}
}
export class Deflate extends Zlib {
	constructor(options?: ZlibOptions) {
		super(DEFLATE, options);
	}
}
export class Inflate extends Zlib {
	constructor(options?: ZlibOptions) {
		super(INFLATE, options);
	}
}
export class Gzip extends Zlib {
	constructor(options?: ZlibOptions) {
		super(GZIP, options);
	}
}
export class Gunzip extends Zlib {
	constructor(options?: ZlibOptions) {
		super(GUNZIP, options);
	}
}
export class DeflateRaw extends Zlib {
	constructor(options?: ZlibOptions) {
		super(DEFLATERAW, options);
	}
}
export class InflateRaw extends Zlib {
	constructor(options?: ZlibOptions) {
		super(INFLATERAW, options);
	}
}
export class Unzip extends Zlib {
	constructor(options?: ZlibOptions) {
		super(UNZIP, options);
	}
}
export class BrotliCompress extends ZlibBase {
	constructor(options?: BrotliOptions) {
		super(BROTLI_ENCODE, options);
	}
}
export class BrotliDecompress extends ZlibBase {
	constructor(options?: BrotliOptions) {
		super(BROTLI_DECODE, options);
	}
}
export class ZstdCompress extends ZlibBase {
	constructor(options?: ZstdOptions) {
		super(ZSTD_COMPRESS, options);
	}
}
export class ZstdDecompress extends ZlibBase {
	constructor(options?: ZstdOptions) {
		super(ZSTD_DECOMPRESS, options);
	}
}
export const createDeflate = (options?: ZlibOptions): Deflate => new Deflate(options);
export const createInflate = (options?: ZlibOptions): Inflate => new Inflate(options);
export const createGzip = (options?: ZlibOptions): Gzip => new Gzip(options);
export const createGunzip = (options?: ZlibOptions): Gunzip => new Gunzip(options);
export const createDeflateRaw = (options?: ZlibOptions): DeflateRaw => new DeflateRaw(options);
export const createInflateRaw = (options?: ZlibOptions): InflateRaw => new InflateRaw(options);
export const createUnzip = (options?: ZlibOptions): Unzip => new Unzip(options);
export const createBrotliCompress = (options?: BrotliOptions): BrotliCompress => new BrotliCompress(options);
export const createBrotliDecompress = (options?: BrotliOptions): BrotliDecompress => new BrotliDecompress(options);
export const createZstdCompress = (options?: ZstdOptions): ZstdCompress => new ZstdCompress(options);
export const createZstdDecompress = (options?: ZstdOptions): ZstdDecompress => new ZstdDecompress(options);

const constructors = [
	Deflate,
	Inflate,
	Gzip,
	Gunzip,
	DeflateRaw,
	InflateRaw,
	Unzip,
	BrotliDecompress,
	BrotliCompress,
	ZstdCompress,
	ZstdDecompress,
];
const syncMethods = [
	native.deflateSync,
	native.inflateSync,
	native.gzipSync,
	native.gunzipSync,
	native.deflateRawSync,
	native.inflateRawSync,
	native.unzipSync,
	native.brotliDecompress,
	native.brotliCompress,
	native.zstdCompress,
	native.zstdDecompress,
];
function compressSync(mode: number, input: InputType, options?: ZlibOptions): Output | ZlibResult {
	const data = inputBytes(input);
	const opts = optionsFor(mode, options);
	if (mode === ZSTD_COMPRESS && opts.pledgedSrcSize === undefined) opts.pledgedSrcSize = data.byteLength;
	if (opts.info) {
		const engine = new constructors[mode - 1](opts);
		return { buffer: engine._processChunk(data, opts.finishFlush!) as Output, engine };
	}
	return syncMethods[mode - 1](data, opts);
}
function compressAsync(
	mode: number,
	input: InputType,
	options: ZlibOptions | CompressCallback | undefined,
	callback?: CompressCallback,
): void {
	if (typeof options === "function") {
		callback = options;
		options = undefined;
	}
	if (typeof callback !== "function") throw invalidArgType("callback", "of type function", callback);
	const cb = bind(callback);
	const data = inputBytes(input);
	const opts = optionsFor(mode, options);
	if (mode === ZSTD_COMPRESS && opts.pledgedSrcSize === undefined) opts.pledgedSrcSize = data.byteLength;
	const engine = new constructors[mode - 1](opts);
	const chunks: Uint8Array[] = [];
	let total = 0,
		called = false;
	const finish = (error: Error | null, result?: Output | ZlibResult) => {
		if (!called) {
			called = true;
			cb(error, result);
		}
	};
	engine.on("data", (chunk: Output) => {
		chunks.push(chunk);
		total += chunk.length;
	});
	engine.once("error", (error: Error) => finish(error));
	engine.once("end", () => {
		const buffer = chunks.length === 1 ? (chunks[0] as Output) : Buffer.concat(chunks, total);
		finish(null, opts.info ? { buffer, engine } : buffer);
	});
	engine.end(data);
}
export function deflateSync(input: InputType, options?: ZlibOptions): Output | ZlibResult {
	return compressSync(DEFLATE, input, options);
}
export function inflateSync(input: InputType, options?: ZlibOptions): Output | ZlibResult {
	return compressSync(INFLATE, input, options);
}
export function gzipSync(input: InputType, options?: ZlibOptions): Output | ZlibResult {
	return compressSync(GZIP, input, options);
}
export function gunzipSync(input: InputType, options?: ZlibOptions): Output | ZlibResult {
	return compressSync(GUNZIP, input, options);
}
export function deflateRawSync(input: InputType, options?: ZlibOptions): Output | ZlibResult {
	return compressSync(DEFLATERAW, input, options);
}
export function inflateRawSync(input: InputType, options?: ZlibOptions): Output | ZlibResult {
	return compressSync(INFLATERAW, input, options);
}
export function unzipSync(input: InputType, options?: ZlibOptions): Output | ZlibResult {
	return compressSync(UNZIP, input, options);
}
export function brotliCompressSync(input: InputType, options?: BrotliOptions): Output | ZlibResult {
	return compressSync(BROTLI_ENCODE, input, options);
}
export function brotliDecompressSync(input: InputType, options?: BrotliOptions): Output | ZlibResult {
	return compressSync(BROTLI_DECODE, input, options);
}
export function zstdCompressSync(input: InputType, options?: ZstdOptions): Output | ZlibResult {
	return compressSync(ZSTD_COMPRESS, input, options);
}
export function zstdDecompressSync(input: InputType, options?: ZstdOptions): Output | ZlibResult {
	return compressSync(ZSTD_DECOMPRESS, input, options);
}
export function deflate(input: InputType, options: ZlibOptions | CompressCallback, callback?: CompressCallback): void {
	compressAsync(DEFLATE, input, options, callback);
}
export function inflate(input: InputType, options: ZlibOptions | CompressCallback, callback?: CompressCallback): void {
	compressAsync(INFLATE, input, options, callback);
}
export function gzip(input: InputType, options: ZlibOptions | CompressCallback, callback?: CompressCallback): void {
	compressAsync(GZIP, input, options, callback);
}
export function gunzip(input: InputType, options: ZlibOptions | CompressCallback, callback?: CompressCallback): void {
	compressAsync(GUNZIP, input, options, callback);
}
export function deflateRaw(
	input: InputType,
	options: ZlibOptions | CompressCallback,
	callback?: CompressCallback,
): void {
	compressAsync(DEFLATERAW, input, options, callback);
}
export function inflateRaw(
	input: InputType,
	options: ZlibOptions | CompressCallback,
	callback?: CompressCallback,
): void {
	compressAsync(INFLATERAW, input, options, callback);
}
export function unzip(input: InputType, options: ZlibOptions | CompressCallback, callback?: CompressCallback): void {
	compressAsync(UNZIP, input, options, callback);
}
export function brotliCompress(
	input: InputType,
	options: BrotliOptions | CompressCallback,
	callback?: CompressCallback,
): void {
	compressAsync(BROTLI_ENCODE, input, options, callback);
}
export function brotliDecompress(
	input: InputType,
	options: BrotliOptions | CompressCallback,
	callback?: CompressCallback,
): void {
	compressAsync(BROTLI_DECODE, input, options, callback);
}
export function zstdCompress(
	input: InputType,
	options: ZstdOptions | CompressCallback,
	callback?: CompressCallback,
): void {
	compressAsync(ZSTD_COMPRESS, input, options, callback);
}
export function zstdDecompress(
	input: InputType,
	options: ZstdOptions | CompressCallback,
	callback?: CompressCallback,
): void {
	compressAsync(ZSTD_DECOMPRESS, input, options, callback);
}
export function crc32(data: string | ArrayBufferView, value = 0): number {
	if (typeof data !== "string" && !ArrayBuffer.isView(data))
		throw invalidArgType("data", "a string or an instance of Buffer, TypedArray, or DataView", data);
	integer(value, "value", 0, 0xffffffff);
	return native.crc32(data, value);
}

// Legacy top-level zlib constants remain public, as in Node.
export const {
	Z_NO_FLUSH,
	Z_PARTIAL_FLUSH,
	Z_SYNC_FLUSH,
	Z_FULL_FLUSH,
	Z_FINISH,
	Z_BLOCK,
	Z_TREES,
	Z_OK,
	Z_STREAM_END,
	Z_NEED_DICT,
	Z_ERRNO,
	Z_STREAM_ERROR,
	Z_DATA_ERROR,
	Z_MEM_ERROR,
	Z_BUF_ERROR,
	Z_VERSION_ERROR,
	Z_NO_COMPRESSION,
	Z_BEST_SPEED,
	Z_BEST_COMPRESSION,
	Z_DEFAULT_COMPRESSION,
	Z_FILTERED,
	Z_HUFFMAN_ONLY,
	Z_RLE,
	Z_FIXED,
	Z_DEFAULT_STRATEGY,
	Z_BINARY,
	Z_TEXT,
	Z_ASCII,
	Z_UNKNOWN,
	Z_DEFLATED,
	Z_NULL,
	ZLIB_VERNUM,
	Z_DEFAULT_LEVEL,
	Z_MIN_WINDOWBITS,
	Z_MAX_WINDOWBITS,
	Z_DEFAULT_WINDOWBITS,
	Z_MIN_CHUNK,
	Z_MAX_CHUNK,
	Z_DEFAULT_CHUNK,
	Z_MIN_MEMLEVEL,
	Z_MAX_MEMLEVEL,
	Z_DEFAULT_MEMLEVEL,
	Z_MIN_LEVEL,
	Z_MAX_LEVEL,
} = constants;
const zlib: Record<string, unknown> = {
	Deflate,
	Inflate,
	Gzip,
	Gunzip,
	DeflateRaw,
	InflateRaw,
	Unzip,
	BrotliCompress,
	BrotliDecompress,
	ZstdCompress,
	ZstdDecompress,
	createDeflate,
	createInflate,
	createGzip,
	createGunzip,
	createDeflateRaw,
	createInflateRaw,
	createUnzip,
	createBrotliCompress,
	createBrotliDecompress,
	createZstdCompress,
	createZstdDecompress,
	deflate,
	inflate,
	gzip,
	gunzip,
	deflateRaw,
	inflateRaw,
	unzip,
	brotliCompress,
	brotliDecompress,
	zstdCompress,
	zstdDecompress,
	deflateSync,
	inflateSync,
	gzipSync,
	gunzipSync,
	deflateRawSync,
	inflateRawSync,
	unzipSync,
	brotliCompressSync,
	brotliDecompressSync,
	zstdCompressSync,
	zstdDecompressSync,
	constants,
	codes,
	crc32,
};
for (const name in constants) if (!name.startsWith("BROTLI")) zlib[name] = constants[name];
export default zlib;
