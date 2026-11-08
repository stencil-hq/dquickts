import { codedError, invalidArgType } from "internal:errors";

function bytes(input?: ArrayBuffer | ArrayBufferView): Uint8Array {
	if (input === undefined) return new Uint8Array(0);
	if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
	if (input instanceof ArrayBuffer || Object.prototype.toString.call(input) === "[object SharedArrayBuffer]")
		return new Uint8Array(input as ArrayBuffer);
	throw invalidArgType("input", "an ArrayBuffer or ArrayBufferView", input);
}
export class TextEncoder {
	get encoding(): string {
		return "utf-8";
	}
	encode(input = ""): Uint8Array {
		const s = String(input),
			out = new Uint8Array(s.length * 3);
		const { written } = this.encodeInto(s, out);
		return out.slice(0, written);
	}
	encodeInto(source: string, destination: Uint8Array): { read: number; written: number } {
		if (typeof source !== "string") throw invalidArgType("src", "a string", source);
		if (!(destination instanceof Uint8Array)) throw invalidArgType("dest", "a Uint8Array", destination);
		let read = 0,
			written = 0;
		while (read < source.length) {
			let c = source.charCodeAt(read),
				n = 1;
			if (c >= 0xd800 && c <= 0xdbff) {
				const low = source.charCodeAt(read + 1);
				if (low >= 0xdc00 && low <= 0xdfff) {
					c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
					n = 2;
				} else c = 0xfffd;
			} else if (c >= 0xdc00 && c <= 0xdfff) c = 0xfffd;
			const size = c < 128 ? 1 : c < 2048 ? 2 : c < 65536 ? 3 : 4;
			if (written + size > destination.length) break;
			if (size === 1) destination[written++] = c;
			else {
				if (size === 4) destination[written++] = 0xf0 | (c >> 18);
				if (size >= 3) destination[written++] = (size === 3 ? 0xe0 : 0x80) | ((c >> 12) & 63);
				destination[written++] = (size === 2 ? 0xc0 : 0x80) | ((c >> 6) & (size === 2 ? 31 : 63));
				destination[written++] = 0x80 | (c & 63);
			}
			read += n;
		}
		return { read, written };
	}
	get [Symbol.toStringTag](): string {
		return "TextEncoder";
	}
}
const windows = [
	0x20ac, 0x81, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021, 0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0x8d, 0x17d, 0x8f,
	0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0x9d, 0x17e, 0x178,
];
export class TextDecoder {
	readonly encoding: string;
	readonly fatal: boolean;
	readonly ignoreBOM: boolean;
	private pending = new Uint8Array(0);
	private started = false;
	constructor(label = "utf-8", options: { fatal?: boolean; ignoreBOM?: boolean } = {}) {
		const name = String(label)
			.replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, "")
			.toLowerCase();
		if (["utf-8", "utf8", "unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "x-unicode20utf8"].includes(name))
			this.encoding = "utf-8";
		else if (["utf-16", "utf-16le", "ucs-2", "unicode", "unicodefeff", "csunicode", "iso-10646-ucs-2"].includes(name))
			this.encoding = "utf-16le";
		else if (["utf-16be", "unicodefffe"].includes(name)) this.encoding = "utf-16be";
		else if (
			[
				"latin1",
				"latin-1",
				"l1",
				"iso-8859-1",
				"iso8859-1",
				"iso88591",
				"iso_8859-1",
				"iso_8859-1:1987",
				"cp819",
				"ibm819",
				"csisolatin1",
				"windows-1252",
				"cp1252",
				"x-cp1252",
				"ascii",
				"us-ascii",
				"ansi_x3.4-1968",
			].includes(name)
		)
			this.encoding = "windows-1252";
		else throw codedError("ERR_ENCODING_NOT_SUPPORTED", `The "${label}" encoding is not supported`, RangeError);
		this.fatal = Boolean(options?.fatal);
		this.ignoreBOM = Boolean(options?.ignoreBOM);
	}
	decode(input?: ArrayBuffer | ArrayBufferView, options: { stream?: boolean } = {}): string {
		let data = bytes(input);
		if (this.pending.length) {
			const joined = new Uint8Array(this.pending.length + data.length);
			joined.set(this.pending);
			joined.set(data, this.pending.length);
			data = joined;
		}
		this.pending = new Uint8Array(0);
		const stream = Boolean(options?.stream);
		let out = "",
			i = 0;
		const error = (): string => {
			if (this.fatal) {
				this.pending = new Uint8Array(0);
				this.started = false;
				throw codedError(
					"ERR_ENCODING_INVALID_ENCODED_DATA",
					`The encoded data was not valid for encoding ${this.encoding}`,
					TypeError,
				);
			}
			return "\ufffd";
		};
		while (i < data.length) {
			if (this.encoding === "windows-1252") {
				const c = data[i++];
				out += String.fromCharCode(c >= 128 && c < 160 ? windows[c - 128] : c);
				continue;
			}
			const start = i;
			if (this.encoding !== "utf-8") {
				if (i + 1 >= data.length) {
					if (stream) break;
					out += error();
					i++;
					continue;
				}
				const be = this.encoding === "utf-16be";
				const c = be ? data[i] * 256 + data[i + 1] : data[i] + data[i + 1] * 256;
				i += 2;
				if (c >= 0xd800 && c <= 0xdbff) {
					if (i + 1 >= data.length) {
						if (stream) {
							i = start;
							break;
						}
						out += error();
						continue;
					}
					const d = be ? data[i] * 256 + data[i + 1] : data[i] + data[i + 1] * 256;
					if (d >= 0xdc00 && d <= 0xdfff) {
						out += String.fromCharCode(c, d);
						i += 2;
					} else out += error();
				} else out += c >= 0xdc00 && c <= 0xdfff ? error() : String.fromCharCode(c);
				continue;
			}
			const first = data[i++];
			if (first < 128) {
				out += String.fromCharCode(first);
				continue;
			}
			const n =
				first >= 0xc2 && first <= 0xdf
					? 2
					: first >= 0xe0 && first <= 0xef
						? 3
						: first >= 0xf0 && first <= 0xf4
							? 4
							: 0;
			if (!n) {
				out += error();
				continue;
			}
			let c = first & (n === 2 ? 31 : n === 3 ? 15 : 7),
				valid = true;
			for (let j = 1; j < n; j++) {
				if (i === data.length) {
					if (stream) {
						i = start;
						valid = false;
					} else {
						out += error();
						valid = false;
					}
					break;
				}
				const b = data[i],
					lower = j === 1 && first === 0xe0 ? 0xa0 : j === 1 && first === 0xf0 ? 0x90 : 0x80,
					upper = j === 1 && first === 0xed ? 0x9f : j === 1 && first === 0xf4 ? 0x8f : 0xbf;
				if (b < lower || b > upper) {
					out += error();
					valid = false;
					break;
				}
				i++;
				c = (c << 6) | (b & 63);
			}
			if (i === start) break;
			if (valid) out += String.fromCodePoint(c);
		}
		if (stream && i < data.length) this.pending = data.slice(i);
		if (out.length && !this.started) {
			this.started = true;
			if (!this.ignoreBOM && this.encoding !== "windows-1252" && out.charCodeAt(0) === 0xfeff) out = out.slice(1);
		}
		if (!stream) this.started = false;
		return out;
	}
	get [Symbol.toStringTag](): string {
		return "TextDecoder";
	}
}
