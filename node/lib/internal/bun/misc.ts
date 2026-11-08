import * as core from "qbun:core";
import * as native from "qbun:misc";
import { inspect as nodeInspect, isDeepStrictEqual } from "node:util";
import { spawn } from "node:child_process";
import { randomBytes } from "node:crypto";
import { codedError, notSupported } from "internal:errors";
export function stringWidth(
	value: string,
	options: boolean | { countAnsiEscapeCodes?: boolean; ambiguousIsNarrow?: boolean } = false,
	ambiguousIsNarrow = true,
): number {
	return typeof options === "object"
		? native.stringWidth(value, options.countAnsiEscapeCodes ?? false, options.ambiguousIsNarrow ?? true)
		: native.stringWidth(value, options, ambiguousIsNarrow);
}
export const stripANSI = native.stripANSI;
export const escapeHTML = native.escapeHTML;
const colorNames: Record<string, string> = {};
const namedColors =
	"aliceblue:f0f8ff antiquewhite:faebd7 aqua:00ffff aquamarine:7fffd4 azure:f0ffff beige:f5f5dc bisque:ffe4c4 black:000000 blanchedalmond:ffebcd blue:0000ff blueviolet:8a2be2 brown:a52a2a burlywood:deb887 cadetblue:5f9ea0 chartreuse:7fff00 chocolate:d2691e coral:ff7f50 cornflowerblue:6495ed cornsilk:fff8dc crimson:dc143c cyan:00ffff darkblue:00008b darkcyan:008b8b darkgoldenrod:b8860b darkgray:a9a9a9 darkgreen:006400 darkgrey:a9a9a9 darkkhaki:bdb76b darkmagenta:8b008b darkolivegreen:556b2f darkorange:ff8c00 darkorchid:9932cc darkred:8b0000 darksalmon:e9967a darkseagreen:8fbc8f darkslateblue:483d8b darkslategray:2f4f4f darkslategrey:2f4f4f darkturquoise:00ced1 darkviolet:9400d3 deeppink:ff1493 deepskyblue:00bfff dimgray:696969 dimgrey:696969 dodgerblue:1e90ff firebrick:b22222 floralwhite:fffaf0 forestgreen:228b22 fuchsia:ff00ff gainsboro:dcdcdc ghostwhite:f8f8ff gold:ffd700 goldenrod:daa520 gray:808080 green:008000 greenyellow:adff2f grey:808080 honeydew:f0fff0 hotpink:ff69b4 indianred:cd5c5c indigo:4b0082 ivory:fffff0 khaki:f0e68c lavender:e6e6fa lavenderblush:fff0f5 lawngreen:7cfc00 lemonchiffon:fffacd lightblue:add8e6 lightcoral:f08080 lightcyan:e0ffff lightgoldenrodyellow:fafad2 lightgray:d3d3d3 lightgreen:90ee90 lightgrey:d3d3d3 lightpink:ffb6c1 lightsalmon:ffa07a lightseagreen:20b2aa lightskyblue:87cefa lightslategray:778899 lightslategrey:778899 lightsteelblue:b0c4de lightyellow:ffffe0 lime:00ff00 limegreen:32cd32 linen:faf0e6 magenta:ff00ff maroon:800000 mediumaquamarine:66cdaa mediumblue:0000cd mediumorchid:ba55d3 mediumpurple:9370db mediumseagreen:3cb371 mediumslateblue:7b68ee mediumspringgreen:00fa9a mediumturquoise:48d1cc mediumvioletred:c71585 midnightblue:191970 mintcream:f5fffa mistyrose:ffe4e1 moccasin:ffe4b5 navajowhite:ffdead navy:000080 oldlace:fdf5e6 olive:808000 olivedrab:6b8e23 orange:ffa500 orangered:ff4500 orchid:da70d6 palegoldenrod:eee8aa palegreen:98fb98 paleturquoise:afeeee palevioletred:db7093 papayawhip:ffefd5 peachpuff:ffdab9 peru:cd853f pink:ffc0cb plum:dda0dd powderblue:b0e0e6 purple:800080 rebeccapurple:663399 red:ff0000 rosybrown:bc8f8f royalblue:4169e1 saddlebrown:8b4513 salmon:fa8072 sandybrown:f4a460 seagreen:2e8b57 seashell:fff5ee sienna:a0522d silver:c0c0c0 skyblue:87ceeb slateblue:6a5acd slategray:708090 slategrey:708090 snow:fffafa springgreen:00ff7f steelblue:4682b4 tan:d2b48c teal:008080 thistle:d8bfd8 tomato:ff6347 turquoise:40e0d0 violet:ee82ee wheat:f5deb3 white:ffffff whitesmoke:f5f5f5 yellow:ffff00 yellowgreen:9acd32";
for (const pair of namedColors.split(" ")) {
	const [name, hex] = pair.split(":");
	colorNames[name] = hex;
}
export function color(input: unknown, format = "css"): unknown {
	let r = 0,
		g = 0,
		b = 0,
		a = 1;
	if (typeof input === "number") {
		r = (input >>> 16) & 255;
		g = (input >>> 8) & 255;
		b = input & 255;
	} else if (Array.isArray(input) || ArrayBuffer.isView(input)) {
		const values = Array.from(input as ArrayLike<number>);
		if (values.length < 3) return null;
		[r, g, b] = values;
		a = values[3] ?? 1;
	} else if (input && typeof input === "object" && "r" in input && "g" in input && "b" in input) {
		r = Number(input.r);
		g = Number(input.g);
		b = Number(input.b);
		if ("a" in input) a = Number(input.a);
	} else {
		let s = String(input).trim().toLowerCase();
		if (s === "transparent") {
			s = "#000000";
			a = 0;
		}
		if (colorNames[s]) s = "#" + colorNames[s];
		if (/^#[\da-f]+$/i.test(s)) {
			let h = s.slice(1);
			if (h.length === 3 || h.length === 4) h = [...h].map((c) => c + c).join("");
			if (h.length !== 6 && h.length !== 8) return null;
			r = parseInt(h.slice(0, 2), 16);
			g = parseInt(h.slice(2, 4), 16);
			b = parseInt(h.slice(4, 6), 16);
			if (h.length === 8) a = parseInt(h.slice(6), 16) / 255;
		} else {
			const m = /^(rgba?|hsla?)\((.*)\)$/.exec(s);
			if (!m) return null;
			const p = m[2].trim().split(/\s*[,/]\s*|\s+/);
			if (p.length < 3 || p.length > 4) return null;
			if (p[3] !== undefined) a = parseFloat(p[3]) / (p[3].endsWith("%") ? 100 : 1);
			if (m[1].startsWith("rgb")) [r, g, b] = p.slice(0, 3).map((v) => parseFloat(v) * (v.endsWith("%") ? 2.55 : 1));
			else {
				let h = parseFloat(p[0]);
				if (p[0].endsWith("turn")) h *= 360;
				else if (p[0].endsWith("rad")) h *= 180 / Math.PI;
				h = (((h % 360) + 360) % 360) / 60;
				const sat = Math.max(0, Math.min(1, parseFloat(p[1]) / 100)),
					l = Math.max(0, Math.min(1, parseFloat(p[2]) / 100));
				const c = (1 - Math.abs(2 * l - 1)) * sat,
					x = c * (1 - Math.abs((h % 2) - 1)),
					z = l - c / 2;
				const rgb =
					h < 1 ? [c, x, 0] : h < 2 ? [x, c, 0] : h < 3 ? [0, c, x] : h < 4 ? [0, x, c] : h < 5 ? [x, 0, c] : [c, 0, x];
				[r, g, b] = rgb.map((v) => (v + z) * 255);
			}
		}
	}
	if (![r, g, b, a].every(Number.isFinite)) return null;
	[r, g, b] = [r, g, b].map((v) => Math.round(Math.max(0, Math.min(255, v))));
	a = Math.max(0, Math.min(1, a));
	const hex = "#" + [r, g, b].map((v) => v.toString(16).padStart(2, "0")).join("");
	switch (format) {
		case "number":
			return (r << 16) | (g << 8) | b;
		case "[rgb]":
			return [r, g, b];
		case "[rgba]":
			return [r, g, b, a];
		case "{rgb}":
			return { r, g, b };
		case "{rgba}":
			return { r, g, b, a };
		case "hex":
			return hex;
		case "HEX":
			return hex.toUpperCase();
		case "rgb":
			return `rgb(${r}, ${g}, ${b})`;
		case "rgba":
			return `rgba(${r}, ${g}, ${b}, ${a})`;
		case "ansi":
		case "ansi-16m":
			return `\x1b[38;2;${r};${g};${b}m`;
		case "ansi-256": {
			const index =
				r === g && g === b
					? r < 8
						? 16
						: r > 248
							? 231
							: Math.round(((r - 8) / 247) * 24) + 232
					: 16 + 36 * Math.round((r / 255) * 5) + 6 * Math.round((g / 255) * 5) + Math.round((b / 255) * 5);
			return `\x1b[38;5;${index}m`;
		}
		case "ansi-16": {
			const value = (b > 127 ? 4 : 0) | (g > 127 ? 2 : 0) | (r > 127 ? 1 : 0);
			return `\x1b[${Math.max(r, g, b) > 191 ? 90 + value : 30 + value}m`;
		}
		case "hsl": {
			const max = Math.max(r, g, b) / 255,
				min = Math.min(r, g, b) / 255,
				d = max - min,
				l = (max + min) / 2;
			let h = 0;
			if (d)
				h = max === r / 255 ? ((g - b) / 255 / d) % 6 : max === g / 255 ? (b - r) / 255 / d + 2 : (r - g) / 255 / d + 4;
			return `hsl(${(h * 60 + 360) % 360}, ${d ? (d / (1 - Math.abs(2 * l - 1))) * 100 : 0}%, ${l * 100}%)`;
		}
		case "css": {
			if (a !== 1) return `rgba(${r}, ${g}, ${b}, ${a})`;
			let shortest = hex;
			if (hex[1] === hex[2] && hex[3] === hex[4] && hex[5] === hex[6]) shortest = `#${hex[1]}${hex[3]}${hex[5]}`;
			for (const name in colorNames)
				if ("#" + colorNames[name] === hex && name.length < shortest.length) shortest = name;
			return shortest;
		}
		default:
			throw codedError("ERR_INVALID_ARG_VALUE", `Unknown color format: ${format}`);
	}
}
export function wrapAnsi(
	input: string,
	columns: number,
	options: { hard?: boolean; wordWrap?: boolean; trim?: boolean; ambiguousIsNarrow?: boolean } = {},
): string {
	columns = Math.max(1, Math.floor(columns));
	const tokens = input.match(/\x1b\[[0-?]*[ -/]*[@-~]|\x1b\](?:(?!\x07|\x1b\\)[\s\S])*(?:\x07|\x1b\\)|[^]/gu) ?? [];
	let result = "",
		line = "",
		width = 0,
		link = "";
	const styles = new Map<number, string>();
	const close = () =>
		(link ? "\x1b]8;;\x1b\\" : "") +
		[...styles.keys()]
			.reverse()
			.map((code) => `\x1b[${code}m`)
			.join("");
	const newline = () => {
		result += (options.trim !== false ? line.replace(/ +$/, "") : line) + close() + "\n";
		line = [...styles.values()].join("") + link;
		width = 0;
	};
	for (let i = 0; i < tokens.length; i++) {
		const tokenStart = i;
		let token = tokens[i];
		if (token[0] === "\x1b") {
			line += token;
			if (token.startsWith("\x1b]8;")) link = /^\x1b\]8;;(?:\x07|\x1b\\)$/.test(token) ? "" : token;
			else if (token.endsWith("m")) {
				const codes = token.slice(2, -1).split(";").map(Number);
				for (let j = 0; j < codes.length; j++) {
					const code = codes[j];
					if (code === 0) {
						styles.clear();
						continue;
					}
					if (styles.has(code)) {
						styles.delete(code);
						continue;
					}
					const end =
						code === 1 || code === 2
							? 22
							: code === 3
								? 23
								: code === 4
									? 24
									: code === 7
										? 27
										: code === 8
											? 28
											: code === 9
												? 29
												: (code >= 30 && code <= 37) || code === 38 || (code >= 90 && code <= 97)
													? 39
													: (code >= 40 && code <= 47) || code === 48 || (code >= 100 && code <= 107)
														? 49
														: 0;
					if (!end) continue;
					let sequence = `\x1b[${code}m`;
					if (code === 38 || code === 48) {
						const length = codes[j + 1] === 2 ? 4 : 2;
						sequence = `\x1b[${codes.slice(j, j + length + 1).join(";")}m`;
						j += length;
					}
					styles.set(end, sequence);
				}
			}
			continue;
		}
		if (token === "\n") {
			newline();
			continue;
		}
		// Keep extended emoji graphemes intact without requiring Intl.Segmenter.
		while (
			i + 1 < tokens.length &&
			(/^[\p{Mark}\uFE0F\u200D\u{1F3FB}-\u{1F3FF}]$/u.test(tokens[i + 1]) ||
				token.endsWith("\u200d") ||
				(/^[\u{1F1E6}-\u{1F1FF}]$/u.test(token) && /^[\u{1F1E6}-\u{1F1FF}]$/u.test(tokens[i + 1])))
		)
			token += tokens[++i];
		const w = stringWidth(token, false, options.ambiguousIsNarrow !== false);
		let previous = tokenStart - 1;
		while (previous >= 0 && tokens[previous][0] === "\x1b") previous--;
		if (options.wordWrap !== false && token !== " " && (width === 0 || previous < 0 || tokens[previous] === " ")) {
			let word = token;
			for (let j = i + 1; j < tokens.length && tokens[j] !== " " && tokens[j] !== "\n"; j++) word += tokens[j];
			if (width && width + stringWidth(word, false, options.ambiguousIsNarrow !== false) > columns) newline();
		}
		if (width + w > columns && width > 0 && (options.hard || options.wordWrap === false)) newline();
		if (token === " " && width === 0 && options.trim !== false) continue;
		if (token === " " && width >= columns && options.wordWrap !== false) {
			newline();
			continue;
		}
		line += token;
		width += w;
	}
	return result + (options.trim !== false ? line.replace(/ +$/, "") : line);
}
type InspectOptions = { depth?: number; colors?: boolean; [key: string]: unknown };
export const inspect = Object.assign(
	function (value: unknown, options: InspectOptions = {}): string {
		return nodeInspect(value, { depth: 2, colors: false, ...options });
	},
	{
		custom: Symbol.for("nodejs.util.inspect.custom"),
		table(value: unknown, properties?: string[], options: InspectOptions = {}): string {
			const rows: [string, unknown][] = [];
			if (value instanceof Map) {
				for (const [k, v] of value) rows.push([inspect(k), v]);
			} else if (value instanceof Set) {
				let i = 0;
				for (const v of value) rows.push([String(i++), v]);
			} else if (value && typeof value === "object") {
				for (const k in value) if (Object.hasOwn(value, k)) rows.push([k, (value as Record<string, unknown>)[k]]);
			} else return inspect(value, options);
			const names = properties?.slice() ?? [];
			if (!properties)
				for (const [, row] of rows)
					if (row && typeof row === "object")
						for (const key in row) if (Object.hasOwn(row, key) && !names.includes(key)) names.push(key);
			if (rows.some(([, row]) => !row || typeof row !== "object")) names.push("Values");
			const cells = [
				["(index)", ...names],
				...rows.map(([key, row]) => [
					key,
					...names.map((name) =>
						name === "Values" && (!row || typeof row !== "object")
							? inspect(row, options)
							: row && typeof row === "object" && name in row
								? inspect((row as Record<string, unknown>)[name], options)
								: "",
					),
				]),
			];
			const widths = cells[0].map((_, i) => Math.max(...cells.map((row) => stringWidth(row[i]))));
			const border = (left: string, middle: string, right: string) =>
				left + widths.map((w) => "─".repeat(w + 2)).join(middle) + right;
			const lines = cells.map(
				(row) => "│ " + row.map((cell, i) => cell + " ".repeat(widths[i] - stringWidth(cell))).join(" │ ") + " │",
			);
			lines.splice(1, 0, border("├", "┼", "┤"));
			return [border("┌", "┬", "┐"), ...lines, border("└", "┴", "┘")].join("\n");
		},
	},
);
export function deepEquals(a: unknown, b: unknown, strict = false): boolean {
	if (strict) return isDeepStrictEqual(a, b);
	return equal(a, b, new Map());
}
function equal(a: unknown, b: unknown, seen: Map<object, object>): boolean {
	if (Object.is(a, b)) return true;
	if (!a || !b || typeof a !== "object" || typeof b !== "object") return false;
	if (Object.prototype.toString.call(a) !== Object.prototype.toString.call(b)) return false;
	if (seen.has(a)) return seen.get(a) === b;
	seen.set(a, b);
	if (a instanceof Date && b instanceof Date) return Object.is(+a, +b);
	if (a instanceof RegExp && b instanceof RegExp) return String(a) === String(b) && a.lastIndex === b.lastIndex;
	if (a instanceof Error && b instanceof Error && (a.name !== b.name || a.message !== b.message)) return false;
	if (Array.isArray(a) && Array.isArray(b) && a.length !== b.length) return false;
	if (a instanceof ArrayBuffer && b instanceof ArrayBuffer) {
		const x = new Uint8Array(a),
			y = new Uint8Array(b);
		return x.length === y.length && x.every((v, i) => v === y[i]);
	}
	if (ArrayBuffer.isView(a) && ArrayBuffer.isView(b)) {
		const x = new Uint8Array(a.buffer, a.byteOffset, a.byteLength),
			y = new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
		return x.length === y.length && x.every((v, i) => v === y[i]);
	}
	if (a instanceof Map && b instanceof Map) {
		if (a.size !== b.size) return false;
		const rest = [...b];
		for (const [k, v] of a) {
			const i = rest.findIndex(([l, w]) => equal(k, l, new Map(seen)) && equal(v, w, new Map(seen)));
			if (i < 0) return false;
			rest.splice(i, 1);
		}
		return true;
	}
	if (a instanceof Set && b instanceof Set) {
		if (a.size !== b.size) return false;
		const rest = [...b];
		for (const v of a) {
			const i = rest.findIndex((w) => equal(v, w, new Map(seen)));
			if (i < 0) return false;
			rest.splice(i, 1);
		}
		return true;
	}
	if (a instanceof WeakMap || a instanceof WeakSet || a instanceof Promise) return false;
	const x = a as Record<PropertyKey, unknown>,
		y = b as Record<PropertyKey, unknown>;
	const keys = Reflect.ownKeys(x).filter((k) => Object.prototype.propertyIsEnumerable.call(x, k));
	const other = Reflect.ownKeys(y).filter((k) => Object.prototype.propertyIsEnumerable.call(y, k));
	return keys.length === other.length && keys.every((k) => Object.hasOwn(y, k) && equal(x[k], y[k], seen));
}
export function deepMatch(value: unknown, subset: unknown): boolean {
	if (Object.is(value, subset)) return true;
	if (!value || !subset || typeof value !== "object" || typeof subset !== "object") return false;
	for (const k in subset)
		if (!deepMatch((value as Record<string, unknown>)[k], (subset as Record<string, unknown>)[k])) return false;
	return true;
}
export const peek = Object.assign(
	function <T>(value: T): T {
		const state = core.promiseState(value) as [number, T] | undefined;
		if (!state) return value;
		if (state[0] === 2) throw state[1];
		return state[0] === 1 ? state[1] : value;
	},
	{
		status(value: unknown): string {
			const state = core.promiseState(value) as [number, unknown] | undefined;
			return !state ? "fulfilled" : ["pending", "fulfilled", "rejected"][state[0]];
		},
	},
);
export function sleep(ms: number | Date): Promise<void> {
	const p = Promise.withResolvers<void>();
	setTimeout(p.resolve, ms instanceof Date ? Math.max(0, +ms - Date.now()) : ms);
	return p.promise;
}
export function sleepSync(ms: number): void {
	if (ms > 0) Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}
const clockOrigin = core.startTime() as bigint;
export function nanoseconds(): number {
	return Number((core.hrtime() as bigint) - clockOrigin);
}
export function gc(_force?: boolean): void {
	core.gc();
}
export function generateHeapSnapshot(): object {
	return {
		snapshot: { meta: { node_fields: [], edge_fields: [] }, node_count: 0, edge_count: 0 },
		nodes: [],
		edges: [],
		strings: [],
	};
}
export function shrink(): void {
	core.gc();
}
export function allocUnsafe(size: number): Uint8Array {
	return new Uint8Array(size);
}
function bytes(value: ArrayBuffer | ArrayBufferView | string): Uint8Array {
	return typeof value === "string"
		? new TextEncoder().encode(value)
		: ArrayBuffer.isView(value)
			? new Uint8Array(value.buffer, value.byteOffset, value.byteLength)
			: new Uint8Array(value);
}
export function concatArrayBuffers(
	buffers: (ArrayBuffer | ArrayBufferView)[],
	maxLength = Infinity,
	asUint8Array = false,
): ArrayBuffer | Uint8Array {
	let n = 0;
	for (const b of buffers) n += b.byteLength;
	n = Math.min(n, maxLength);
	const out = new Uint8Array(n);
	let i = 0;
	for (const b of buffers) {
		const v = bytes(b);
		const z = Math.min(n - i, v.length);
		out.set(v.subarray(0, z), i);
		i += z;
		if (i === n) break;
	}
	return asUint8Array ? out : out.buffer;
}
export function indexOfLine(buffer: ArrayBuffer | ArrayBufferView, offset = 0): number {
	return bytes(buffer).indexOf(10, offset);
}
export const unsafe = {
	arrayBufferToString(buffer: ArrayBuffer | ArrayBufferView): string {
		return new TextDecoder().decode(bytes(buffer));
	},
	gcAggressionLevel(_level?: number): number {
		return 0;
	},
	segfault(): never {
		throw notSupported("unsafe.segfault");
	},
};
export class ArrayBufferSink {
	private chunks: Uint8Array[] = [];
	private size = 0;
	private options: { asUint8Array?: boolean; stream?: boolean } = {};
	start(options: { asUint8Array?: boolean; stream?: boolean } = {}): void {
		this.options = options;
		this.chunks = [];
		this.size = 0;
	}
	write(value: ArrayBuffer | ArrayBufferView | string): number {
		const b = bytes(value).slice();
		this.chunks.push(b);
		this.size += b.length;
		return b.length;
	}
	flush(): number | Uint8Array | ArrayBuffer {
		if (!this.options.stream) return 0;
		return this.take();
	}
	private take(): Uint8Array | ArrayBuffer {
		const result = concatArrayBuffers(this.chunks, Infinity, this.options.asUint8Array);
		this.chunks = [];
		this.size = 0;
		return result;
	}
	end(): Uint8Array | ArrayBuffer {
		return this.take();
	}
	get [Symbol.toStringTag](): string {
		return "ArrayBufferSink";
	}
}
export async function readableStreamToArray<T>(stream: ReadableStream<T>): Promise<T[]> {
	const reader = stream.getReader(),
		out: T[] = [];
	try {
		for (;;) {
			const r = await reader.read();
			if (r.done) break;
			out.push(r.value);
		}
	} finally {
		reader.releaseLock();
	}
	return out;
}
export async function readableStreamToBytes(stream: ReadableStream): Promise<Uint8Array> {
	return concatArrayBuffers(
		(await readableStreamToArray(stream)).map((v) => bytes(v as ArrayBuffer | string)),
		Infinity,
		true,
	) as Uint8Array;
}
export async function readableStreamToArrayBuffer(stream: ReadableStream): Promise<ArrayBuffer> {
	return (await readableStreamToBytes(stream)).buffer;
}
export async function readableStreamToText(stream: ReadableStream): Promise<string> {
	return new TextDecoder().decode(await readableStreamToBytes(stream));
}
export async function readableStreamToJSON(stream: ReadableStream): Promise<unknown> {
	return JSON.parse(await readableStreamToText(stream));
}
export async function readableStreamToBlob(stream: ReadableStream): Promise<Blob> {
	return new Blob(await readableStreamToArray(stream));
}
export async function readableStreamToFormData(stream: ReadableStream, headers?: HeadersInit): Promise<FormData> {
	return new Response(await readableStreamToBytes(stream), { headers }).formData();
}
export function openInEditor(path: string, options: { editor?: string; line?: number; column?: number } = {}): void {
	const editor = options.editor || process.env.VISUAL || process.env.EDITOR || "vi";
	spawn(editor, [...(options.line ? [`+${options.line}`] : []), path], { stdio: "inherit" });
}
export function randomUUIDv7(
	encoding: "hex" | "base64" | "base64url" | "buffer" = "hex",
	timestamp = Date.now(),
): string | Uint8Array {
	const b = randomBytes(16);
	let t = BigInt(timestamp);
	for (let i = 5; i >= 0; i--) {
		b[i] = Number(t & 255n);
		t >>= 8n;
	}
	b[6] = (b[6] & 15) | 112;
	b[8] = (b[8] & 63) | 128;
	if (encoding === "buffer") return b;
	const h = Array.from(b, (v) => v.toString(16).padStart(2, "0")).join("");
	if (encoding === "hex")
		return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
	return Buffer.from(b).toString(encoding);
}
export interface CookieOptions {
	name?: string;
	value?: string;
	domain?: string;
	path?: string;
	expires?: Date | number | string;
	maxAge?: number;
	secure?: boolean;
	httpOnly?: boolean;
	sameSite?: "strict" | "lax" | "none";
	partitioned?: boolean;
}
export class Cookie {
	name: string;
	value: string;
	domain?: string;
	path?: string;
	expires?: Date;
	maxAge?: number;
	secure = false;
	httpOnly = false;
	sameSite?: string;
	partitioned = false;
	constructor(name: string | CookieOptions, value = "", options: CookieOptions = {}) {
		const o = typeof name === "object" ? name : { ...options, name, value };
		this.name = o.name ?? "";
		this.value = o.value ?? "";
		this.domain = o.domain;
		this.path = o.path;
		this.expires = o.expires === undefined ? undefined : new Date(o.expires);
		this.maxAge = o.maxAge;
		this.secure = !!o.secure;
		this.httpOnly = !!o.httpOnly;
		this.sameSite = o.sameSite;
		this.partitioned = !!o.partitioned;
	}
	static parse(text: string): Cookie {
		const [head, ...attrs] = text.split(";");
		const i = head.indexOf("=");
		const c = new Cookie(head.slice(0, i).trim(), head.slice(i + 1).trim());
		for (const attr of attrs) {
			const [key, ...rest] = attr.trim().split("=");
			const v = rest.join("=");
			switch (key.toLowerCase()) {
				case "domain":
					c.domain = v;
					break;
				case "path":
					c.path = v;
					break;
				case "expires":
					c.expires = new Date(v);
					break;
				case "max-age":
					c.maxAge = Number(v);
					break;
				case "secure":
					c.secure = true;
					break;
				case "httponly":
					c.httpOnly = true;
					break;
				case "samesite":
					c.sameSite = v.toLowerCase();
					break;
				case "partitioned":
					c.partitioned = true;
			}
		}
		return c;
	}
	isExpired(): boolean {
		return this.maxAge !== undefined ? this.maxAge <= 0 : !!this.expires && +this.expires <= Date.now();
	}
	serialize(): string {
		return this.toString();
	}
	toString(): string {
		let s = `${this.name}=${this.value}`;
		if (this.domain) s += `; Domain=${this.domain}`;
		if (this.path) s += `; Path=${this.path}`;
		if (this.expires) s += `; Expires=${this.expires.toUTCString()}`;
		if (this.maxAge !== undefined) s += `; Max-Age=${this.maxAge}`;
		if (this.secure) s += "; Secure";
		if (this.httpOnly) s += "; HttpOnly";
		if (this.sameSite) s += `; SameSite=${this.sameSite[0].toUpperCase() + this.sameSite.slice(1)}`;
		if (this.partitioned) s += "; Partitioned";
		return s;
	}
	toJSON(): CookieOptions {
		return { ...this } as CookieOptions;
	}
}
export class CookieMap implements Iterable<[string, string]> {
	private map = new Map<string, Cookie>();
	private changed = new Set<string>();
	constructor(input: string | Record<string, string> | Iterable<[string, string]> = "") {
		if (typeof input === "string") {
			for (const part of input.split(";")) {
				const i = part.indexOf("=");
				if (i >= 0) {
					const name = part.slice(0, i).trim();
					let v = part.slice(i + 1).trim();
					try {
						v = decodeURIComponent(v);
					} catch {}
					this.map.set(name, new Cookie(name, v));
				}
			}
		} else if (Symbol.iterator in input) {
			for (const [k, v] of input as Iterable<[string, string]>) this.map.set(k, new Cookie(k, v));
		} else for (const k in input) this.map.set(k, new Cookie(k, (input as Record<string, string>)[k]));
	}
	get size(): number {
		return this.map.size;
	}
	get(name: string): string | null {
		return this.map.get(name)?.value ?? null;
	}
	has(name: string): boolean {
		return this.map.has(name);
	}
	set(name: string | CookieOptions, value?: string, options: CookieOptions = {}): void {
		const c = typeof name === "string" ? new Cookie(name, value, options) : new Cookie(name);
		this.map.set(c.name, c);
		this.changed.add(c.name);
	}
	delete(name: string | CookieOptions): void {
		const c = typeof name === "string" ? { name } : name;
		this.set({ ...c, value: "", maxAge: 0 });
	}
	*entries(): IterableIterator<[string, string]> {
		for (const [k, v] of this.map) yield [k, v.value];
	}
	keys(): IterableIterator<string> {
		return this.map.keys();
	}
	*values(): IterableIterator<string> {
		for (const c of this.map.values()) yield c.value;
	}
	[Symbol.iterator](): IterableIterator<[string, string]> {
		return this.entries();
	}
	forEach(fn: (value: string, key: string, map: CookieMap) => void, thisArg?: unknown): void {
		for (const [k, v] of this) fn.call(thisArg, v, k, this);
	}
	toSetCookieHeaders(): string[] {
		return [...this.changed].map((k) => this.map.get(k)!.toString());
	}
	toJSON(): Record<string, string> {
		return Object.fromEntries(this);
	}
	toString(): string {
		return [...this].map(([k, v]) => `${k}=${encodeURIComponent(v)}`).join("; ");
	}
}
