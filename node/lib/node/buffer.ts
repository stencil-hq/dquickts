import { resolveObjectURL as resolveBlobURL } from "node:url";
import { Image } from "internal:bun/image";
import * as text from "qbun:text";
import * as core from "qbun:core";
import { codedError, invalidArgType, outOfRange } from "internal:errors";

export const kMaxLength = 0x1fffffffffffff;
export const kStringMaxLength = 0x1fffffe8;
export const constants = Object.freeze({ MAX_LENGTH: kMaxLength, MAX_STRING_LENGTH: kStringMaxLength });
export let INSPECT_MAX_BYTES = 50;
type Encoding = "utf8" | "utf16le" | "latin1" | "ascii" | "hex" | "base64" | "base64url";
function encoding(enc: unknown = "utf8"): Encoding {
	if (!enc) return "utf8";
	switch (String(enc).toLowerCase()) {
		case "utf8":
		case "utf-8":
			return "utf8";
		case "ucs2":
		case "ucs-2":
		case "utf16le":
		case "utf-16le":
			return "utf16le";
		case "binary":
		case "latin1":
			return "latin1";
		case "ascii":
			return "ascii";
		case "hex":
			return "hex";
		case "base64":
			return "base64";
		case "base64url":
			return "base64url";
	}
	throw codedError("ERR_UNKNOWN_ENCODING", `Unknown encoding: ${String(enc)}`, TypeError);
}
function integer(v: unknown, name: string, max: number, min = 0): number {
	if (typeof v !== "number") throw invalidArgType(name, "of type number", v);
	if (!Number.isInteger(v) || v < min || v > max) throw outOfRange(name, `>= ${min} and <= ${max}`, v);
	return v;
}
function size(v: unknown): number {
	return integer(v, "size", kMaxLength);
}
function bytes(v: unknown, name = "buffer"): Uint8Array {
	if (!(v instanceof Uint8Array)) throw invalidArgType(name, "an instance of Buffer or Uint8Array", v);
	return v;
}
function bound(b: Uint8Array, off: unknown, width: number): number {
	if (typeof off !== "number") throw invalidArgType("offset", "of type number", off);
	if (!Number.isInteger(off)) throw outOfRange("offset", "an integer", off);
	if (b.length < width)
		throw codedError("ERR_BUFFER_OUT_OF_BOUNDS", "Attempt to access memory outside buffer bounds", RangeError);
	return integer(off, "offset", b.length - width);
}
function view(b: Uint8Array): DataView {
	return new DataView(b.buffer, b.byteOffset, b.byteLength);
}
class FastBuffer extends Uint8Array {
	static get [Symbol.species]() {
		return FastBuffer;
	}
	toString(enc: unknown = "utf8", start = 0, end = this.length): string {
		start = Math.min(this.length, Math.max(0, Math.trunc(start) || 0));
		end = Math.min(this.length, Math.max(0, Math.trunc(end) || 0));
		return text.decode(this, encoding(enc), start, Math.max(start, end));
	}
	toLocaleString(): string {
		return this.toString();
	}
	write(
		str: string,
		offset: number | string = 0,
		length: number | string = this.length - (typeof offset === "number" ? offset : 0),
		enc: unknown = "utf8",
	): number {
		if (typeof str !== "string") throw invalidArgType("string", "of type string", str);
		if (typeof offset === "string") {
			enc = offset;
			offset = 0;
			length = this.length;
		} else {
			integer(offset, "offset", this.length);
			if (typeof length === "string") {
				enc = length;
				length = this.length - offset;
			} else integer(length, "length", this.length);
		}
		const n = Math.min(length as number, this.length - offset);
		const e = encoding(enc);
		if (e === "utf8") return text.utf8EncodeInto(str, this.subarray(offset, offset + n))[1];
		const data = text.encode(str, e) as Uint8Array;
		const count = e === "utf16le" ? Math.min(data.length, n - (n % 2)) : Math.min(data.length, n);
		this.set(data.subarray(0, count), offset);
		return count;
	}
	toJSON(): { type: "Buffer"; data: number[] } {
		return { type: "Buffer", data: Array.from(this) };
	}
	equals(other: Uint8Array): boolean {
		return text.compare(this, bytes(other, "otherBuffer")) === 0;
	}
	compare(
		other: Uint8Array,
		targetStart = 0,
		targetEnd = other.length,
		sourceStart = 0,
		sourceEnd = this.length,
	): number {
		bytes(other, "target");
		integer(targetStart, "targetStart", other.length);
		integer(targetEnd, "targetEnd", other.length);
		integer(sourceStart, "sourceStart", this.length);
		integer(sourceEnd, "sourceEnd", this.length);
		return text.compare(this.subarray(sourceStart, sourceEnd), other.subarray(targetStart, targetEnd));
	}
	copy(target: Uint8Array, targetStart = 0, sourceStart = 0, sourceEnd = this.length): number {
		bytes(target, "target");
		targetStart = Math.trunc(Number(targetStart)) || 0;
		sourceStart = Math.trunc(Number(sourceStart)) || 0;
		sourceEnd = Math.trunc(Number(sourceEnd)) || 0;
		if (targetStart < 0) throw outOfRange("targetStart", ">= 0", targetStart);
		if (sourceStart < 0 || sourceStart > this.length)
			throw outOfRange("sourceStart", `>= 0 && <= ${this.length}`, sourceStart);
		if (sourceEnd < 0) throw outOfRange("sourceEnd", ">= 0", sourceEnd);
		const n = Math.max(0, Math.min(sourceEnd, this.length) - sourceStart);
		const count = Math.max(0, Math.min(n, target.length - targetStart));
		if (count) target.set(this.subarray(sourceStart, sourceStart + count), targetStart);
		return count;
	}
	slice(start = 0, end = this.length): FastBuffer {
		return this.subarray(start, end);
	}
	subarray(start = 0, end = this.length): FastBuffer {
		const u = Uint8Array.prototype.subarray.call(this, start, end) as FastBuffer;
		Object.setPrototypeOf(u, FastBuffer.prototype);
		return u;
	}
	fill(value: unknown, start: number | string = 0, end: number | string = this.length, enc: unknown = "utf8"): this {
		if (typeof start === "string") {
			enc = start;
			start = 0;
			end = this.length;
		} else if (typeof end === "string") {
			enc = end;
			end = this.length;
		}
		integer(start, "offset", this.length);
		integer(end, "end", this.length);
		if (end <= start) return this;
		let p: Uint8Array;
		if (typeof value === "string") p = value.length ? Buffer.from(value, enc) : Buffer.of(0);
		else if (value instanceof Uint8Array) p = value;
		else p = Buffer.of(Number(value) & 255);
		text.fill(this, p, start, end);
		return this;
	}
	indexOf(value: unknown, offset: number | string = 0, enc: unknown = "utf8"): number {
		return search(this, value, offset, enc, false);
	}
	lastIndexOf(value: unknown, offset: number | string = this.length, enc: unknown = "utf8"): number {
		return search(this, value, offset, enc, true);
	}
	includes(value: unknown, offset: number | string = 0, enc: unknown = "utf8"): boolean {
		return this.indexOf(value, offset, enc) !== -1;
	}
	swap16(): this {
		text.swap16(this);
		return this;
	}
	swap32(): this {
		text.swap32(this);
		return this;
	}
	swap64(): this {
		text.swap64(this);
		return this;
	}
	get parent(): ArrayBufferLike {
		return this.buffer;
	}
	get offset(): number {
		return this.byteOffset;
	}
	[Symbol.for("nodejs.util.inspect.custom")](): string {
		const count = Math.min(INSPECT_MAX_BYTES, this.length);
		const hex = text.decode(this, "hex", 0, count) as string;
		const parts: string[] = [];
		for (let i = 0; i < hex.length; i += 2) parts.push(hex.slice(i, i + 2));
		return `<Buffer${parts.length ? " " + parts.join(" ") : ""}${this.length > count ? ` ... ${this.length - count} more byte${this.length - count === 1 ? "" : "s"}` : ""}>`;
	}
	readUIntLE(offset: number, byteLength: number): number {
		return readInteger(this, offset, byteLength, true, false);
	}
	readUIntBE(offset: number, byteLength: number): number {
		return readInteger(this, offset, byteLength, false, false);
	}
	readIntLE(offset: number, byteLength: number): number {
		return readInteger(this, offset, byteLength, true, true);
	}
	readIntBE(offset: number, byteLength: number): number {
		return readInteger(this, offset, byteLength, false, true);
	}
	writeUIntLE(value: number, offset: number, byteLength: number): number {
		return writeInteger(this, value, offset, byteLength, true, false);
	}
	writeUIntBE(value: number, offset: number, byteLength: number): number {
		return writeInteger(this, value, offset, byteLength, false, false);
	}
	writeIntLE(value: number, offset: number, byteLength: number): number {
		return writeInteger(this, value, offset, byteLength, true, true);
	}
	writeIntBE(value: number, offset: number, byteLength: number): number {
		return writeInteger(this, value, offset, byteLength, false, true);
	}
}
function readInteger(b: Uint8Array, off: number, width: number, le: boolean, signed: boolean): number {
	integer(width, "byteLength", 6, 1);
	bound(b, off, width);
	let n = 0;
	for (let i = 0; i < width; i++) n = n * 256 + b[off + (le ? width - i - 1 : i)];
	return signed && n >= 2 ** (width * 8 - 1) ? n - 2 ** (width * 8) : n;
}
function writeInteger(b: Uint8Array, value: number, off: number, width: number, le: boolean, signed: boolean): number {
	integer(width, "byteLength", 6, 1);
	value = +value;
	const max = 2 ** (width * 8 - (signed ? 1 : 0));
	if (value < (signed ? -max : 0) || value >= max)
		throw outOfRange("value", `>= ${signed ? -max : 0} and <= ${max - 1}`, value);
	bound(b, off, width);
	if (value < 0) value += 2 ** (width * 8);
	for (let i = 0; i < width; i++) {
		b[off + (le ? i : width - i - 1)] = value;
		value = Math.floor(value / 256);
	}
	return off + width;
}
// Install fixed-width numeric methods with one bounds-checking implementation.
const proto = FastBuffer.prototype as unknown as Record<string, unknown>;
for (const signed of [false, true])
	for (const width of [1, 2, 4])
		for (const le of [true, false]) {
			if (width === 1 && !le) continue;
			const suffix = `${signed ? "Int" : "UInt"}${width * 8}${width === 1 ? "" : le ? "LE" : "BE"}`;
			proto[`read${suffix}`] = function (this: FastBuffer, offset = 0) {
				return readInteger(this, offset, width, le, signed);
			};
			proto[`write${suffix}`] = function (this: FastBuffer, value: number, offset = 0) {
				return writeInteger(this, value, offset, width, le, signed);
			};
		}
for (const le of [true, false])
	for (const signed of [false, true]) {
		const suffix = `Big${signed ? "Int" : "UInt"}64${le ? "LE" : "BE"}`;
		proto[`read${suffix}`] = function (this: FastBuffer, offset = 0) {
			bound(this, offset, 8);
			return signed ? view(this).getBigInt64(offset, le) : view(this).getBigUint64(offset, le);
		};
		proto[`write${suffix}`] = function (this: FastBuffer, value: bigint, offset = 0) {
			if (typeof value !== "bigint") throw invalidArgType("value", "of type bigint", value);
			const min = signed ? -(1n << 63n) : 0n,
				max = (1n << (signed ? 63n : 64n)) - 1n;
			if (value < min || value > max) throw outOfRange("value", `>= ${min} and <= ${max}`, value);
			bound(this, offset, 8);
			if (signed) view(this).setBigInt64(offset, value, le);
			else view(this).setBigUint64(offset, value, le);
			return offset + 8;
		};
	}
for (const width of [4, 8])
	for (const le of [true, false]) {
		const suffix = `${width === 4 ? "Float" : "Double"}${le ? "LE" : "BE"}`;
		proto[`read${suffix}`] = function (this: FastBuffer, offset = 0) {
			bound(this, offset, width);
			return width === 4 ? view(this).getFloat32(offset, le) : view(this).getFloat64(offset, le);
		};
		proto[`write${suffix}`] = function (this: FastBuffer, value: number, offset = 0) {
			value = +value;
			bound(this, offset, width);
			if (width === 4) view(this).setFloat32(offset, value, le);
			else view(this).setFloat64(offset, value, le);
			return offset + width;
		};
	}
for (const name of Object.getOwnPropertyNames(proto))
	if (name.includes("UInt")) proto[name.replace("UInt", "Uint")] = proto[name];
for (const name of ["readUIntLE", "readUIntBE", "writeUIntLE", "writeUIntBE"])
	proto[name.replace("UInt", "Uint")] = proto[name];
function search(b: FastBuffer, value: unknown, offset: number | string, enc: unknown, back: boolean): number {
	if (typeof offset === "string") {
		enc = offset;
		offset = back ? b.length : 0;
	}
	let n = Number(offset);
	if (Number.isNaN(n)) n = back ? b.length : 0;
	n = Math.trunc(n);
	if (n < 0) n += b.length;
	const e = typeof value === "string" ? encoding(enc) : undefined;
	const needle =
		typeof value === "number"
			? Buffer.of(value & 255)
			: typeof value === "string"
				? Buffer.from(value, e)
				: bytes(value, "value");
	if (needle.length === 0) return Math.max(0, Math.min(b.length, n));
	if ((back && n < 0) || (!back && n >= b.length)) return -1;
	n = Math.max(0, Math.min(b.length, n));
	if (e === "utf16le") {
		if (b.length < 2 || needle.length < 2) return -1;
		const m = Math.floor(needle.length / 2) * 2;
		let pos = Math.floor(n / 2) * 2;
		if (back) pos = Math.min(pos, Math.floor((b.length - m) / 2) * 2);
		for (; back ? pos >= 0 : pos + m <= b.length; pos += back ? -2 : 2) {
			let match = true;
			for (let j = 0; j < m; j++)
				if (b[pos + j] !== needle[j]) {
					match = false;
					break;
				}
			if (match) return pos;
		}
		return -1;
	}
	return text.indexOf(b, needle, n, back);
}
function from(value: unknown, encOrOffset?: unknown, length?: number): FastBuffer {
	if (typeof value === "string") return text.encode(value, encoding(encOrOffset)) as FastBuffer;
	if (
		value instanceof ArrayBuffer ||
		(typeof SharedArrayBuffer !== "undefined" && value instanceof SharedArrayBuffer)
	) {
		const off = encOrOffset === undefined ? 0 : Math.trunc(Number(encOrOffset)) || 0;
		if (off < 0 || off > value.byteLength)
			throw codedError("ERR_BUFFER_OUT_OF_BOUNDS", '"offset" is outside of buffer bounds', RangeError);
		const len = length === undefined ? value.byteLength - off : Math.trunc(Number(length)) || 0;
		if (len < 0 || len + off > value.byteLength)
			throw codedError("ERR_BUFFER_OUT_OF_BOUNDS", '"length" is outside of buffer bounds', RangeError);
		return new FastBuffer(value, off, len);
	}
	if (value !== null && typeof value === "object") {
		const obj = value as {
			valueOf?: () => unknown;
			[Symbol.toPrimitive]?: (hint: string) => unknown;
			length?: number;
			type?: string;
			data?: number[];
		};
		const primitive = obj.valueOf?.();
		if (primitive !== undefined && primitive !== value) return from(primitive, encOrOffset, length);
		if (obj.type === "Buffer" && Array.isArray(obj.data)) return new FastBuffer(obj.data);
		if (ArrayBuffer.isView(value) || typeof obj.length === "number") return new FastBuffer(value as ArrayLike<number>);
		if ("length" in obj) return new FastBuffer(0);
		if (obj[Symbol.toPrimitive]) return from(obj[Symbol.toPrimitive]!("string"), encOrOffset, length);
	}
	throw invalidArgType(
		"first argument",
		"of type string or an instance of Buffer, ArrayBuffer, or Array or an Array-like Object",
		value,
	);
}
interface BufferConstructor {
	(value: unknown, encodingOrOffset?: unknown, length?: number): FastBuffer;
	new (value: unknown, encodingOrOffset?: unknown, length?: number): FastBuffer;
	prototype: FastBuffer;
	poolSize: number;
	from: typeof from;
	alloc(size: number, fill?: unknown, encoding?: string): FastBuffer;
	allocUnsafe(size: number): FastBuffer;
	allocUnsafeSlow(size: number): FastBuffer;
	isBuffer(value: unknown): value is FastBuffer;
	isEncoding(value: unknown): boolean;
	byteLength(value: unknown, encoding?: unknown): number;
	compare(a: Uint8Array, b: Uint8Array): number;
	concat(list: Uint8Array[], length?: number): FastBuffer;
	copyBytesFrom(view: ArrayBufferView, offset?: number, length?: number): FastBuffer;
	of(...values: number[]): FastBuffer;
}
export const Buffer = function Buffer(value: unknown, encOrOffset?: unknown, length?: number): FastBuffer {
	if (typeof value === "number") {
		if (typeof encOrOffset === "string") throw invalidArgType("string", "of type string", value);
		return new FastBuffer(size(value));
	}
	return from(value, encOrOffset, length);
} as BufferConstructor;
Buffer.prototype = FastBuffer.prototype;
Object.defineProperty(FastBuffer.prototype, "constructor", { value: Buffer, writable: true, configurable: true });
Object.setPrototypeOf(Buffer, Uint8Array);
Buffer.poolSize = 8192;
Buffer.from = from;
Buffer.alloc = (n, fill = 0, enc) => {
	const b = new FastBuffer(size(n));
	if (fill !== 0) b.fill(fill, 0, b.length, enc);
	return b;
};
Buffer.allocUnsafe = Buffer.allocUnsafeSlow = (n) => new FastBuffer(size(n));
Buffer.isBuffer = (v): v is FastBuffer => v instanceof FastBuffer;
Buffer.isEncoding = (v) => {
	if (typeof v !== "string" || v.length === 0) return false;
	try {
		encoding(v);
		return true;
	} catch {
		return false;
	}
};
Buffer.byteLength = (v, enc) => {
	if (
		v instanceof ArrayBuffer ||
		(typeof SharedArrayBuffer !== "undefined" && v instanceof SharedArrayBuffer) ||
		ArrayBuffer.isView(v)
	)
		return v.byteLength;
	if (typeof v !== "string")
		throw invalidArgType("string", "of type string or an instance of Buffer, ArrayBuffer, or ArrayBufferView", v);
	let e: Encoding;
	try {
		e = encoding(enc);
	} catch {
		e = "utf8";
	}
	switch (e) {
		case "utf8":
			return text.byteLengthUtf8(v);
		case "utf16le":
			return v.length * 2;
		case "hex":
			return v.length >>> 1;
		case "base64":
		case "base64url": {
			let n = v.length;
			if (n && v[n - 1] === "=") n--;
			if (n && v[n - 1] === "=") n--;
			return Math.floor((n * 3) / 4);
		}
		default:
			return v.length;
	}
};
Buffer.compare = (a, b) => text.compare(bytes(a, "buf1"), bytes(b, "buf2"));
Buffer.concat = (list, length) => {
	if (!Array.isArray(list)) throw invalidArgType("list", "an instance of Array", list);
	let total = 0;
	for (const b of list) total += bytes(b).length;
	const result = Buffer.alloc(length === undefined ? total : size(length));
	let offset = 0;
	for (const b of list) {
		const n = Math.min(b.length, result.length - offset);
		if (n <= 0) break;
		result.set(b.subarray(0, n), offset);
		offset += n;
	}
	return result;
};
Buffer.copyBytesFrom = (v, offset = 0, length?: number) => {
	if (!ArrayBuffer.isView(v) || v instanceof DataView) throw invalidArgType("view", "a TypedArray", v);
	const a = v as unknown as { length: number; BYTES_PER_ELEMENT: number; buffer: ArrayBufferLike; byteOffset: number };
	integer(offset, "offset", Number.MAX_SAFE_INTEGER);
	if (length !== undefined) integer(length, "length", Number.MAX_SAFE_INTEGER);
	const count = Math.max(0, Math.min(length ?? a.length - offset, a.length - offset));
	return from(
		new Uint8Array(
			a.buffer,
			a.byteOffset + Math.min(offset, a.length) * a.BYTES_PER_ELEMENT,
			count * a.BYTES_PER_ELEMENT,
		),
	);
};
Buffer.of = (...values) => new FastBuffer(values);
core.setBufferPrototype(Buffer.prototype);
export type Buffer = FastBuffer;
export function SlowBuffer(length: number): FastBuffer {
	return Buffer.alloc(+length || 0);
}
export function isUtf8(input: ArrayBuffer | ArrayBufferView): boolean {
	return text.isUtf8(input);
}
export function isAscii(input: ArrayBuffer | ArrayBufferView): boolean {
	return text.isAscii(input);
}
export function transcode(source: Uint8Array, fromEncoding: string, toEncoding: string): FastBuffer {
	bytes(source, "source");
	const a = encoding(fromEncoding),
		b = encoding(toEncoding);
	if (!["utf8", "utf16le", "latin1", "ascii"].includes(a) || !["utf8", "utf16le", "latin1", "ascii"].includes(b))
		throw codedError("U_ILLEGAL_ARGUMENT_ERROR", "Unable to transcode Buffer");
	let str: string;
	if (a === "ascii") {
		str = "";
		for (const byte of source) str += byte < 128 ? String.fromCharCode(byte) : "\ufffd";
	} else str = text.decode(source, a, 0, source.length) as string;
	if (b === "ascii" || b === "latin1") {
		const max = b === "ascii" ? 127 : 255;
		let converted = "";
		for (const c of str) converted += c.codePointAt(0)! > max ? "?" : c;
		str = converted;
	}
	return Buffer.from(str, b);
}
export const atob = globalThis.atob;
export const btoa = globalThis.btoa;
const blobData = Symbol("Blob.data");
export class Blob {
	readonly [blobData]: Uint8Array;
	readonly size: number;
	readonly type: string;
	constructor(parts: Iterable<unknown> = [], options: { type?: string; endings?: "transparent" | "native" } = {}) {
		if (parts === null || typeof parts !== "object" || typeof parts[Symbol.iterator] !== "function")
			throw invalidArgType("sources", "a sequence", parts);
		if (options.endings !== undefined && options.endings !== "transparent" && options.endings !== "native")
			throw codedError("ERR_INVALID_ARG_VALUE", "The property 'options.endings' is invalid", TypeError);
		const chunks: Uint8Array[] = [];
		let total = 0;
		for (const part of parts) {
			let b: Uint8Array;
			if (part instanceof Blob) b = part[blobData];
			else if (
				part instanceof ArrayBuffer ||
				(typeof SharedArrayBuffer !== "undefined" && part instanceof SharedArrayBuffer)
			)
				b = new Uint8Array(part);
			else if (ArrayBuffer.isView(part)) b = new Uint8Array(part.buffer, part.byteOffset, part.byteLength);
			else {
				let str = String(part);
				if (options.endings === "native") {
					const runtimeInfo = core.info() as { platform: string };
					const platform = runtimeInfo.platform;
					str = str.replace(/\r\n|\r|\n/g, platform === "win32" ? "\r\n" : "\n");
				}
				b = text.utf8Encode(str);
			}
			chunks.push(b);
			total += b.length;
		}
		const data = new Uint8Array(total);
		let off = 0;
		for (const b of chunks) {
			data.set(b, off);
			off += b.length;
		}
		this[blobData] = data;
		this.size = total;
		const type = String(options.type ?? "");
		this.type = /[^\x20-\x7e]/.test(type) ? "" : type.toLowerCase();
		Object.defineProperties(this, {
			size: { value: this.size, enumerable: true, writable: false, configurable: false },
			type: { value: this.type, enumerable: true, writable: false, configurable: false },
		});
	}
	slice(start = 0, end = this.size, contentType = ""): Blob {
		const n = this.size;
		const a = Number.isNaN(+start) ? 0 : Math.trunc(+start);
		const z = Number.isNaN(+end) ? 0 : Math.trunc(+end);
		return new Blob(
			[
				this[blobData].subarray(
					a < 0 ? Math.max(n + a, 0) : Math.min(a, n),
					z < 0 ? Math.max(n + z, 0) : Math.min(z, n),
				),
			],
			{ type: contentType },
		);
	}
	async arrayBuffer(): Promise<ArrayBuffer> {
		return this[blobData].slice().buffer as ArrayBuffer;
	}
	async bytes(): Promise<Uint8Array> {
		return this[blobData].slice();
	}
	async text(): Promise<string> {
		const data = this[blobData];
		const start = data[0] === 239 && data[1] === 187 && data[2] === 191 ? 3 : 0;
		return text.decode(data, "utf8", start, data.length);
	}
	stream(): ReadableStream<Uint8Array> {
		const data = this[blobData];
		let offset = 0;
		return new ReadableStream({
			type: "bytes",
			pull(controller) {
				if (offset >= data.length) {
					controller.close();
					return;
				}
				const end = Math.min(offset + 65536, data.length);
				controller.enqueue(data.slice(offset, end));
				offset = end;
			},
		});
	}
	image(options?: ConstructorParameters<typeof Image>[1]): Image {
		return new Image(this, options);
	}
	get [Symbol.toStringTag](): string {
		return "Blob";
	}
}
/** Internal serialization boundary. Callers must copy and never mutate this view. */
export function getBlobBytes(blob: Blob): Uint8Array {
	if (!(blob instanceof Blob)) throw invalidArgType("blob", "an instance of Blob", blob);
	return blob[blobData];
}
export class File extends Blob {
	readonly name: string;
	readonly lastModified: number;
	readonly webkitRelativePath = "";
	constructor(
		parts: Iterable<unknown>,
		name: string,
		options: { type?: string; endings?: "transparent" | "native"; lastModified?: number } = {},
	) {
		if (arguments.length < 2)
			throw codedError("ERR_MISSING_ARGS", 'The "fileBits" and "fileName" arguments must be specified', TypeError);
		super(parts, options);
		this.name = String(name).replace(
			/[\ud800-\udbff](?![\udc00-\udfff])|(?<![\ud800-\udbff])[\udc00-\udfff]/g,
			"\ufffd",
		);
		this.lastModified = options.lastModified === undefined ? Date.now() : Math.trunc(Number(options.lastModified)) || 0;
		Object.defineProperties(this, {
			name: { value: this.name, enumerable: true, writable: false, configurable: false },
			lastModified: { value: this.lastModified, enumerable: true, writable: false, configurable: false },
			webkitRelativePath: { value: "", enumerable: true, writable: false, configurable: false },
		});
	}
	get [Symbol.toStringTag](): string {
		return "File";
	}
}
export function resolveObjectURL(id: string): Blob | undefined {
	return resolveBlobURL(String(id));
}
export default {
	Buffer,
	SlowBuffer,
	Blob,
	File,
	kMaxLength,
	kStringMaxLength,
	constants,
	get INSPECT_MAX_BYTES() {
		return INSPECT_MAX_BYTES;
	},
	set INSPECT_MAX_BYTES(value: number) {
		INSPECT_MAX_BYTES = value;
	},
	atob,
	btoa,
	isUtf8,
	isAscii,
	transcode,
	resolveObjectURL,
};
