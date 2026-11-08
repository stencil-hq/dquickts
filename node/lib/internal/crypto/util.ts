// Shared helpers of the node:crypto implementation (internal:crypto/*): argument conversion and
// validation with Node's error codes and output encoding.
import { inspect } from "node:util";
import { codedError, describeReceived, type CodedError } from "internal:errors";

export type BinaryLike = string | ArrayBuffer | SharedArrayBuffer | ArrayBufferView;
export type Callback<T> = (err: Error | null, result?: T) => void;

// ---------------------------------------------------------------- errors

export function errType(name: string, expected: string, actual: unknown): CodedError {
	const what = name.includes(".") ? "property" : "argument";
	return codedError(
		"ERR_INVALID_ARG_TYPE",
		`The "${name}" ${what} must be ${expected}. ${describeReceived(actual)}`,
		TypeError,
	);
}

function show(value: unknown): string {
	if (typeof value === "string") return `'${value}'`;
	if (typeof value === "bigint") return `${value}n`;
	if (typeof value === "function") return `[Function: ${value.name || "anonymous"}]`;
	if (value !== null && typeof value === "object") {
		try {
			return inspect(value);
		} catch {
			return "[object]";
		}
	}
	return String(value);
}

export function errValue(name: string, value: unknown, reason = "is invalid"): CodedError {
	const what = name.includes(".") ? "property" : "argument";
	return codedError("ERR_INVALID_ARG_VALUE", `The ${what} '${name}' ${reason}. Received ${show(value)}`, TypeError);
}

export function errRange(name: string, range: string, value: unknown): CodedError {
	let shown: string;
	if (typeof value === "number" && Number.isInteger(value) && Math.abs(value) > 2 ** 32) {
		shown = String(value).replace(/(\d)(?=(\d{3})+$)/g, "$1_");
	} else if (typeof value === "bigint") {
		shown = `${value}n`;
	} else {
		shown = typeof value === "string" ? `'${value}'` : String(value);
	}
	return codedError(
		"ERR_OUT_OF_RANGE",
		`The value of "${name}" is out of range. It must be ${range}. Received ${shown}`,
		RangeError,
	);
}

export function errState(op?: string): CodedError {
	return codedError("ERR_CRYPTO_INVALID_STATE", op ? `Invalid state for operation ${op}` : "Invalid state", Error);
}

export function errInvalidDigest(digest: string): CodedError {
	return codedError("ERR_CRYPTO_INVALID_DIGEST", `Invalid digest: ${digest}`, TypeError);
}

export function errKeyObjectType(actual: string, expected: string): CodedError {
	return codedError(
		"ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE",
		`Invalid key object type ${actual}, expected ${expected}.`,
		TypeError,
	);
}

export function errIncompatible(a: string, b: string): CodedError {
	return codedError("ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS", `The selected key encoding ${a} ${b}.`, Error);
}

export function errMissingOption(name: string): CodedError {
	return codedError("ERR_MISSING_OPTION", `${name} is required`, TypeError);
}

// ------------------------------------------------------------ validators

export function validateString(value: unknown, name: string): asserts value is string {
	if (typeof value !== "string") throw errType(name, "of type string", value);
}

export function validateFunction(value: unknown, name: string): asserts value is (...args: never[]) => unknown {
	if (typeof value !== "function") throw errType(name, "of type function", value);
}

export function validateBoolean(value: unknown, name: string): asserts value is boolean {
	if (typeof value !== "boolean") throw errType(name, "of type boolean", value);
}

export function validateObject(value: unknown, name: string): asserts value is Record<string, unknown> {
	if (value === null || typeof value !== "object" || Array.isArray(value)) throw errType(name, "of type object", value);
}

export function validateNumber(value: unknown, name: string, min?: number, max?: number): asserts value is number {
	if (typeof value !== "number") throw errType(name, "of type number", value);
	if (
		(min !== undefined && value < min) ||
		(max !== undefined && value > max) ||
		((min !== undefined || max !== undefined) && Number.isNaN(value))
	) {
		const range = `${min !== undefined ? `>= ${min}` : ""}${min !== undefined && max !== undefined ? " && " : ""}${max !== undefined ? `<= ${max}` : ""}`;
		throw errRange(name, range, value);
	}
}

export function validateInteger(
	value: unknown,
	name: string,
	min = Number.MIN_SAFE_INTEGER,
	max = Number.MAX_SAFE_INTEGER,
): asserts value is number {
	if (typeof value !== "number") throw errType(name, "of type number", value);
	if (!Number.isInteger(value)) throw errRange(name, "an integer", value);
	if (value < min || value > max) throw errRange(name, `>= ${min} && <= ${max}`, value);
}

export function validateInt32(
	value: unknown,
	name: string,
	min = -2147483648,
	max = 2147483647,
): asserts value is number {
	validateInteger(value, name, min, max);
}

export function validateUint32(value: unknown, name: string, positive = false): asserts value is number {
	validateInteger(value, name, positive ? 1 : 0, 4294967295);
}

export function validateOneOf<T>(value: unknown, name: string, oneOf: readonly T[]): asserts value is T {
	if (!oneOf.includes(value as T)) {
		const allowed = oneOf.map((v) => (typeof v === "string" ? `'${v}'` : String(v))).join(", ");
		throw errValue(name, value, `must be one of: ${allowed}`);
	}
}

// ------------------------------------------------------------ conversions

export function isArrayBufferView(v: unknown): v is ArrayBufferView {
	return ArrayBuffer.isView(v);
}

export function isAnyArrayBuffer(v: unknown): v is ArrayBuffer | SharedArrayBuffer {
	return v instanceof ArrayBuffer || (typeof SharedArrayBuffer !== "undefined" && v instanceof SharedArrayBuffer);
}

export function isStringOrBuffer(v: unknown): boolean {
	return typeof v === "string" || isArrayBufferView(v) || isAnyArrayBuffer(v);
}

const kEncodings: Record<string, string> = {
	utf8: "utf8",
	"utf-8": "utf8",
	ucs2: "utf16le",
	"ucs-2": "utf16le",
	utf16le: "utf16le",
	"utf-16le": "utf16le",
	latin1: "latin1",
	binary: "latin1",
	ascii: "ascii",
	hex: "hex",
	base64: "base64",
	base64url: "base64url",
};

export function normalizeEncoding(enc: unknown): string | undefined {
	if (enc === undefined || enc === null || enc === "") return "utf8";
	if (typeof enc !== "string") return undefined;
	return Object.prototype.hasOwnProperty.call(kEncodings, enc.toLowerCase())
		? kEncodings[enc.toLowerCase()]
		: undefined;
}

/** Node's getArrayBufferOrView(): strings are encoded with `encoding` (default utf8). */
export function toBytes(
	data: unknown,
	name: string,
	encoding?: unknown,
): ArrayBuffer | SharedArrayBuffer | ArrayBufferView {
	if (isAnyArrayBuffer(data) || isArrayBufferView(data)) return data;
	if (typeof data === "string") {
		const enc = normalizeEncoding(encoding);
		if (enc === "buffer" || enc === undefined) return Buffer.from(data, "utf8");
		return Buffer.from(data, enc as BufferEncoding);
	}
	throw errType(name, "of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView", data);
}

/** Like toBytes() but keeps UTF-8 strings as strings (the natives encode them). */
export function toData(data: unknown, name: string, encoding?: unknown): BinaryLike {
	if (typeof data === "string") {
		const enc = normalizeEncoding(encoding);
		if (enc === "utf8" || enc === undefined) return data;
		return Buffer.from(data, enc as BufferEncoding);
	}
	return toBytes(data, name, encoding);
}

/** A Buffer view of bytes (no copy). */
export function asBuffer(data: ArrayBuffer | SharedArrayBuffer | ArrayBufferView): Buffer {
	if (Buffer.isBuffer(data)) return data;
	if (isArrayBufferView(data)) return Buffer.from(data.buffer, data.byteOffset, data.byteLength);
	return Buffer.from(data);
}

export function copyBytes(data: ArrayBuffer | SharedArrayBuffer | ArrayBufferView): Buffer {
	return Buffer.from(asBuffer(data));
}

/** Format output: undefined / "buffer" → Buffer, otherwise a string in that encoding. */
export function encodeOutput(buf: Buffer, encoding?: unknown): Buffer | string {
	if (encoding === undefined || encoding === null || encoding === "buffer") return buf;
	const enc = normalizeEncoding(encoding);
	if (enc === undefined) return buf;
	return buf.toString(enc as BufferEncoding);
}

export function toArrayBuffer(buf: Uint8Array): ArrayBuffer {
	if (buf.byteOffset === 0 && buf.byteLength === buf.buffer.byteLength && buf.buffer instanceof ArrayBuffer)
		return buf.buffer;
	return buf.slice().buffer as ArrayBuffer;
}

export function base64url(buf: Uint8Array): string {
	return asBuffer(buf).toString("base64url");
}

export function getStringOption(options: unknown, key: string): string | undefined {
	if (options !== null && typeof options === "object") {
		const v = (options as Record<string, unknown>)[key];
		if (v !== undefined && v !== null) return String(v);
	}
	return undefined;
}

export function getUIntOption(options: unknown, key: string): number {
	if (options !== null && typeof options === "object") {
		const value = (options as Record<string, unknown>)[key];
		if (value !== undefined) {
			if (value === (value as number) >>> 0) return value as number;
			throw errValue(`options.${key}`, value);
		}
	}
	return -1;
}

/** Node's filterDuplicateStrings(): case-insensitive dedupe, sorted. */
export function filterDuplicateStrings(items: string[], low = false): string[] {
	const map = new Map<string, string>();
	for (const item of items) {
		const key = item.toLowerCase();
		map.set(key, low ? key : item);
	}
	return [...map.values()].sort();
}

export function cachedResult<T>(fn: () => T): () => T {
	let result: T | undefined;
	return () => {
		if (result === undefined) result = fn();
		return Array.isArray(result) ? (result.slice() as unknown as T) : result;
	};
}

/** Run `fn` on the next tick, preserving async context like Node's job callbacks. */
export function nextTick(fn: () => void): void {
	process.nextTick(fn);
}
