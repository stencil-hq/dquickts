// WHATWG Headers (Bun flavor: lowercase iteration, getSetCookie, toJSON,
// count, getAll). Entries keep the name as first given, used on the wire.

export type HeadersInit =
	| Headers
	| Iterable<readonly [string, string] | readonly string[]>
	| Record<string, string | readonly string[] | undefined>;

export type HeadersGuard = "none" | "immutable" | "request" | "response";

interface HeaderEntry {
	/** name as first given (sent on the wire) */
	name: string;
	/** combined value (", " joined), unused for set-cookie */
	value: string;
	/** set-cookie values, kept apart */
	cookies: string[] | null;
}

const kMap = Symbol("headers map");
const kGuard = Symbol("headers guard");
const kSorted = Symbol("headers sorted");

const TOKEN = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
const INVALID_VALUE = /[\0\r\n]/;

/** Strip leading and trailing HTTP whitespace (tab, space, CR, LF). */
export function normalizeValue(value: string): string {
	let start = 0;
	let end = value.length;
	while (start < end) {
		const c = value.charCodeAt(start);
		if (c !== 0x20 && c !== 0x09 && c !== 0x0a && c !== 0x0d) break;
		start++;
	}
	while (end > start) {
		const c = value.charCodeAt(end - 1);
		if (c !== 0x20 && c !== 0x09 && c !== 0x0a && c !== 0x0d) break;
		end--;
	}
	return start === 0 && end === value.length ? value : value.slice(start, end);
}

function checkName(name: string): string {
	if (!TOKEN.test(name)) throw new TypeError(`Invalid header name: "${name}"`);
	return name;
}

function checkValue(name: string, value: string): string {
	const v = normalizeValue(value);
	if (INVALID_VALUE.test(v)) throw new TypeError(`Header '${name}' has invalid value: '${v}'`);
	return v;
}

function byteString(value: unknown): string {
	return typeof value === "string" ? value : String(value);
}

export class Headers {
	[kMap]: Map<string, HeaderEntry> = new Map();
	[kGuard]: HeadersGuard = "none";
	[kSorted]: [string, string][] | null = null;

	constructor(init?: HeadersInit) {
		Object.defineProperty(this, kMap, { enumerable: false });
		Object.defineProperty(this, kGuard, { enumerable: false });
		Object.defineProperty(this, kSorted, { enumerable: false, writable: true });
		if (init !== undefined && init !== null) fillHeaders(this, init);
		else if (init === null) throw new TypeError("Headers constructor: init must be a sequence or a record");
	}

	append(name: string, value: string): void {
		if (arguments.length < 2) throw new TypeError("Headers.append requires 2 arguments");
		name = checkName(byteString(name));
		value = checkValue(name, byteString(value));
		if (this[kGuard] === "immutable") throw new TypeError("Headers are immutable");
		appendHeader(this, name, value);
	}

	delete(name: string): void {
		name = checkName(byteString(name));
		if (this[kGuard] === "immutable") throw new TypeError("Headers are immutable");
		if (this[kMap].delete(name.toLowerCase())) this[kSorted] = null;
	}

	get(name: string): string | null {
		name = checkName(byteString(name));
		const entry = this[kMap].get(name.toLowerCase());
		if (!entry) return null;
		return entry.cookies ? entry.cookies.join(", ") : entry.value;
	}

	getSetCookie(): string[] {
		const entry = this[kMap].get("set-cookie");
		return entry?.cookies ? entry.cookies.slice() : [];
	}

	/** Bun: only "set-cookie" is supported, like getSetCookie(). */
	getAll(name: string): string[] {
		name = checkName(byteString(name)).toLowerCase();
		if (name !== "set-cookie") throw new TypeError('Only "set-cookie" is supported.');
		return this.getSetCookie();
	}

	has(name: string): boolean {
		name = checkName(byteString(name));
		return this[kMap].has(name.toLowerCase());
	}

	set(name: string, value: string): void {
		if (arguments.length < 2) throw new TypeError("Headers.set requires 2 arguments");
		name = checkName(byteString(name));
		value = checkValue(name, byteString(value));
		if (this[kGuard] === "immutable") throw new TypeError("Headers are immutable");
		const lower = name.toLowerCase();
		const existing = this[kMap].get(lower);
		this[kMap].set(lower, {
			name: existing?.name ?? name,
			value,
			cookies: lower === "set-cookie" ? [value] : null,
		});
		this[kSorted] = null;
	}

	forEach(callback: (value: string, name: string, headers: Headers) => void, thisArg?: unknown): void {
		if (typeof callback !== "function") throw new TypeError("Headers.forEach: callback must be a function");
		const list = sortedEntries(this);
		for (let i = 0; i < list.length; i++) callback.call(thisArg, list[i][1], list[i][0], this);
	}

	*keys(): IterableIterator<string> {
		for (const [name] of sortedEntries(this)) yield name;
	}

	*values(): IterableIterator<string> {
		for (const [, value] of sortedEntries(this)) yield value;
	}

	*entries(): IterableIterator<[string, string]> {
		for (const [name, value] of sortedEntries(this)) yield [name, value];
	}

	[Symbol.iterator](): IterableIterator<[string, string]> {
		return this.entries();
	}

	/** Bun: the number of distinct headers (each set-cookie counts). */
	get count(): number {
		let n = 0;
		for (const entry of this[kMap].values()) n += entry.cookies ? entry.cookies.length : 1;
		return n;
	}

	/** Bun: a plain object; set-cookie is an array. */
	toJSON(): Record<string, string | string[]> {
		const out: Record<string, string | string[]> = {};
		for (const [lower, entry] of [...this[kMap].entries()].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0))) {
			out[lower] = entry.cookies ? entry.cookies.slice() : entry.value;
		}
		return out;
	}

	get [Symbol.toStringTag](): string {
		return "Headers";
	}

	[Symbol.for("nodejs.util.inspect.custom")](
		depth: number,
		options: object,
		inspect: (v: unknown, o?: object) => string,
	): string {
		const shown = inspect(this.toJSON(), options);
		return `Headers ${shown}`;
	}
}

function appendHeader(headers: Headers, name: string, value: string): void {
	const lower = name.toLowerCase();
	const map = headers[kMap];
	const entry = map.get(lower);
	if (lower === "set-cookie") {
		if (entry?.cookies) entry.cookies.push(value);
		else map.set(lower, { name, value, cookies: [value] });
	} else if (entry) {
		entry.value = `${entry.value}, ${value}`;
	} else {
		map.set(lower, { name, value, cookies: null });
	}
	headers[kSorted] = null;
}

function sortedEntries(headers: Headers): [string, string][] {
	let list = headers[kSorted];
	if (list) return list;
	const names = [...headers[kMap].keys()].sort();
	list = [];
	for (const name of names) {
		const entry = headers[kMap].get(name)!;
		if (entry.cookies) for (const cookie of entry.cookies) list.push([name, cookie]);
		else list.push([name, entry.value]);
	}
	headers[kSorted] = list;
	return list;
}

/** Append every header of `init` (Headers, sequence of pairs or record). */
export function fillHeaders(headers: Headers, init: HeadersInit): void {
	if (init instanceof Headers) {
		for (const entry of init[kMap].values()) {
			if (entry.cookies) for (const cookie of entry.cookies) appendHeader(headers, entry.name, cookie);
			else appendHeader(headers, entry.name, entry.value);
		}
		return;
	}
	if (typeof init !== "object" || init === null) {
		throw new TypeError("Headers constructor: init must be a sequence or a record");
	}
	const iterable = init as Iterable<readonly string[]>;
	if (typeof iterable[Symbol.iterator] === "function") {
		for (const pair of iterable) {
			if (
				typeof pair !== "object" ||
				pair === null ||
				typeof (pair as Iterable<string>)[Symbol.iterator] !== "function"
			) {
				throw new TypeError("Headers constructor: expected name/value pair to be length 2, found 1.");
			}
			const items = Array.isArray(pair) ? pair : [...(pair as Iterable<string>)];
			if (items.length !== 2) {
				throw new TypeError(`Headers constructor: expected name/value pair to be length 2, found ${items.length}.`);
			}
			headers.append(byteString(items[0]), byteString(items[1]));
		}
		return;
	}
	const record = init as Record<string, string | readonly string[] | undefined>;
	for (const key in record) {
		if (!Object.prototype.hasOwnProperty.call(record, key)) continue;
		const value = record[key];
		if (value === undefined) continue;
		if (Array.isArray(value)) for (const v of value) headers.append(key, byteString(v));
		else headers.append(key, byteString(value));
	}
}

export function setGuard(headers: Headers, guard: HeadersGuard): void {
	headers[kGuard] = guard;
}

export function getGuard(headers: Headers): HeadersGuard {
	return headers[kGuard];
}

/** Append without validation (headers received from the network). */
export function appendRaw(headers: Headers, name: string, value: string): void {
	appendHeader(headers, name, value);
}

/** Delete without guard checks. */
export function deleteRaw(headers: Headers, name: string): void {
	if (headers[kMap].delete(name.toLowerCase())) headers[kSorted] = null;
}

/** Set without validation or guard checks. */
export function setRaw(headers: Headers, name: string, value: string): void {
	const lower = name.toLowerCase();
	const existing = headers[kMap].get(lower);
	headers[kMap].set(lower, { name: existing?.name ?? name, value, cookies: lower === "set-cookie" ? [value] : null });
	headers[kSorted] = null;
}

/** The header list for the wire: [name as given, value] (set-cookie apart). */
export function wireEntries(headers: Headers): [string, string][] {
	const out: [string, string][] = [];
	for (const entry of headers[kMap].values()) {
		if (entry.cookies) for (const cookie of entry.cookies) out.push([entry.name, cookie]);
		else out.push([entry.name, entry.value]);
	}
	return out;
}

/** A copy of `headers` with the same guard-free content. */
export function cloneHeaders(headers: Headers, guard: HeadersGuard = headers[kGuard]): Headers {
	const copy = new Headers();
	for (const [lower, entry] of headers[kMap]) {
		copy[kMap].set(lower, {
			name: entry.name,
			value: entry.value,
			cookies: entry.cookies ? entry.cookies.slice() : null,
		});
	}
	copy[kGuard] = guard;
	return copy;
}

/** The parsed MIME essence + params of the content-type header ("" if none). */
export function contentType(headers: Headers): string {
	return headers[kMap].get("content-type")?.value ?? "";
}

export function isToken(value: string): boolean {
	return TOKEN.test(value);
}
