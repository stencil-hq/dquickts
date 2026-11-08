// WHATWG FormData, multipart/form-data serialization (to a Blob) and the
// parsers used by Body.formData() (multipart and urlencoded).

import { Blob, File } from "node:buffer";
import { TextDecoder, TextEncoder } from "internal:encoding";

export type FormDataEntryValue = string | File;

/** A Blob or an object reading like one (Bun.file()). */
export interface BlobLike {
	readonly size: number;
	readonly type: string;
	readonly name?: string;
	arrayBuffer(): Promise<ArrayBuffer>;
	stream(): ReadableStream<Uint8Array>;
}

export function isBlobLike(value: unknown): value is BlobLike {
	if (value instanceof Blob) return true;
	if (value === null || typeof value !== "object") return false;
	const v = value as Record<string, unknown>;
	return (
		typeof v.arrayBuffer === "function" &&
		typeof v.stream === "function" &&
		typeof v.size === "number" &&
		typeof v.type === "string"
	);
}

/** A node:buffer Blob or File holding its bytes (not a lazy subclass such as Bun.file()). */
function isMemoryBlob(value: unknown): value is Blob {
	if (!(value instanceof Blob)) return false;
	const proto = Object.getPrototypeOf(value);
	return proto === Blob.prototype || proto === File.prototype;
}

const kEntries = Symbol("form data entries");
/** File names given to append()/set() for Blob-like values that are not Blobs (Bun.file()). */
const fileNames = new WeakMap<object, string>();

function baseName(path: string): string {
	const i = path.lastIndexOf("/");
	return i >= 0 ? path.slice(i + 1) : path;
}

function toEntryValue(value: unknown, filename?: string): FormDataEntryValue {
	if (!isMemoryBlob(value) && isBlobLike(value)) {
		// Bun.file(): kept lazy, read when the form is serialized
		if (filename !== undefined) fileNames.set(value, String(filename));
		return value as unknown as File;
	}
	if (value instanceof Blob) {
		const blob = value as Blob & { name?: string; lastModified?: number };
		if (blob instanceof File && filename === undefined) return blob;
		const name = filename !== undefined ? String(filename) : blob instanceof File ? blob.name : "blob";
		const options: { type: string; lastModified?: number } = { type: blob.type };
		if (blob instanceof File) options.lastModified = blob.lastModified;
		return new File([blob], name, options);
	}
	return String(value);
}

export class FormData {
	[kEntries]: [string, FormDataEntryValue][] = [];

	constructor(form?: unknown) {
		Object.defineProperty(this, kEntries, { enumerable: false });
		if (form !== undefined) throw new TypeError("FormData constructor: HTMLFormElement is not supported");
	}

	append(name: string, value: string | Blob, filename?: string): void {
		if (arguments.length < 2) throw new TypeError("FormData.append requires at least 2 arguments");
		if (filename !== undefined && !isBlobLike(value)) {
			throw new TypeError("FormData.append: a filename requires a Blob value");
		}
		this[kEntries].push([String(name), toEntryValue(value, filename)]);
	}

	delete(name: string): void {
		name = String(name);
		this[kEntries] = this[kEntries].filter((entry) => entry[0] !== name);
	}

	get(name: string): FormDataEntryValue | null {
		name = String(name);
		const entry = this[kEntries].find((e) => e[0] === name);
		return entry ? entry[1] : null;
	}

	getAll(name: string): FormDataEntryValue[] {
		name = String(name);
		return this[kEntries].filter((e) => e[0] === name).map((e) => e[1]);
	}

	has(name: string): boolean {
		name = String(name);
		return this[kEntries].some((e) => e[0] === name);
	}

	set(name: string, value: string | Blob, filename?: string): void {
		if (arguments.length < 2) throw new TypeError("FormData.set requires at least 2 arguments");
		if (filename !== undefined && !isBlobLike(value)) {
			throw new TypeError("FormData.set: a filename requires a Blob value");
		}
		name = String(name);
		const entry: [string, FormDataEntryValue] = [name, toEntryValue(value, filename)];
		const list = this[kEntries];
		const index = list.findIndex((e) => e[0] === name);
		if (index < 0) {
			list.push(entry);
			return;
		}
		list[index] = entry;
		this[kEntries] = list.filter((e, i) => i <= index || e[0] !== name);
	}

	forEach(callback: (value: FormDataEntryValue, name: string, parent: FormData) => void, thisArg?: unknown): void {
		if (typeof callback !== "function") throw new TypeError("FormData.forEach: callback must be a function");
		for (const [name, value] of this[kEntries].slice()) callback.call(thisArg, value, name, this);
	}

	*entries(): IterableIterator<[string, FormDataEntryValue]> {
		const list = this[kEntries];
		for (let i = 0; i < list.length; i++) yield [list[i][0], list[i][1]];
	}

	*keys(): IterableIterator<string> {
		for (const [name] of this.entries()) yield name;
	}

	*values(): IterableIterator<FormDataEntryValue> {
		for (const [, value] of this.entries()) yield value;
	}

	[Symbol.iterator](): IterableIterator<[string, FormDataEntryValue]> {
		return this.entries();
	}

	/** Bun: a plain object; repeated names become arrays. */
	toJSON(): Record<string, FormDataEntryValue | FormDataEntryValue[]> {
		const out: Record<string, FormDataEntryValue | FormDataEntryValue[]> = {};
		for (const [name, value] of this[kEntries]) {
			const existing = out[name];
			if (existing === undefined) out[name] = value;
			else if (Array.isArray(existing)) existing.push(value);
			else out[name] = [existing, value];
		}
		return out;
	}

	get [Symbol.toStringTag](): string {
		return "FormData";
	}

	[Symbol.for("nodejs.util.inspect.custom")](
		depth: number,
		options: object,
		inspect: (v: unknown, o?: object) => string,
	): string {
		return `FormData ${inspect(
			this[kEntries].map(([name, value]) => ({ name, value })),
			options,
		)}`;
	}
}

function escapeName(name: string): string {
	return name.replace(/\n/g, "%0A").replace(/\r/g, "%0D").replace(/"/g, "%22");
}

function randomBoundary(): string {
	let hex = "";
	for (let i = 0; i < 24; i++) hex += Math.floor(Math.random() * 16).toString(16);
	return `----formdata-qbun-${hex}`;
}

/**
 * Serialize as multipart/form-data: a Blob when every file is a Blob, a
 * ReadableStream when some are lazy Blob-likes (Bun.file()).
 */
export function serializeFormData(form: FormData): {
	blob: Blob | null;
	stream: ReadableStream<Uint8Array> | null;
	length: number | null;
	type: string;
} {
	const boundary = randomBoundary();
	const encoder = new TextEncoder();
	const parts: (Uint8Array | Blob | BlobLike)[] = [];
	let lazy = false;
	for (const [name, value] of form[kEntries]) {
		const disposition = `--${boundary}\r\nContent-Disposition: form-data; name="${escapeName(name)}"`;
		if (typeof value === "string") {
			parts.push(encoder.encode(`${disposition}\r\n\r\n${value.replace(/\r(?!\n)|(?<!\r)\n/g, "\r\n")}\r\n`));
		} else {
			const blob = value as BlobLike;
			const type = blob.type || "application/octet-stream";
			const filename =
				fileNames.get(blob) ??
				(isMemoryBlob(value) && value instanceof File ? value.name : baseName(blob.name ?? "blob"));
			parts.push(encoder.encode(`${disposition}; filename="${escapeName(filename)}"\r\nContent-Type: ${type}\r\n\r\n`));
			parts.push(blob);
			parts.push(encoder.encode("\r\n"));
			if (!isMemoryBlob(value)) lazy = true;
		}
	}
	parts.push(encoder.encode(`--${boundary}--\r\n`));
	const type = `multipart/form-data; boundary=${boundary}`;
	if (!lazy) return { blob: new Blob(parts), stream: null, length: null, type };
	let length = 0;
	for (const part of parts) length += part instanceof Uint8Array ? part.length : part.size;
	let index = 0;
	const stream = new ReadableStream<Uint8Array>({
		async pull(controller) {
			if (index >= parts.length) {
				controller.close();
				return;
			}
			const part = parts[index++];
			controller.enqueue(part instanceof Uint8Array ? part : new Uint8Array(await part.arrayBuffer()));
		},
	});
	return { blob: null, stream, length, type };
}

/** The value of parameter `name` of a header value like `a/b; name="v"`. */
export function headerParam(value: string, name: string): string | null {
	const re = new RegExp(`;\\s*${name}\\s*=\\s*(?:"((?:[^"\\\\]|\\\\.)*)"|([^;\\s]*))`, "i");
	const m = re.exec(value);
	if (!m) return null;
	return m[1] !== undefined ? m[1].replace(/\\(.)/g, "$1") : m[2];
}

function indexOf(haystack: Uint8Array, needle: Uint8Array, from: number): number {
	const n = needle.length;
	const first = needle[0];
	const last = haystack.length - n;
	outer: for (let i = from; i <= last; i++) {
		if (haystack[i] !== first) continue;
		for (let j = 1; j < n; j++) if (haystack[i + j] !== needle[j]) continue outer;
		return i;
	}
	return -1;
}

/** Parse a multipart/form-data body. */
export function parseMultipart(bytes: Uint8Array, contentType: string): FormData {
	const boundary = headerParam(contentType, "boundary");
	if (!boundary) throw new TypeError("Missing or invalid multipart boundary");
	const encoder = new TextEncoder();
	const decoder = new TextDecoder();
	const delimiter = encoder.encode(`--${boundary}`);
	const form = new FormData();
	let pos = indexOf(bytes, delimiter, 0);
	if (pos < 0) throw new TypeError("Failed to parse multipart body: no boundary found");
	for (;;) {
		pos += delimiter.length;
		// "--" after the delimiter ends the body
		if (bytes[pos] === 0x2d && bytes[pos + 1] === 0x2d) break;
		// skip transport padding and CRLF
		while (bytes[pos] === 0x20 || bytes[pos] === 0x09) pos++;
		if (bytes[pos] === 0x0d && bytes[pos + 1] === 0x0a) pos += 2;
		else throw new TypeError("Failed to parse multipart body: malformed delimiter");
		const headerEnd = indexOf(bytes, encoder.encode("\r\n\r\n"), pos);
		if (headerEnd < 0) throw new TypeError("Failed to parse multipart body: unterminated part headers");
		const headerText = latin1String(bytes.subarray(pos, headerEnd));
		const bodyStart = headerEnd + 4;
		const next = indexOf(bytes, encoder.encode(`\r\n--${boundary}`), bodyStart);
		if (next < 0) throw new TypeError("Failed to parse multipart body: missing closing boundary");
		const body = bytes.subarray(bodyStart, next);
		let disposition = "";
		let type = "";
		for (const line of headerText.split("\r\n")) {
			const colon = line.indexOf(":");
			if (colon < 0) continue;
			const key = line.slice(0, colon).trim().toLowerCase();
			const value = line.slice(colon + 1).trim();
			if (key === "content-disposition") disposition = value;
			else if (key === "content-type") type = value;
		}
		if (!/^form-data\b/i.test(disposition))
			throw new TypeError("Failed to parse multipart body: invalid Content-Disposition");
		const rawName = headerParam(disposition, "name");
		if (rawName === null) throw new TypeError("Failed to parse multipart body: part without a name");
		const name = decoder.decode(latin1Bytes(rawName));
		const starName = headerParam(disposition, "filename\\*");
		const rawFilename = headerParam(disposition, "filename");
		if (starName !== null || rawFilename !== null) {
			let filename: string;
			if (starName !== null) {
				const m = /^([^']*)'[^']*'(.*)$/.exec(starName);
				filename = m ? decodeURIComponent(m[2]) : starName;
			} else {
				filename = decoder.decode(latin1Bytes(rawFilename!));
			}
			form.append(name, new File([body.slice()], filename, { type: type || "application/octet-stream" }));
		} else {
			form.append(name, decoder.decode(body));
		}
		pos = next + 2;
	}
	return form;
}

/** Bytes as a latin1 string (header text). */
function latin1String(bytes: Uint8Array): string {
	let out = "";
	for (let i = 0; i < bytes.length; i += 8192) out += String.fromCharCode(...bytes.subarray(i, i + 8192));
	return out;
}

/** The bytes of a string decoded as latin1 (header parameters are bytes). */
function latin1Bytes(s: string): Uint8Array {
	const out = new Uint8Array(s.length);
	for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i) & 0xff;
	return out;
}

/** Parse an application/x-www-form-urlencoded body. */
export function parseUrlEncoded(text: string): FormData {
	const form = new FormData();
	for (const [name, value] of new URLSearchParams(text)) form.append(name, value);
	return form;
}

export function formDataEntries(form: FormData): [string, FormDataEntryValue][] {
	return form[kEntries];
}
