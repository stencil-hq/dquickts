// Helpers shared by the Node stream and WHATWG stream implementations:
// Node-style error constructors (`new ERR_X(...)`, `new ERR_X.TypeError(...)`),
// the validators of lib/internal/validators.js and the type checks of
// lib/internal/util/types.js, without primordials.

import * as core from "qbun:core";
import { codedError } from "internal:errors";
import { inspect as utilInspect } from "node:util";

type ErrorBase = new (message?: string) => Error;
export type NodeError = Error & { code: string; [key: string]: unknown };
/** A Node error constructor: `new ERR_X(...args)` returns a `Base` instance carrying `code`. */
export type NodeErrorCtor = (new (
	...args: unknown[]
) => NodeError) & {
	TypeError: new (...args: unknown[]) => NodeError;
	RangeError: new (...args: unknown[]) => NodeError;
	Error: new (...args: unknown[]) => NodeError;
};

function inspect(value: unknown, opts?: object): string {
	try {
		return utilInspect(value, opts);
	} catch {
		try {
			return typeof value === "string" ? `'${value}'` : String(value);
		} catch {
			return "[object]";
		}
	}
}

function format(template: string, args: unknown[]): string {
	let i = 0;
	return template.replace(/%[sdj%]/g, (m) => {
		if (m === "%%") return "%";
		if (i >= args.length) return m;
		const a = args[i++];
		if (m === "%d") return String(Number(a));
		if (m === "%j") {
			try {
				return JSON.stringify(a);
			} catch {
				return "[Circular]";
			}
		}
		return typeof a === "string" ? a : inspect(a);
	});
}

function E(
	code: string,
	msg: string | ((...args: never[]) => string),
	Base: ErrorBase,
	...others: ErrorBase[]
): NodeErrorCtor {
	const make = (B: ErrorBase) =>
		function NodeError(...args: unknown[]): NodeError {
			const message = typeof msg === "function" ? (msg as (...a: unknown[]) => string)(...args) : format(msg, args);
			return codedError(code, message, B) as NodeError;
		} as unknown as new (
			...args: unknown[]
		) => NodeError;
	const ctor = make(Base) as unknown as NodeErrorCtor;
	const holder = ctor as unknown as Record<string, unknown>;
	for (const B of [Base, ...others]) holder[B.name] = make(B);
	return ctor;
}

export function determineSpecificType(value: unknown): string {
	if (value === null) return "null";
	if (value === undefined) return "undefined";
	switch (typeof value) {
		case "bigint":
			return `type bigint (${value}n)`;
		case "number":
			if (value === 0) return 1 / value === -Infinity ? "type number (-0)" : "type number (0)";
			if (value !== value) return "type number (NaN)";
			return `type number (${value})`;
		case "boolean":
			return value ? "type boolean (true)" : "type boolean (false)";
		case "symbol":
			return `type symbol (${String(value)})`;
		case "function":
			return `function ${(value as { name: string }).name}`;
		case "object": {
			const name = (value as { constructor?: { name?: unknown } }).constructor?.name;
			if (typeof name === "string" && name !== "") return `an instance of ${name}`;
			return inspect(value, { depth: -1 });
		}
		case "string": {
			let s = value;
			if (s.length > 28) s = `${s.slice(0, 25)}...`;
			return s.indexOf("'") === -1 ? `type string ('${s}')` : `type string (${JSON.stringify(s)})`;
		}
		default: {
			let s = inspect(value, { colors: false });
			if (s.length > 28) s = `${s.slice(0, 25)}...`;
			return `type ${typeof value} (${s})`;
		}
	}
}

export function formatList(array: string[], type = "and"): string {
	switch (array.length) {
		case 0:
			return "";
		case 1:
			return `${array[0]}`;
		case 2:
			return `${array[0]} ${type} ${array[1]}`;
		case 3:
			return `${array[0]}, ${array[1]}, ${type} ${array[2]}`;
		default:
			return `${array.slice(0, -1).join(", ")}, ${type} ${array[array.length - 1]}`;
	}
}

const kTypes = ["string", "function", "number", "object", "Function", "Object", "boolean", "bigint", "symbol"];
const classRegExp = /^[A-Z][a-zA-Z0-9]*$/;

function invalidArgTypeMessage(name: string, expected: string | string[], actual: unknown): string {
	const list = Array.isArray(expected) ? expected : [expected];
	let msg = "The ";
	if (name.endsWith(" argument")) msg += `${name} `;
	else msg += `"${name}" ${name.includes(".") ? "property" : "argument"} `;
	msg += "must be ";
	const types: string[] = [];
	const instances: string[] = [];
	const other: string[] = [];
	for (const value of list) {
		if (kTypes.includes(value)) types.push(value.toLowerCase());
		else if (classRegExp.test(value)) instances.push(value);
		else other.push(value);
	}
	if (instances.length > 0) {
		const pos = types.indexOf("object");
		if (pos !== -1) {
			types.splice(pos, 1);
			instances.push("Object");
		}
	}
	if (types.length > 0) {
		msg += `${types.length > 1 ? "one of type" : "of type"} ${formatList(types, "or")}`;
		if (instances.length > 0 || other.length > 0) msg += " or ";
	}
	if (instances.length > 0) {
		msg += `an instance of ${formatList(instances, "or")}`;
		if (other.length > 0) msg += " or ";
	}
	if (other.length > 0) {
		if (other.length > 1) msg += `one of ${formatList(other, "or")}`;
		else {
			if (other[0].toLowerCase() !== other[0]) msg += "an ";
			msg += `${other[0]}`;
		}
	}
	return `${msg}. Received ${determineSpecificType(actual)}`;
}

function addNumericalSeparator(val: string): string {
	let res = "";
	let i = val.length;
	const start = val[0] === "-" ? 1 : 0;
	for (; i >= start + 4; i -= 3) res = `_${val.slice(i - 3, i)}${res}`;
	return `${val.slice(0, i)}${res}`;
}

export const ERR_INVALID_ARG_TYPE = E("ERR_INVALID_ARG_TYPE", invalidArgTypeMessage, TypeError);
export const ERR_INVALID_ARG_VALUE = E(
	"ERR_INVALID_ARG_VALUE",
	(name: string, value: unknown, reason = "is invalid") => {
		let inspected = inspect(value);
		if (inspected.length > 128) inspected = `${inspected.slice(0, 128)}...`;
		const type = name.includes(".") ? "property" : "argument";
		return `The ${type} '${name}' ${reason}. Received ${inspected}`;
	},
	TypeError,
	RangeError,
);
export const ERR_OUT_OF_RANGE = E(
	"ERR_OUT_OF_RANGE",
	(str: string, range: string, input: unknown, replaceDefaultBoolean = false) => {
		let msg = replaceDefaultBoolean ? str : `The value of "${str}" is out of range.`;
		let received: string;
		if (Number.isInteger(input) && Math.abs(input as number) > 2 ** 32) {
			received = addNumericalSeparator(String(input));
		} else if (typeof input === "bigint") {
			received = String(input);
			if (input > 2n ** 32n || input < -(2n ** 32n)) received = addNumericalSeparator(received);
			received += "n";
		} else {
			received = inspect(input);
		}
		msg += ` It must be ${range}. Received ${received}`;
		return msg;
	},
	RangeError,
);
export const ERR_ARG_NOT_ITERABLE = E("ERR_ARG_NOT_ITERABLE", "%s must be iterable", TypeError);
export const ERR_ILLEGAL_CONSTRUCTOR = E("ERR_ILLEGAL_CONSTRUCTOR", "Illegal constructor", TypeError);
export const ERR_INVALID_RETURN_VALUE = E(
	"ERR_INVALID_RETURN_VALUE",
	(input: string, name: string, value: unknown) =>
		`Expected ${input} to be returned from the "${name}" function but got ${determineSpecificType(value)}.`,
	TypeError,
	RangeError,
);
export const ERR_INVALID_STATE = E("ERR_INVALID_STATE", "Invalid state: %s", Error, TypeError, RangeError);
export const ERR_INVALID_THIS = E("ERR_INVALID_THIS", 'Value of "this" must be of type %s', TypeError);
export const ERR_METHOD_NOT_IMPLEMENTED = E("ERR_METHOD_NOT_IMPLEMENTED", "The %s method is not implemented", Error);
export const ERR_MISSING_ARGS = E(
	"ERR_MISSING_ARGS",
	(...args: (string | string[])[]) => {
		const wrap = (a: string) => `"${a}"`;
		const names = args.map((a) => (Array.isArray(a) ? a.map(wrap).join(" or ") : wrap(a)));
		return `The ${formatList(names)} argument${args.length > 1 ? "s" : ""} must be specified`;
	},
	TypeError,
);
export const ERR_MISSING_OPTION = E("ERR_MISSING_OPTION", "%s is required", TypeError);
export const ERR_MULTIPLE_CALLBACK = E("ERR_MULTIPLE_CALLBACK", "Callback called multiple times", Error);
export const ERR_OPERATION_FAILED = E("ERR_OPERATION_FAILED", "Operation failed: %s", Error, TypeError);
export const ERR_STREAM_ALREADY_FINISHED = E(
	"ERR_STREAM_ALREADY_FINISHED",
	"Cannot call %s after a stream was finished",
	Error,
);
export const ERR_STREAM_CANNOT_PIPE = E("ERR_STREAM_CANNOT_PIPE", "Cannot pipe, not readable", Error);
export const ERR_STREAM_DESTROYED = E("ERR_STREAM_DESTROYED", "Cannot call %s after a stream was destroyed", Error);
export const ERR_STREAM_NULL_VALUES = E("ERR_STREAM_NULL_VALUES", "May not write null values to stream", TypeError);
export const ERR_STREAM_PREMATURE_CLOSE = E("ERR_STREAM_PREMATURE_CLOSE", "Premature close", Error);
export const ERR_STREAM_PUSH_AFTER_EOF = E("ERR_STREAM_PUSH_AFTER_EOF", "stream.push() after EOF", Error);
export const ERR_STREAM_UNABLE_TO_PIPE = E(
	"ERR_STREAM_UNABLE_TO_PIPE",
	"Cannot pipe to a closed or destroyed stream",
	Error,
);
export const ERR_STREAM_UNSHIFT_AFTER_END_EVENT = E(
	"ERR_STREAM_UNSHIFT_AFTER_END_EVENT",
	"stream.unshift() after end event",
	Error,
);
export const ERR_STREAM_WRAP = E("ERR_STREAM_WRAP", "Stream has StringDecoder set or is in objectMode", Error);
export const ERR_STREAM_WRITE_AFTER_END = E("ERR_STREAM_WRITE_AFTER_END", "write after end", Error, TypeError);
export const ERR_UNKNOWN_ENCODING = E("ERR_UNKNOWN_ENCODING", "Unknown encoding: %s", TypeError);

/** Node's AbortError (a plain Error with code ABORT_ERR, not a DOMException). */
export class AbortError extends Error {
	code: string;
	constructor(message = "The operation was aborted", options: { cause?: unknown } | undefined = undefined) {
		if (options !== undefined && typeof options !== "object")
			throw new ERR_INVALID_ARG_TYPE("options", "Object", options);
		super(message, options);
		this.code = "ABORT_ERR";
		this.name = "AbortError";
	}
}

/** The DOMException("…", "AbortError") web APIs reject with. */
export function domAbortError(message = "This operation was aborted"): DOMException {
	return new DOMException(message, "AbortError");
}

/** Combine two errors the way Node's internal aggregateTwoErrors does. */
export function aggregateTwoErrors(innerError: unknown, outerError: unknown): unknown {
	if (innerError && outerError && innerError !== outerError) {
		const outer = outerError as { errors?: unknown[]; message?: string; code?: unknown };
		if (Array.isArray(outer.errors)) {
			outer.errors.push(innerError);
			return outerError;
		}
		const err = new AggregateError([outerError, innerError], outer.message) as AggregateError & { code?: unknown };
		err.code = outer.code;
		return err;
	}
	return innerError || outerError;
}

// ---------------------------------------------------------------- validators

export const kValidateObjectNone = 0;
export const kValidateObjectAllowNullable = 1 << 0;
export const kValidateObjectAllowArray = 1 << 1;
export const kValidateObjectAllowFunction = 1 << 2;
export const kValidateObjectAllowObjects = kValidateObjectAllowArray | kValidateObjectAllowFunction;
export const kValidateObjectAllowObjectsAndNull =
	kValidateObjectAllowNullable | kValidateObjectAllowArray | kValidateObjectAllowFunction;

export function validateObject(value: unknown, name: string, options = kValidateObjectNone): void {
	if (options === kValidateObjectNone) {
		if (value === null || Array.isArray(value) || typeof value !== "object")
			throw new ERR_INVALID_ARG_TYPE(name, "Object", value);
		return;
	}
	if ((kValidateObjectAllowNullable & options) === 0 && value === null)
		throw new ERR_INVALID_ARG_TYPE(name, "Object", value);
	if ((kValidateObjectAllowArray & options) === 0 && Array.isArray(value))
		throw new ERR_INVALID_ARG_TYPE(name, "Object", value);
	const t = typeof value;
	if (t !== "object" && ((kValidateObjectAllowFunction & options) === 0 || t !== "function")) {
		throw new ERR_INVALID_ARG_TYPE(name, "Object", value);
	}
}

/** WebIDL dictionary check: undefined, null, objects and functions. */
export function validateDictionary(value: unknown, name: string): void {
	if (value != null && typeof value !== "object" && typeof value !== "function") {
		throw new ERR_INVALID_ARG_TYPE(name, "a dictionary", value);
	}
}

export function validateFunction(value: unknown, name: string): asserts value is (...args: unknown[]) => unknown {
	if (typeof value !== "function") throw new ERR_INVALID_ARG_TYPE(name, "Function", value);
}

export function validateBoolean(value: unknown, name: string): asserts value is boolean {
	if (typeof value !== "boolean") throw new ERR_INVALID_ARG_TYPE(name, "boolean", value);
}

export function validateString(value: unknown, name: string): asserts value is string {
	if (typeof value !== "string") throw new ERR_INVALID_ARG_TYPE(name, "string", value);
}

export function validateNumber(
	value: unknown,
	name: string,
	min?: number | null,
	max?: number | null,
): asserts value is number {
	if (typeof value !== "number") throw new ERR_INVALID_ARG_TYPE(name, "number", value);
	if (
		(min != null && value < min) ||
		(max != null && value > max) ||
		((min != null || max != null) && Number.isNaN(value))
	) {
		throw new ERR_OUT_OF_RANGE(
			name,
			`${min != null ? `>= ${min}` : ""}${min != null && max != null ? " && " : ""}${max != null ? `<= ${max}` : ""}`,
			value,
		);
	}
}

export function validateInteger(
	value: unknown,
	name: string,
	min = Number.MIN_SAFE_INTEGER,
	max = Number.MAX_SAFE_INTEGER,
): asserts value is number {
	if (typeof value !== "number") throw new ERR_INVALID_ARG_TYPE(name, "number", value);
	if (!Number.isInteger(value)) throw new ERR_OUT_OF_RANGE(name, "an integer", value);
	if (value < min || value > max) throw new ERR_OUT_OF_RANGE(name, `>= ${min} && <= ${max}`, value);
}

export function validateUint32(value: unknown, name: string, positive = false): asserts value is number {
	if (typeof value !== "number") throw new ERR_INVALID_ARG_TYPE(name, "number", value);
	if (!Number.isInteger(value)) throw new ERR_OUT_OF_RANGE(name, "an integer", value);
	const min = positive ? 1 : 0;
	const max = 4_294_967_295;
	if (value < min || value > max) throw new ERR_OUT_OF_RANGE(name, `>= ${min} && <= ${max}`, value);
}

export function validateOneOf(value: unknown, name: string, oneOf: readonly unknown[]): void {
	if (!oneOf.includes(value)) {
		const allowed = oneOf.map((v) => (typeof v === "string" ? `'${v}'` : String(v))).join(", ");
		throw new ERR_INVALID_ARG_VALUE(name, value, `must be one of: ${allowed}`);
	}
}

export function validateAbortSignal(signal: unknown, name: string): asserts signal is AbortSignal | undefined {
	if (signal !== undefined && (signal === null || typeof signal !== "object" || !("aborted" in (signal as object)))) {
		throw new ERR_INVALID_ARG_TYPE(name, "AbortSignal", signal);
	}
}

export function validateBuffer(buffer: unknown, name = "buffer"): asserts buffer is ArrayBufferView {
	if (!ArrayBuffer.isView(buffer)) throw new ERR_INVALID_ARG_TYPE(name, ["Buffer", "TypedArray", "DataView"], buffer);
}

// ---------------------------------------------------------------- type checks

const TypedArrayPrototype = Object.getPrototypeOf(Uint8Array.prototype) as object;
const typedArrayTag = Object.getOwnPropertyDescriptor(TypedArrayPrototype, Symbol.toStringTag)!.get as (
	this: unknown,
) => string | undefined;
const arrayBufferByteLength = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "byteLength")!.get as (
	this: unknown,
) => number;
const sabByteLength =
	typeof SharedArrayBuffer === "function"
		? (Object.getOwnPropertyDescriptor(SharedArrayBuffer.prototype, "byteLength")!.get as (this: unknown) => number)
		: undefined;
const dataViewByteLength = Object.getOwnPropertyDescriptor(DataView.prototype, "byteLength")!.get as (
	this: unknown,
) => number;

export const isArrayBufferView = ArrayBuffer.isView as (value: unknown) => value is ArrayBufferView;

export function typedArrayKind(value: unknown): string | undefined {
	return typedArrayTag.call(value);
}

export function isTypedArray(value: unknown): value is Uint8Array {
	return typedArrayTag.call(value) !== undefined;
}

export function isUint8Array(value: unknown): value is Uint8Array {
	return typedArrayTag.call(value) === "Uint8Array";
}

export function isDataView(value: unknown): value is DataView {
	if (!ArrayBuffer.isView(value) || isTypedArray(value)) return false;
	try {
		dataViewByteLength.call(value);
		return true;
	} catch {
		return false;
	}
}

export function isArrayBuffer(value: unknown): value is ArrayBuffer {
	if (value === null || typeof value !== "object") return false;
	try {
		arrayBufferByteLength.call(value);
		return true;
	} catch {
		return false;
	}
}

export function isSharedArrayBuffer(value: unknown): value is SharedArrayBuffer {
	if (sabByteLength === undefined || value === null || typeof value !== "object") return false;
	try {
		sabByteLength.call(value);
		return true;
	} catch {
		return false;
	}
}

export function isAnyArrayBuffer(value: unknown): value is ArrayBuffer | SharedArrayBuffer {
	return isArrayBuffer(value) || isSharedArrayBuffer(value);
}

export function isPromise(value: unknown): value is Promise<unknown> {
	if (value === null || typeof value !== "object") return false;
	// promiseState() returns undefined for anything but a native promise
	return core.promiseState(value as Promise<unknown>) !== undefined;
}

// ---------------------------------------------------------------- misc util

export const kEmptyObject: Readonly<Record<string, never>> = Object.freeze(Object.create(null));
export const customInspectSymbol: unique symbol = Symbol.for("nodejs.util.inspect.custom") as never;
export const kEnumerableProperty: PropertyDescriptor = Object.freeze({
	__proto__: null,
	enumerable: true,
} as PropertyDescriptor);

/** Call `fn` at most once (Node's internal/util once). */
export function once<A extends unknown[], R>(
	fn: (this: unknown, ...args: A) => R,
): (this: unknown, ...args: A) => R | undefined {
	let called = false;
	return function (this: unknown, ...args: A): R | undefined {
		if (called) return undefined;
		called = true;
		return fn.apply(this, args);
	};
}

/** process.nextTick through the global process object. */
export function nextTick(fn: (...args: unknown[]) => void, ...args: unknown[]): void {
	process.nextTick(fn, ...args);
}
