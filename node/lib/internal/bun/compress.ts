// Bun's DEFLATE helpers use raw DEFLATE by default (unlike node:zlib).
import * as binding from "qbun:zlib";
import type { Buffer } from "node:buffer";
import { bind } from "internal:async-context";
import { invalidArgType, outOfRange } from "internal:errors";

export interface ZlibCompressionOptions {
	level?: number;
	memLevel?: number;
	windowBits?: number;
	strategy?: number;
	library?: "zlib" | "libdeflate";
}
export interface ZstdCompressionOptions {
	level?: number;
}
type Input = string | ArrayBuffer | SharedArrayBuffer | ArrayBufferView;
interface NativeOptions extends ZlibCompressionOptions {
	params?: Record<number, number>;
	pledgedSrcSize?: number;
}
interface Step {
	output: Buffer;
	consumed: number;
	done: boolean;
	pending: boolean;
}
interface Engine {
	process(input: Uint8Array, flush: number, cb: (error: Error | null, result?: Step) => void): void;
	close(): void;
}
interface Native {
	createEngine(mode: number, options?: NativeOptions): Engine;
	deflateSync(input: Uint8Array, options: NativeOptions): Buffer;
	inflateSync(input: Uint8Array, options: NativeOptions): Buffer;
	gzipSync(input: Uint8Array, options: NativeOptions): Buffer;
	gunzipSync(input: Uint8Array, options: NativeOptions): Buffer;
	deflateRawSync(input: Uint8Array, options: NativeOptions): Buffer;
	inflateRawSync(input: Uint8Array, options: NativeOptions): Buffer;
	zstdCompress(input: Uint8Array, options: NativeOptions): Buffer;
	zstdDecompress(input: Uint8Array, options?: NativeOptions): Buffer;
}
const native = binding as unknown as Native;
function bytes(input: Input): Uint8Array {
	if (typeof input === "string") return Buffer.from(input);
	if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
	if (input instanceof ArrayBuffer || (typeof SharedArrayBuffer !== "undefined" && input instanceof SharedArrayBuffer))
		return new Uint8Array(input);
	throw invalidArgType("data", "a string or an instance of TypedArray, DataView, or ArrayBuffer", input);
}
function integer(value: unknown, name: string, min: number, max: number): number {
	if (typeof value !== "number") throw invalidArgType(name, "of type number", value);
	if (!Number.isInteger(value) || value < min || value > max) throw outOfRange(name, `>= ${min} and <= ${max}`, value);
	return value;
}
function zlibOptions(
	options: ZlibCompressionOptions | undefined,
	gzip: boolean,
): { options: NativeOptions; wrapper: number } {
	if (options !== undefined && (options === null || typeof options !== "object"))
		throw invalidArgType("options", "of type object", options);
	const opts: NativeOptions = { ...options };
	if (opts.library !== undefined && opts.library !== "zlib" && opts.library !== "libdeflate")
		throw invalidArgType("options.library", "'zlib' or 'libdeflate'", opts.library);
	if (opts.level !== undefined) {
		integer(
			opts.level,
			"options.level",
			opts.library === "libdeflate" ? 0 : -1,
			opts.library === "libdeflate" ? 12 : 9,
		);
		// qbun intentionally shares the zlib backend for both library selectors.
		if (opts.level > 9) opts.level = 9;
	}
	if (opts.memLevel !== undefined) integer(opts.memLevel, "options.memLevel", 1, 9);
	if (opts.strategy !== undefined) integer(opts.strategy, "options.strategy", 0, 4);
	const bits = opts.windowBits ?? (gzip ? 31 : -15);
	integer(bits, "options.windowBits", -15, 31);
	let wrapper: number;
	if (bits >= -15 && bits <= -9) {
		wrapper = 0;
		opts.windowBits = -bits;
	} else if (bits >= 9 && bits <= 15) {
		wrapper = 1;
		opts.windowBits = bits;
	} else if (bits >= 25 && bits <= 31) {
		wrapper = 2;
		opts.windowBits = bits - 16;
	} else throw outOfRange("options.windowBits", "-15..-9, 9..15, or 25..31", bits);
	return { options: opts, wrapper };
}
function zlibSync(
	input: Input,
	options: ZlibCompressionOptions | undefined,
	gzip: boolean,
	decode: boolean,
): Uint8Array {
	const config = zlibOptions(options, gzip);
	const data = bytes(input);
	const output =
		config.wrapper === 0
			? decode
				? native.inflateRawSync(data, config.options)
				: native.deflateRawSync(data, config.options)
			: config.wrapper === 1
				? decode
					? native.inflateSync(data, config.options)
					: native.deflateSync(data, config.options)
				: decode
					? native.gunzipSync(data, config.options)
					: native.gzipSync(data, config.options);
	return new Uint8Array(output.buffer, output.byteOffset, output.byteLength);
}
export function gzipSync(input: Input, options?: ZlibCompressionOptions): Uint8Array {
	return zlibSync(input, options, true, false);
}
export function gunzipSync(input: Input, options?: ZlibCompressionOptions): Uint8Array {
	return zlibSync(input, options, true, true);
}
export function deflateSync(input: Input, options?: ZlibCompressionOptions): Uint8Array {
	return zlibSync(input, options, false, false);
}
export function inflateSync(input: Input, options?: ZlibCompressionOptions): Uint8Array {
	return zlibSync(input, options, false, true);
}
function zstdOptions(options?: ZstdCompressionOptions): NativeOptions {
	if (options !== undefined && (options === null || typeof options !== "object"))
		throw invalidArgType("options", "of type object", options);
	const level = options?.level ?? 3;
	integer(level, "options.level", -131072, 22);
	return { params: { 100: level } };
}
export function zstdCompressSync(input: Input, options?: ZstdCompressionOptions): Buffer {
	// Register Buffer's native prototype before the binding allocates its output.
	void Buffer;
	const data = bytes(input);
	const opts = zstdOptions(options);
	opts.pledgedSrcSize = data.byteLength;
	return native.zstdCompress(data, opts);
}
export function zstdDecompressSync(input: Input): Buffer {
	// Register Buffer's native prototype before the binding allocates its output.
	void Buffer;
	return native.zstdDecompress(bytes(input));
}
function zstdAsync(input: Input, compress: boolean, options?: ZstdCompressionOptions): Promise<Buffer> {
	const { promise, resolve, reject } = Promise.withResolvers<Buffer>();
	let engine: Engine | undefined;
	try {
		// Register Buffer's native prototype before the binding allocates its output.
		void Buffer;
		const data = bytes(input);
		const opts: NativeOptions = compress ? zstdOptions(options) : {};
		if (compress) opts.pledgedSrcSize = data.byteLength;
		engine = native.createEngine(compress ? 10 : 11, opts);
		const handle = engine;
		const chunks: Uint8Array[] = [];
		let offset = 0,
			total = 0;
		const step = () => {
			try {
				handle.process(
					data.subarray(offset),
					2,
					bind((error: Error | null, result?: Step) => {
						if (error) {
							handle.close();
							reject(error);
							return;
						}
						const r = result!;
						offset += r.consumed;
						if (r.output.length) {
							chunks.push(r.output);
							total += r.output.length;
						}
						if (r.pending || (offset < data.length && !(r.done && !r.consumed && !r.output.length))) {
							step();
							return;
						}
						handle.close();
						try {
							resolve(chunks.length === 1 ? (chunks[0] as Buffer) : Buffer.concat(chunks, total));
						} catch (error) {
							reject(error);
						}
					}),
				);
			} catch (error) {
				handle.close();
				reject(error);
			}
		};
		step();
	} catch (error) {
		engine?.close();
		reject(error);
	}
	return promise;
}
export function zstdCompress(input: Input, options?: ZstdCompressionOptions): Promise<Buffer> {
	return zstdAsync(input, true, options);
}
export function zstdDecompress(input: Input): Promise<Buffer> {
	return zstdAsync(input, false);
}
