// internal:bun/image — Bun.Image: a lazy, chainable decode → transform → encode pipeline.
// Chainables only record settings (a second call overwrites the first); a terminal hands the
// input and the settings to qbun:image, which runs the pipeline on the libuv threadpool.
// Execution order is fixed like Bun's: autoOrient → rotate → flip/flop → resize → modulate.
import * as nativeModule from "qbun:image";
import process from "node:process";
import type * as BunFile from "internal:bun/file";
import { codedError, invalidArgType, invalidArgValue, validateNumber, validateObject } from "internal:errors";

type Format = "jpeg" | "png" | "webp" | "heic" | "avif" | "bmp" | "tiff" | "gif";
type OutputFormat = "jpeg" | "png" | "webp" | "heic" | "avif";
type Filter =
	| "nearest"
	| "box"
	| "bilinear"
	| "linear"
	| "cubic"
	| "mitchell"
	| "lanczos2"
	| "lanczos3"
	| "mks2013"
	| "mks2021";
type Backend = "system" | "bun";

interface ConstructorOptions {
	maxPixels?: number;
	autoOrient?: boolean;
}
interface ResizeOptions {
	filter?: Filter;
	fit?: "fill" | "inside";
	withoutEnlargement?: boolean;
}
interface ModulateOptions {
	brightness?: number;
	saturation?: number;
}
interface Metadata {
	width: number;
	height: number;
	format: Format;
}
interface BlobLike {
	arrayBuffer(): Promise<ArrayBuffer>;
}
type Input = string | ArrayBuffer | ArrayBufferView | BlobLike;

const MODE_METADATA = 0;
const MODE_ENCODE = 1;
const MODE_PLACEHOLDER = 2;

/** Settings handed to qbun:image (see node/image.c). */
interface NativeOptions {
	mode: number;
	maxPixels: number;
	autoOrient: boolean;
	rotate: number;
	flip: boolean;
	flop: boolean;
	resize: boolean;
	width: number;
	height: number;
	filter: number;
	inside: boolean;
	withoutEnlargement: boolean;
	modulate: boolean;
	brightness: number;
	saturation: number;
	format: OutputFormat | "";
	quality: number;
	progressive: boolean;
	compressionLevel: number;
	palette: boolean;
	colors: number;
	dither: boolean;
	lossless: boolean;
}
interface NativeResult {
	data: Uint8Array | undefined;
	format: Format;
	width: number;
	height: number;
}
interface NativeImage {
	/** On error only `err` is set. */
	run(
		input: string | ArrayBufferView,
		options: NativeOptions,
		callback: (err: Error | null, data: Uint8Array | undefined, format: Format, width: number, height: number) => void,
	): void;
	runSync(input: string | ArrayBufferView, options: NativeOptions): NativeResult;
}
// The C module's namespace is untyped to the engine; this is its shape.
const native = nativeModule as unknown as NativeImage;

const FILTERS: Record<Filter, number> = {
	nearest: 0,
	box: 1,
	bilinear: 2,
	linear: 2,
	cubic: 3,
	mitchell: 4,
	lanczos2: 5,
	lanczos3: 6,
	mks2013: 7,
	mks2021: 8,
};
const DEFAULT_MAX_PIXELS = 0x3fff * 0x3fff;
const EXTENSION_FORMATS: Record<string, OutputFormat> = {
	jpg: "jpeg",
	jpeg: "jpeg",
	png: "png",
	webp: "webp",
	heic: "heic",
	avif: "avif",
};

let backend: Backend = process.platform === "darwin" || process.platform === "win32" ? "system" : "bun";

/** A Buffer view of `bytes`. */
function asBuffer(bytes: Uint8Array): Uint8Array & { toString(encoding: string): string } {
	return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

function toBase64(bytes: Uint8Array): string {
	return asBuffer(bytes).toString("base64");
}

function plainBytes(bytes: Uint8Array): Uint8Array {
	return new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

function mimeType(format: Format): string {
	return `image/${format}`;
}

function isBlobLike(value: unknown): value is BlobLike {
	return (
		value !== null && typeof value === "object" && "arrayBuffer" in value && typeof value.arrayBuffer === "function"
	);
}

function checkBuffer(buffer: ArrayBufferLike): void {
	if (typeof SharedArrayBuffer !== "undefined" && buffer instanceof SharedArrayBuffer) {
		throw codedError(
			"ERR_INVALID_ARG_TYPE",
			"Bun.Image does not accept a SharedArrayBuffer; pass a copy (buf.slice())",
			TypeError,
		);
	}
	if (buffer instanceof ArrayBuffer && buffer.resizable) {
		throw codedError(
			"ERR_INVALID_ARG_TYPE",
			"Bun.Image does not accept a resizable ArrayBuffer; pass a fixed view (buf.slice())",
			TypeError,
		);
	}
}

function validateQuality(value: unknown, name: string, min: number): number {
	validateNumber(value, name, min, 100);
	return Math.round(value);
}

function optionalBoolean(value: unknown, name: string): boolean {
	if (value === undefined) return false;
	if (typeof value !== "boolean") throw invalidArgType(name, "of type boolean", value);
	return value;
}

function validateOptions<T extends object>(options: unknown, name: string): T | undefined {
	if (options === undefined || options === null) return undefined;
	validateObject(options, name);
	return options as T;
}

export class Image {
	static get backend(): Backend {
		return backend;
	}

	static set backend(value: Backend) {
		if (value !== "system" && value !== "bun") throw invalidArgValue("backend", value, 'must be "system" or "bun"');
		backend = value;
	}

	/** qbun has no clipboard backend: like Bun on Linux, there is never an image. */
	static fromClipboard(): Image | null {
		return null;
	}

	static hasClipboardImage(): boolean {
		return false;
	}

	static clipboardChangeCount(): number {
		return -1;
	}

	#input: Input;
	#maxPixels = DEFAULT_MAX_PIXELS;
	#autoOrient = true;
	#rotate = 0;
	#flip = false;
	#flop = false;
	#resize: { width: number; height: number; filter: number; inside: boolean; withoutEnlargement: boolean } | null =
		null;
	#modulate: { brightness: number; saturation: number } | null = null;
	#format: OutputFormat | "" = "";
	#quality = -1;
	#progressive = false;
	#compressionLevel = -1;
	#palette = false;
	#colors = 256;
	#dither = false;
	#lossless = false;
	#width = -1;
	#height = -1;

	constructor(input: Input, options?: ConstructorOptions) {
		if (typeof input === "string") {
			this.#input = input;
		} else if (
			input instanceof ArrayBuffer ||
			(typeof SharedArrayBuffer !== "undefined" && input instanceof SharedArrayBuffer)
		) {
			checkBuffer(input);
			this.#input = input;
		} else if (ArrayBuffer.isView(input)) {
			checkBuffer(input.buffer);
			this.#input = input;
		} else if (isBlobLike(input)) {
			this.#input = input;
		} else {
			throw invalidArgType("input", "a string, an ArrayBuffer, a TypedArray, or a Blob", input);
		}
		const opts = validateOptions<ConstructorOptions>(options, "options");
		if (opts?.maxPixels !== undefined) {
			validateNumber(opts.maxPixels, "options.maxPixels", 1);
			this.#maxPixels = opts.maxPixels;
		}
		if (opts?.autoOrient !== undefined) {
			if (typeof opts.autoOrient !== "boolean")
				throw invalidArgType("options.autoOrient", "of type boolean", opts.autoOrient);
			this.#autoOrient = opts.autoOrient;
		}
	}

	/** Output width after the first awaited terminal; `-1` before. */
	get width(): number {
		return this.#width;
	}

	/** Output height after the first awaited terminal; `-1` before. */
	get height(): number {
		return this.#height;
	}

	resize(width: number, height?: number, options?: ResizeOptions): this {
		let w = 0;
		let h = 0;
		if (width !== undefined && width !== null && width !== 0) {
			validateNumber(width, "width", 1);
			if (!Number.isFinite(width)) throw invalidArgValue("width", width, "must be a finite number");
			w = Math.max(1, Math.round(width));
		}
		if (height !== undefined && height !== null && height !== 0) {
			validateNumber(height, "height", 1);
			if (!Number.isFinite(height)) throw invalidArgValue("height", height, "must be a finite number");
			h = Math.max(1, Math.round(height));
		}
		if (!w && !h) throw invalidArgValue("width", width, "must be a positive number");
		const opts = validateOptions<ResizeOptions>(options, "options");
		let filter = FILTERS.lanczos3;
		if (opts?.filter !== undefined) {
			if (typeof opts.filter !== "string" || !Object.hasOwn(FILTERS, opts.filter)) {
				throw invalidArgValue("options.filter", opts.filter, `must be one of: ${Object.keys(FILTERS).join(", ")}`);
			}
			filter = FILTERS[opts.filter];
		}
		const fit = opts?.fit ?? "fill";
		if (fit !== "fill" && fit !== "inside") throw invalidArgValue("options.fit", fit, 'must be "fill" or "inside"');
		this.#resize = {
			width: w,
			height: h,
			filter,
			inside: fit === "inside",
			withoutEnlargement: optionalBoolean(opts?.withoutEnlargement, "options.withoutEnlargement"),
		};
		return this;
	}

	rotate(degrees: number): this {
		validateNumber(degrees, "degrees");
		if (!Number.isFinite(degrees) || degrees % 90 !== 0) {
			throw invalidArgValue("degrees", degrees, "must be a multiple of 90");
		}
		this.#rotate = ((degrees % 360) + 360) % 360;
		return this;
	}

	flip(): this {
		this.#flip = true;
		return this;
	}

	flop(): this {
		this.#flop = true;
		return this;
	}

	modulate(options: ModulateOptions): this {
		validateObject(options, "options");
		const brightness = options.brightness ?? 1;
		const saturation = options.saturation ?? 1;
		validateNumber(brightness, "options.brightness", 0);
		validateNumber(saturation, "options.saturation", 0);
		this.#modulate = { brightness, saturation };
		return this;
	}

	#setFormat(format: OutputFormat): void {
		this.#format = format;
		this.#quality = -1;
		this.#progressive = false;
		this.#compressionLevel = -1;
		this.#palette = false;
		this.#colors = 256;
		this.#dither = false;
		this.#lossless = false;
	}

	jpeg(options?: { quality?: number; progressive?: boolean }): this {
		const opts = validateOptions<{ quality?: number; progressive?: boolean }>(options, "options");
		this.#setFormat("jpeg");
		if (opts?.quality !== undefined) this.#quality = validateQuality(opts.quality, "options.quality", 1);
		this.#progressive = optionalBoolean(opts?.progressive, "options.progressive");
		return this;
	}

	png(options?: { compressionLevel?: number; palette?: boolean; colors?: number; dither?: boolean }): this {
		const opts = validateOptions<{ compressionLevel?: number; palette?: boolean; colors?: number; dither?: boolean }>(
			options,
			"options",
		);
		this.#setFormat("png");
		if (opts?.compressionLevel !== undefined) {
			validateNumber(opts.compressionLevel, "options.compressionLevel", 0, 9);
			this.#compressionLevel = Math.round(opts.compressionLevel);
		}
		this.#palette = optionalBoolean(opts?.palette, "options.palette");
		if (opts?.colors !== undefined) {
			validateNumber(opts.colors, "options.colors", 2, 256);
			this.#colors = Math.round(opts.colors);
		}
		this.#dither = optionalBoolean(opts?.dither, "options.dither");
		return this;
	}

	webp(options?: { quality?: number; lossless?: boolean }): this {
		const opts = validateOptions<{ quality?: number; lossless?: boolean }>(options, "options");
		this.#setFormat("webp");
		if (opts?.quality !== undefined) this.#quality = validateQuality(opts.quality, "options.quality", 0);
		this.#lossless = optionalBoolean(opts?.lossless, "options.lossless");
		return this;
	}

	heic(options?: { quality?: number }): this {
		const opts = validateOptions<{ quality?: number }>(options, "options");
		this.#setFormat("heic");
		if (opts?.quality !== undefined) this.#quality = validateQuality(opts.quality, "options.quality", 0);
		return this;
	}

	avif(options?: { quality?: number }): this {
		const opts = validateOptions<{ quality?: number }>(options, "options");
		this.#setFormat("avif");
		if (opts?.quality !== undefined) this.#quality = validateQuality(opts.quality, "options.quality", 0);
		return this;
	}

	#nativeOptions(mode: number, format: OutputFormat | ""): NativeOptions {
		const resize = this.#resize;
		const modulate = this.#modulate;
		return {
			mode,
			maxPixels: this.#maxPixels,
			autoOrient: this.#autoOrient,
			rotate: this.#rotate,
			flip: this.#flip,
			flop: this.#flop,
			resize: resize !== null,
			width: resize?.width ?? 0,
			height: resize?.height ?? 0,
			filter: resize?.filter ?? FILTERS.lanczos3,
			inside: resize?.inside ?? false,
			withoutEnlargement: resize?.withoutEnlargement ?? false,
			modulate: modulate !== null,
			brightness: modulate?.brightness ?? 1,
			saturation: modulate?.saturation ?? 1,
			format,
			quality: this.#quality,
			progressive: this.#progressive,
			compressionLevel: this.#compressionLevel,
			palette: this.#palette,
			colors: this.#colors,
			dither: this.#dither,
			lossless: this.#lossless,
		};
	}

	/** The input as a path, a byte view (checked for a transferred buffer) or a Blob. */
	#syncInput(): string | ArrayBufferView | BlobLike {
		const input = this.#input;
		if (typeof input === "string" || isBlobLike(input)) return input;
		const buffer = input instanceof ArrayBuffer ? input : input.buffer;
		if (buffer instanceof ArrayBuffer && buffer.detached) {
			throw codedError("ERR_INVALID_STATE", "The input ArrayBuffer of this Bun.Image was transferred");
		}
		return input instanceof ArrayBuffer ? new Uint8Array(input) : input;
	}

	/** The input for qbun:image: a Blob is read here (BunFile errors such as ENOENT surface as-is). */
	async #nativeInput(): Promise<string | ArrayBufferView> {
		const input = this.#syncInput();
		return isBlobLike(input) ? new Uint8Array(await input.arrayBuffer()) : input;
	}

	async #run(mode: number, format: OutputFormat | "" = this.#format): Promise<NativeResult> {
		const input = await this.#nativeInput();
		const { promise, resolve, reject } = Promise.withResolvers<NativeResult>();
		native.run(input, this.#nativeOptions(mode, format), (err, data, outFormat, width, height) => {
			if (err) reject(err);
			else resolve({ data, format: outFormat, width, height });
		});
		const result = await promise;
		this.#width = result.width;
		this.#height = result.height;
		return result;
	}

	async #encode(format?: OutputFormat | ""): Promise<{ data: Uint8Array; format: Format }> {
		const result = await this.#run(MODE_ENCODE, format);
		return { data: result.data as Uint8Array, format: result.format };
	}

	async bytes(): Promise<Uint8Array> {
		return plainBytes((await this.#encode()).data);
	}

	async buffer(): Promise<Uint8Array> {
		return asBuffer((await this.#encode()).data);
	}

	/** Sharp-compatible alias of buffer(). */
	toBuffer(): Promise<Uint8Array> {
		return this.buffer();
	}

	async blob(): Promise<Blob> {
		const { data, format } = await this.#encode();
		return new Blob([plainBytes(data)], { type: mimeType(format) });
	}

	async toBase64(): Promise<string> {
		return toBase64((await this.#encode()).data);
	}

	async dataurl(): Promise<string> {
		const { data, format } = await this.#encode();
		return `data:${mimeType(format)};base64,${toBase64(data)}`;
	}

	async write(dest: unknown): Promise<number> {
		let format = this.#format;
		if (format === "" && typeof dest === "string") {
			const dot = dest.lastIndexOf(".");
			const slash = Math.max(dest.lastIndexOf("/"), dest.lastIndexOf("\\"));
			if (dot > slash + 1) format = EXTENSION_FORMATS[dest.slice(dot + 1).toLowerCase()] ?? "";
		}
		const { data } = await this.#encode(format);
		// Buffer imports Image; defer fs-backed Bun.write so stream classes cannot read Buffer before initialization.
		const file: typeof BunFile = require("internal:bun/file");
		return file.write(dest, plainBytes(data));
	}

	async placeholder(as?: "dataurl"): Promise<string> {
		if (as !== undefined && as !== "dataurl") throw invalidArgValue("as", as, 'must be "dataurl"');
		const input = await this.#nativeInput();
		const { promise, resolve, reject } = Promise.withResolvers<Uint8Array>();
		native.run(input, this.#nativeOptions(MODE_PLACEHOLDER, "png"), (err, data) => {
			if (err) reject(err);
			else resolve(data as Uint8Array);
		});
		return `data:image/png;base64,${toBase64(await promise)}`;
	}

	async metadata(): Promise<Metadata> {
		const result = await this.#run(MODE_METADATA);
		return { width: result.width, height: result.height, format: result.format };
	}

	/**
	 * Synchronous encode for a Bun.Image used as a Response body (Bun encodes during body
	 * init). Blob inputs cannot be read synchronously and throw ERR_INVALID_STATE.
	 */
	[Symbol.for("qbun.image.encodeSync")](): { data: Uint8Array; type: string } {
		const input = this.#syncInput();
		if (isBlobLike(input)) {
			throw codedError("ERR_INVALID_STATE", "A Bun.Image backed by a Blob must be awaited before use as a body");
		}
		const result = native.runSync(input, this.#nativeOptions(MODE_ENCODE, this.#format));
		this.#width = result.width;
		this.#height = result.height;
		return { data: plainBytes(result.data as Uint8Array), type: mimeType(result.format) };
	}

	get [Symbol.toStringTag](): string {
		return "Image";
	}
}
