import { Buffer } from "node:buffer";
export function unescapeBuffer(s: string, decodeSpaces = false): Buffer {
	const out = Buffer.allocUnsafe(s.length);
	let n = 0;
	for (let i = 0; i < s.length; i++) {
		const c = s.charCodeAt(i);
		if (c === 43 && decodeSpaces) out[n++] = 32;
		else if (c === 37 && i + 2 < s.length && /^[\da-f]{2}$/i.test(s.slice(i + 1, i + 3))) {
			out[n++] = parseInt(s.slice(i + 1, i + 3), 16);
			i += 2;
		} else out[n++] = c;
	}
	return out.subarray(0, n);
}
export function unescape(s: string, decodeSpaces = false): string {
	try {
		return decodeURIComponent(s);
	} catch {
		return unescapeBuffer(s, decodeSpaces).toString();
	}
}
export function escape(input: unknown): string {
	return encodeURIComponent(typeof input === "string" ? input : String(input));
}
export function parse(
	input: string,
	sep = "&",
	eq = "=",
	options: { maxKeys?: number; decodeURIComponent?: (s: string) => string } = {},
): Record<string, string | string[]> {
	const result = Object.create(null) as Record<string, string | string[]>;
	if (typeof input !== "string" || !input.length) return result;
	sep = sep || "&";
	eq = eq || "=";
	const custom = options?.decodeURIComponent,
		decode = typeof custom === "function" ? custom : unescape;
	const convert = (s: string): string => {
		s = s.replace(/\+/g, custom ? "%20" : " ");
		try {
			return decode(s);
		} catch {
			return unescape(s, true);
		}
	};
	const limit = typeof options?.maxKeys === "number" ? options.maxKeys : 1000;
	let start = 0,
		count = 0;
	while (start <= input.length && (limit <= 0 || count < limit)) {
		let end = input.indexOf(sep, start);
		if (end < 0) end = input.length;
		const pair = input.slice(start, end);
		count++;
		if (pair.length) {
			const split = pair.indexOf(eq),
				key = convert(split < 0 ? pair : pair.slice(0, split)),
				value = convert(split < 0 ? "" : pair.slice(split + eq.length));
			const old = result[key];
			if (old === undefined) result[key] = value;
			else if (Array.isArray(old)) old.push(value);
			else result[key] = [old, value];
		}
		if (end === input.length) break;
		start = end + sep.length;
	}
	return result;
}
function primitive(value: unknown): string {
	return typeof value === "string"
		? value
		: typeof value === "bigint" || typeof value === "boolean" || (typeof value === "number" && Number.isFinite(value))
			? String(value)
			: "";
}
export function stringify(
	input: unknown,
	sep = "&",
	eq = "=",
	options: { encodeURIComponent?: (s: string) => string } = {},
): string {
	if (input === null || typeof input !== "object") return "";
	const encodeValue = typeof options?.encodeURIComponent === "function" ? options.encodeURIComponent : escape;
	const parts: string[] = [];
	const object = input as Record<string, unknown>;
	for (const key in object)
		if (Object.prototype.hasOwnProperty.call(object, key)) {
			const prefix = encodeValue(key) + eq,
				value = object[key];
			if (Array.isArray(value)) {
				for (let i = 0; i < value.length; i++) parts.push(prefix + encodeValue(primitive(value[i])));
			} else parts.push(prefix + encodeValue(primitive(value)));
		}
	return parts.join(sep);
}
export { parse as decode, stringify as encode };
export default { parse, decode: parse, stringify, encode: stringify, escape, unescape, unescapeBuffer };
