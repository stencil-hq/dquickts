import * as os from "qbun:os";
import types from "node:util/types";
import { inspect, format, formatWithOptions, stripVTControlCharacters } from "internal:util/inspect";
import { compare } from "internal:util/comparisons";
import { parseArgs } from "internal:util/parse-args";
import { codedError, invalidArgType, invalidArgValue, validateFunction } from "internal:errors";
import { TextEncoder, TextDecoder } from "internal:encoding";
export { inspect, format, formatWithOptions, stripVTControlCharacters, types, parseArgs, TextEncoder, TextDecoder };
type Callable = (...args: unknown[]) => unknown;
export function isDeepStrictEqual(a: unknown, b: unknown, options?: boolean | { skipPrototype?: boolean }): boolean {
	return compare(
		a,
		b,
		true,
		false,
		new Map(),
		typeof options === "boolean" ? options : (options?.skipPrototype ?? false),
	);
}
const promisifyCustom = Symbol.for("nodejs.util.promisify.custom");
export function promisify(original: Callable): Callable {
	validateFunction(original, "original");
	const custom = (original as Callable & Record<symbol, unknown>)[promisifyCustom];
	if (custom !== undefined) {
		validateFunction(custom, "util.promisify.custom");
		Object.defineProperty(custom, promisifyCustom, { value: custom, configurable: true });
		return custom;
	}
	const wrapped = function (this: unknown, ...args: unknown[]) {
		const p = Promise.withResolvers<unknown>();
		try {
			original.apply(this, [...args, (err: unknown, value: unknown) => (err ? p.reject(err) : p.resolve(value))]);
		} catch (e) {
			p.reject(e);
		}
		return p.promise;
	};
	Object.setPrototypeOf(wrapped, Object.getPrototypeOf(original));
	Object.defineProperties(wrapped, Object.getOwnPropertyDescriptors(original));
	Object.defineProperty(wrapped, promisifyCustom, { value: wrapped, configurable: true });
	return wrapped;
}
promisify.custom = promisifyCustom;
export function callbackify(original: Callable): Callable {
	validateFunction(original, "original");
	const wrapped = function (this: unknown, ...args: unknown[]) {
		const cb = args.pop();
		validateFunction(cb, "last argument");
		const result = original.apply(this, args) as Promise<unknown>;
		result.then(
			(value) => process.nextTick(cb.bind(this, null, value)),
			(reason) => {
				if (!reason)
					reason = Object.assign(codedError("ERR_FALSY_VALUE_REJECTION", "Promise was rejected with falsy value"), {
						reason,
					});
				process.nextTick(cb.bind(this, reason));
			},
		);
	};
	Object.setPrototypeOf(wrapped, Object.getPrototypeOf(original));
	Object.defineProperties(wrapped, Object.getOwnPropertyDescriptors(original));
	Object.defineProperty(wrapped, "length", { value: original.length + 1, configurable: true });
	Object.defineProperty(wrapped, "name", { value: `${original.name}Callbackified`, configurable: true });
	return wrapped;
}
const warned = new Set<string>();
export function deprecate<T extends Callable>(fn: T, msg: string, code?: string): T {
	validateFunction(fn, "fn");
	let emitted = false;
	const wrapped = function (this: unknown, ...args: unknown[]) {
		if (!emitted && !process.noDeprecation) {
			emitted = true;
			if (!code || !warned.has(code)) {
				if (code) warned.add(code);
				process.emitWarning(msg, "DeprecationWarning", code);
			}
		}
		if (new.target) return Reflect.construct(fn, args, new.target);
		return fn.apply(this, args);
	};
	Object.setPrototypeOf(wrapped, fn);
	wrapped.prototype = fn.prototype;
	return wrapped as unknown as T;
}
export function inherits(ctor: Function, superCtor: Function): void {
	validateFunction(ctor, "ctor");
	validateFunction(superCtor, "superCtor");
	if (!superCtor.prototype) throw invalidArgType("superCtor.prototype", "of type object", superCtor.prototype);
	Object.defineProperty(ctor, "super_", { value: superCtor, writable: true, configurable: true });
	Object.setPrototypeOf(ctor.prototype, superCtor.prototype);
}
export const isArray = Array.isArray;
export const isBoolean = (v: unknown): boolean => typeof v === "boolean";
export const isNull = (v: unknown): boolean => v === null;
export const isNullOrUndefined = (v: unknown): boolean => v == null;
export const isNumber = (v: unknown): boolean => typeof v === "number";
export const isString = (v: unknown): boolean => typeof v === "string";
export const isSymbol = (v: unknown): boolean => typeof v === "symbol";
export const isUndefined = (v: unknown): boolean => v === undefined;
export const isRegExp = types.isRegExp;
export const isObject = (v: unknown): boolean => v !== null && typeof v === "object";
export const isDate = types.isDate;
export const isError = types.isNativeError;
export const isFunction = (v: unknown): boolean => typeof v === "function";
export const isPrimitive = (v: unknown): boolean => v === null || (typeof v !== "object" && typeof v !== "function");
export const isBuffer = (v: unknown): boolean =>
	v !== null && typeof v === "object" && v.constructor?.name === "Buffer";
export function _extend<T extends object>(target: T, source: object): T {
	if (source && typeof source === "object")
		for (const key in source)
			if (Object.hasOwn(source, key))
				(target as Record<string, unknown>)[key] = (source as Record<string, unknown>)[key];
	return target;
}
export function toUSVString(value: unknown): string {
	return String(value).replace(/[\uD800-\uDBFF][\uDC00-\uDFFF]|[\uD800-\uDFFF]/g, (s) =>
		s.length === 2 ? s : "\uFFFD",
	);
}
export function styleText(
	style: string | string[],
	text: string,
	options: { validateStream?: boolean; stream?: { isTTY?: boolean } } = {},
): string {
	if (typeof text !== "string") throw invalidArgType("text", "of type string", text);
	const list = Array.isArray(style) ? style : [style];
	for (const name of list) if (!Object.hasOwn(inspect.colors, name)) throw invalidArgValue("format", name);
	if (options.validateStream !== false && !colorEnabled(options.stream ?? process.stdout)) return text;
	let out = text;
	for (let i = list.length - 1; i >= 0; i--) {
		const [a, b] = inspect.colors[list[i]];
		out = `\x1b[${a}m${out.replaceAll(`\x1b[${b}m`, `\x1b[${a}m`)}\x1b[${b}m`;
	}
	return out;
}
export function colorEnabled(stream: { isTTY?: boolean }): boolean {
	if (process.env.FORCE_COLOR !== undefined) return process.env.FORCE_COLOR !== "0";
	return (
		process.env.NO_COLOR === undefined &&
		process.env.NODE_DISABLE_COLORS === undefined &&
		!!stream.isTTY &&
		process.env.TERM !== "dumb"
	);
}
export function debuglog(section: string, callback?: (fn: Callable) => void): Callable & { enabled: boolean } {
	const name = section.toUpperCase();
	const pattern = (process.env.NODE_DEBUG ?? "")
		.replace(/[.+?^${}()|[\]\\]/g, "\\$&")
		.replace(/\*/g, ".*")
		.replace(/,/g, "|");
	const enabled = !!pattern && new RegExp(`^(?:${pattern})$`, "i").test(name);
	const fn = Object.assign(
		(...args: unknown[]) => {
			if (enabled) process.stderr.write(`${name} ${process.pid}: ${format(...args)}\n`);
		},
		{ enabled },
	);
	callback?.(fn);
	return fn;
}
export const debug = debuglog;
const fallback: Record<string, number> = {
	EPERM: 1,
	ENOENT: 2,
	ESRCH: 3,
	EINTR: 4,
	EIO: 5,
	ENXIO: 6,
	E2BIG: 7,
	ENOEXEC: 8,
	EBADF: 9,
	ECHILD: 10,
	EAGAIN: 11,
	ENOMEM: 12,
	EACCES: 13,
	EFAULT: 14,
	EBUSY: 16,
	EEXIST: 17,
	EXDEV: 18,
	ENODEV: 19,
	ENOTDIR: 20,
	EISDIR: 21,
	EINVAL: 22,
	ENFILE: 23,
	EMFILE: 24,
	ENOTTY: 25,
	EFBIG: 27,
	ENOSPC: 28,
	ESPIPE: 29,
	EROFS: 30,
	EMLINK: 31,
	EPIPE: 32,
	EDOM: 33,
	ERANGE: 34,
};
const errnoMessages: Record<string, string> = {
	EPERM: "operation not permitted",
	ENOENT: "no such file or directory",
	ESRCH: "no such process",
	EINTR: "interrupted system call",
	EIO: "i/o error",
	ENXIO: "no such device or address",
	E2BIG: "argument list too long",
	ENOEXEC: "exec format error",
	EBADF: "bad file descriptor",
	ECHILD: "no child processes",
	EAGAIN: "resource temporarily unavailable",
	ENOMEM: "not enough memory",
	EACCES: "permission denied",
	EFAULT: "bad address",
	EBUSY: "resource busy or locked",
	EEXIST: "file already exists",
	EXDEV: "cross-device link not permitted",
	ENODEV: "no such device",
	ENOTDIR: "not a directory",
	EISDIR: "illegal operation on a directory",
	EINVAL: "invalid argument",
	ENFILE: "file table overflow",
	EMFILE: "too many open files",
	ENOTTY: "inappropriate ioctl for device",
	EFBIG: "file too large",
	ENOSPC: "no space left on device",
	ESPIPE: "invalid seek",
	EROFS: "read-only file system",
	EMLINK: "too many links",
	EPIPE: "broken pipe",
	EDOM: "argument out of domain",
	ERANGE: "result too large",
	EADDRINUSE: "address already in use",
	EADDRNOTAVAIL: "address not available",
	EAFNOSUPPORT: "address family not supported",
	ECONNABORTED: "software caused connection abort",
	ECONNREFUSED: "connection refused",
	ECONNRESET: "connection reset by peer",
	EDESTADDRREQ: "destination address required",
	EHOSTUNREACH: "host is unreachable",
	EINPROGRESS: "operation now in progress",
	EISCONN: "socket is already connected",
	ELOOP: "too many symbolic links encountered",
	EMSGSIZE: "message too long",
	ENAMETOOLONG: "name too long",
	ENETDOWN: "network is down",
	ENETUNREACH: "network is unreachable",
	ENOBUFS: "no buffer space available",
	ENOSYS: "function not implemented",
	ENOTCONN: "socket is not connected",
	ENOTEMPTY: "directory not empty",
	ENOTSOCK: "socket operation on non-socket",
	ENOTSUP: "operation not supported on socket",
	EOVERFLOW: "value too large for defined data type",
	ETIMEDOUT: "connection timed out",
	ECANCELED: "operation canceled",
	EILSEQ: "illegal byte sequence",
};
export function getSystemErrorMap(): Map<number, [string, string]> {
	let errno = fallback;
	try {
		errno = os.constants.errno ?? fallback;
	} catch {}
	const map = new Map<number, [string, string]>();
	for (const name in errno) map.set(-Math.abs(errno[name]), [name, errnoMessages[name] ?? name]);
	return map;
}
export function getSystemErrorName(err: number): string {
	if (typeof err !== "number") throw invalidArgType("err", "of type number", err);
	if (!Number.isSafeInteger(err) || err >= 0)
		throw codedError(
			"ERR_OUT_OF_RANGE",
			'The value of "err" is out of range. It must be a negative integer.',
			RangeError,
		);
	return getSystemErrorMap().get(err)?.[0] ?? `Unknown system error ${err}`;
}
export function getSystemErrorMessage(err: number): string {
	getSystemErrorName(err);
	return getSystemErrorMap().get(err)?.[1] ?? `Unknown system error ${err}`;
}
const token = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/;
export class MIMEParams implements Iterable<[string, string]> {
	private data = new Map<string, string>();
	get(name: string): string | null {
		return this.data.get(String(name)) ?? null;
	}
	has(name: string): boolean {
		return this.data.has(String(name));
	}
	set(name: string, value: string): void {
		name = String(name);
		value = String(value);
		if (!token.test(name) || /[^\t\x20-\x7e\x80-\xff]/.test(value))
			throw codedError("ERR_INVALID_MIME_SYNTAX", `The MIME syntax for a parameter in "${name}" is invalid`);
		this.data.set(name, value);
	}
	delete(name: string): void {
		this.data.delete(String(name));
	}
	entries(): MapIterator<[string, string]> {
		return this.data.entries();
	}
	keys(): MapIterator<string> {
		return this.data.keys();
	}
	values(): MapIterator<string> {
		return this.data.values();
	}
	[Symbol.iterator](): MapIterator<[string, string]> {
		return this.entries();
	}
	toString(): string {
		return Array.from(this.data, ([k, v]) => `${k}=${token.test(v) ? v : `"${v.replace(/["\\]/g, "\\$&")}"`}`).join(
			";",
		);
	}
	toJSON(): string {
		return this.toString();
	}
}
export class MIMEType {
	private _type: string;
	private _subtype: string;
	readonly params = new MIMEParams();
	constructor(input: unknown) {
		const s = String(input).trim();
		const slash = s.indexOf("/");
		const semi = s.indexOf(";");
		this._type = s.slice(0, slash).toLowerCase();
		this._subtype = s
			.slice(slash + 1, semi < 0 ? undefined : semi)
			.trimEnd()
			.toLowerCase();
		if (slash < 1 || !token.test(this._type) || !token.test(this._subtype))
			throw codedError("ERR_INVALID_MIME_SYNTAX", `The MIME syntax for a type in "${s}" is invalid`);
		if (semi >= 0) {
			const re = /;\s*([^=;\s]+)\s*=\s*(?:"((?:\\.|[^"\\])*)"|([^;]*))/g;
			re.lastIndex = semi;
			let m: RegExpExecArray | null;
			while ((m = re.exec(s))) {
				const name = m[1].toLowerCase();
				if (!this.params.has(name))
					try {
						this.params.set(name, m[2] !== undefined ? m[2].replace(/\\(.)/g, "$1") : m[3].trimEnd());
					} catch {}
			}
		}
	}
	get type(): string {
		return this._type;
	}
	set type(v: string) {
		v = String(v);
		if (!token.test(v)) throw codedError("ERR_INVALID_MIME_SYNTAX", "Invalid MIME type");
		this._type = v.toLowerCase();
	}
	get subtype(): string {
		return this._subtype;
	}
	set subtype(v: string) {
		v = String(v);
		if (!token.test(v)) throw codedError("ERR_INVALID_MIME_SYNTAX", "Invalid MIME subtype");
		this._subtype = v.toLowerCase();
	}
	get essence(): string {
		return `${this.type}/${this.subtype}`;
	}
	toString(): string {
		const p = String(this.params);
		return this.essence + (p ? `;${p}` : "");
	}
	toJSON(): string {
		return this.toString();
	}
}
export function transferableAbortSignal<T extends AbortSignal>(signal: T): T {
	if (!signal || typeof signal.addEventListener !== "function")
		throw invalidArgType("signal", "an AbortSignal", signal);
	return signal;
}
export function transferableAbortController(): AbortController {
	return new AbortController();
}
export function aborted(signal: AbortSignal, resource: object): Promise<void> {
	transferableAbortSignal(signal);
	if (resource === null || (typeof resource !== "object" && typeof resource !== "function"))
		throw invalidArgType("resource", "of type object", resource);
	if (signal.aborted) return Promise.resolve();
	const p = Promise.withResolvers<void>();
	const ref = new WeakRef(resource);
	signal.addEventListener(
		"abort",
		() => {
			if (ref.deref()) p.resolve();
		},
		{ once: true },
	);
	return p.promise;
}
export function getCallSites(
	frameCount = 10,
): { functionName: string; scriptName: string; lineNumber: number; columnNumber: number; column: number }[] {
	if (!Number.isInteger(frameCount) || frameCount < 1 || frameCount > 200)
		throw codedError("ERR_OUT_OF_RANGE", "frameCount must be between 1 and 200", RangeError);
	return (new Error().stack ?? "")
		.split("\n")
		.slice(2, frameCount + 2)
		.map((line) => {
			const m = /at (?:(.*?) \()?(.+?):(\d+):(\d+)\)?$/.exec(line.trim());
			return {
				functionName: m?.[1] ?? "",
				scriptName: m?.[2] ?? "",
				lineNumber: Number(m?.[3] ?? 0),
				columnNumber: Number(m?.[4] ?? 0),
				column: Number(m?.[4] ?? 0),
			};
		});
}
/** Parse Node's dotenv grammar without mutating the process environment. */
export function parseEnv(content: string): Record<string, string> {
	if (typeof content !== "string") throw invalidArgType("content", "of type string", content);
	const result: Record<string, string> = {};
	content = content.replace(/^\uFEFF/, "").replace(/\r\n?/g, "\n");
	let i = 0;
	while (i < content.length) {
		while (i < content.length && /\s/.test(content[i])) i++;
		if (content[i] === "#") {
			while (i < content.length && content[i] !== "\n") i++;
			continue;
		}
		if (content.startsWith("export", i) && /\s/.test(content[i + 6] ?? "")) {
			i += 6;
			while (/[ \t]/.test(content[i] ?? "") && i < content.length) i++;
		}
		const start = i;
		while (i < content.length && content[i] !== "=" && content[i] !== "\n") i++;
		if (content[i] !== "=") {
			i++;
			continue;
		}
		const name = content.slice(start, i++).trim();
		while (i < content.length && /[ \t]/.test(content[i])) i++;
		let value = "";
		const quote = content[i];
		if (quote === "'" || quote === '"' || quote === "`") {
			i++;
			const begin = i;
			while (i < content.length && content[i] !== quote) i++;
			value = content.slice(begin, i);
			if (i < content.length) i++;
			while (i < content.length && content[i] !== "\n") i++;
		} else {
			const begin = i;
			while (i < content.length && content[i] !== "\n" && content[i] !== "#") i++;
			value = content.slice(begin, i).trim();
			while (i < content.length && content[i] !== "\n") i++;
		}
		if (name && name !== "__proto__")
			Object.defineProperty(result, name, { value, writable: true, enumerable: true, configurable: true });
	}
	return result;
}
export const getCallSite = getCallSites;
export default {
	inspect,
	format,
	formatWithOptions,
	stripVTControlCharacters,
	types,
	parseArgs,
	TextEncoder,
	TextDecoder,
	isDeepStrictEqual,
	promisify,
	callbackify,
	deprecate,
	inherits,
	isArray,
	isBoolean,
	isNull,
	isNullOrUndefined,
	isNumber,
	isString,
	isSymbol,
	isUndefined,
	isRegExp,
	isObject,
	isDate,
	isError,
	isFunction,
	isPrimitive,
	isBuffer,
	_extend,
	toUSVString,
	styleText,
	debuglog,
	debug,
	getSystemErrorMap,
	getSystemErrorName,
	getSystemErrorMessage,
	MIMEType,
	MIMEParams,
	transferableAbortSignal,
	transferableAbortController,
	aborted,
	getCallSites,
	getCallSite,
	parseEnv,
};
