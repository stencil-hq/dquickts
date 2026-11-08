import { Buffer } from "node:buffer";
import { codedError, invalidArgType } from "internal:errors";
export class StringDecoder {
	encoding: string;
	lastNeed = 0;
	lastTotal = 0;
	lastChar = Buffer.alloc(4);
	private pending = Buffer.alloc(0);
	constructor(encoding = "utf8") {
		const e = String(encoding).toLowerCase();
		this.encoding =
			e === "utf-8" ? "utf8" : ["utf-16le", "ucs2", "ucs-2"].includes(e) ? "utf16le" : e === "binary" ? "latin1" : e;
		if (!["utf8", "utf16le", "latin1", "ascii", "base64", "base64url", "hex"].includes(this.encoding))
			throw codedError("ERR_UNKNOWN_ENCODING", `Unknown encoding: ${encoding}`, TypeError);
	}
	write(input: string | ArrayBufferView): string {
		if (typeof input === "string") return input;
		if (!ArrayBuffer.isView(input)) throw invalidArgType("buf", "a Buffer, TypedArray, or DataView", input);
		let b = Buffer.from(input.buffer, input.byteOffset, input.byteLength);
		if (this.pending.length) b = Buffer.concat([this.pending, b]);
		let end = b.length,
			total = 0;
		if (this.encoding === "utf8") {
			let start = end - 1;
			while (start >= 0 && end - start <= 4 && (b[start] & 0xc0) === 0x80) start--;
			if (start >= 0) {
				const c = b[start];
				total = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
				if (total && end - start < total) {
					let valid = true;
					for (let j = start + 1; j < end; j++)
						if (
							b[j] < (j === start + 1 && c === 0xe0 ? 0xa0 : j === start + 1 && c === 0xf0 ? 0x90 : 0x80) ||
							b[j] > (j === start + 1 && c === 0xed ? 0x9f : j === start + 1 && c === 0xf4 ? 0x8f : 0xbf)
						)
							valid = false;
					if (valid) end = start;
				}
			}
		} else if (this.encoding === "utf16le") {
			end -= end % 2;
			total = 2;
			if (end >= 2) {
				const c = b[end - 2] | (b[end - 1] << 8);
				if (c >= 0xd800 && c <= 0xdbff) {
					end -= 2;
					total = 4;
				}
			}
		} else if (this.encoding === "base64" || this.encoding === "base64url") {
			end -= end % 3;
			total = 3;
		}
		this.pending = Buffer.from(b.subarray(end));
		this.lastNeed = this.pending.length ? total - this.pending.length : 0;
		this.lastTotal = this.pending.length ? total : 0;
		if (this.pending.length) this.lastChar.set(this.pending);
		return b.toString(this.encoding, 0, end);
	}
	end(input?: string | ArrayBufferView): string {
		let out = input === undefined ? "" : this.write(input);
		if (this.pending.length) out += this.pending.toString(this.encoding);
		this.pending = Buffer.alloc(0);
		this.lastNeed = this.lastTotal = 0;
		return out;
	}
	text(input: Uint8Array, offset: number): string {
		this.pending = Buffer.alloc(0);
		this.lastNeed = this.lastTotal = 0;
		return this.write(input.subarray(offset));
	}
}
export default { StringDecoder };
