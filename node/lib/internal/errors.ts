// Node-style errors: an Error (or subclass) with a `code` property.

type ErrorCtor = new (message?: string) => Error;

export interface CodedError extends Error {
	code: string;
}

/** Create a Node-style error carrying `code`, e.g. ERR_INVALID_ARG_TYPE. */
export function codedError(code: string, message: string, Ctor: ErrorCtor = Error): CodedError {
	const err = new Ctor(message) as CodedError;
	Object.defineProperty(err, "code", { value: code, writable: true, configurable: true, enumerable: true });
	return err;
}

/** Describe a received value the way Node's ERR_INVALID_ARG_TYPE does. */
export function describeReceived(v: unknown): string {
	if (v === null) return "Received null";
	if (v === undefined) return "Received undefined";
	if (typeof v === "function") return `Received function ${v.name || "<anonymous>"}`;
	if (typeof v === "object") {
		const name = Object.getPrototypeOf(v)?.constructor?.name;
		return name ? `Received an instance of ${name}` : "Received [object]";
	}
	let shown = String(v);
	if (shown.length > 28) shown = `${shown.slice(0, 25)}...`;
	if (typeof v === "string") shown = `'${shown}'`;
	return `Received type ${typeof v} (${shown})`;
}

export function invalidArgType(name: string, expected: string, actual: unknown): CodedError {
	const what = name.includes(".") ? "property" : "argument";
	return codedError(
		"ERR_INVALID_ARG_TYPE",
		`The "${name}" ${what} must be ${expected}. ${describeReceived(actual)}`,
		TypeError,
	);
}

export function invalidArgValue(name: string, value: unknown, reason = "is invalid"): CodedError {
	let shown: string;
	try {
		shown = typeof value === "string" ? `'${value}'` : String(value);
	} catch {
		shown = "[object]";
	}
	return codedError("ERR_INVALID_ARG_VALUE", `The argument '${name}' ${reason}. Received ${shown}`, TypeError);
}

export function outOfRange(name: string, range: string, value: unknown): CodedError {
	return codedError(
		"ERR_OUT_OF_RANGE",
		`The value of "${name}" is out of range. It must be ${range}. Received ${String(value)}`,
		RangeError,
	);
}

export function notSupported(what: string): CodedError {
	return codedError("ERR_NOT_SUPPORTED", `${what} is not supported by qbun`);
}

export function abortError(reason?: unknown): Error {
	const err = new DOMException("The operation was aborted.", "AbortError");
	if (reason !== undefined) Object.defineProperty(err, "cause", { value: reason, configurable: true, writable: true });
	return err;
}

export function validateFunction(value: unknown, name: string): asserts value is (...args: unknown[]) => unknown {
	if (typeof value !== "function") throw invalidArgType(name, "of type function", value);
}

export function validateString(value: unknown, name: string): asserts value is string {
	if (typeof value !== "string") throw invalidArgType(name, "of type string", value);
}

export function validateNumber(value: unknown, name: string, min?: number, max?: number): asserts value is number {
	if (typeof value !== "number") throw invalidArgType(name, "of type number", value);
	if ((min !== undefined && value < min) || (max !== undefined && value > max) || Number.isNaN(value)) {
		throw outOfRange(
			name,
			`${min !== undefined ? `>= ${min}` : ""}${min !== undefined && max !== undefined ? " && " : ""}${max !== undefined ? `<= ${max}` : ""}`,
			value,
		);
	}
}

export function validateObject(value: unknown, name: string): asserts value is object {
	if (value === null || typeof value !== "object") throw invalidArgType(name, "of type object", value);
}
