import { getBlobBytes } from "node:buffer";

export type Input = string | ArrayBufferLike | ArrayBufferView;
export type DocumentInput = Input | Blob;
export function bytes(input: unknown): Uint8Array {
	if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
	if (input instanceof ArrayBuffer || (typeof SharedArrayBuffer !== "undefined" && input instanceof SharedArrayBuffer))
		return new Uint8Array(input);
	throw new TypeError("Expected a string, ArrayBuffer, or typed array");
}
export function decode(input: Uint8Array, fatal = true): string {
	return new TextDecoder("utf-8", { fatal, ignoreBOM: true }).decode(input);
}
export function text(input: DocumentInput): string {
	if (typeof input === "string") return input;
	let data: Uint8Array;
	if (
		ArrayBuffer.isView(input) ||
		input instanceof ArrayBuffer ||
		(typeof SharedArrayBuffer !== "undefined" && input instanceof SharedArrayBuffer)
	)
		data = bytes(input);
	else {
		// The buffer module validates the Blob and supplies an immutable internal
		// view. Decoding never mutates it; file-backed Bun.file inputs are rejected.
		data = getBlobBytes(input);
	}
	const value = decode(data);
	return value[0] === "\ufeff" ? value.slice(1) : value;
}
// Match the UTF-8 decoder's replacement-character consumption as well as valid
// Unicode, so JSONL progress remains a byte offset even for malformed strings.
export function utf8Offset(input: Uint8Array, characters: number): number {
	let position = 0;
	let decoded = 0;
	while (position < input.length && decoded < characters) {
		const lead = input[position++];
		if (lead < 0x80) {
			decoded++;
			continue;
		}
		const length =
			lead >= 0xc2 && lead <= 0xdf ? 2 : lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 1;
		let read = 1;
		for (; read < length && position < input.length; read++) {
			const next = input[position];
			if (
				next < 0x80 ||
				next > 0xbf ||
				(read === 1 &&
					((lead === 0xe0 && next < 0xa0) ||
						(lead === 0xed && next > 0x9f) ||
						(lead === 0xf0 && next < 0x90) ||
						(lead === 0xf4 && next > 0x8f)))
			)
				break;
			position++;
		}
		decoded += read === 4 ? 2 : 1;
	}
	return position;
}
export function incompleteUTF8(input: Uint8Array): number {
	if (!input.length) return 0;
	let lead = input.length - 1;
	while (lead >= 0 && input.length - lead <= 4 && (input[lead] & 0xc0) === 0x80) lead--;
	if (lead < 0) return 0;
	const c = input[lead];
	const needed = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 1;
	return input.length - lead < needed ? input.length - lead : 0;
}
export function offset(value: number | undefined, fallback: number, length: number, name: string): number {
	if (value === undefined) return fallback;
	if (typeof value !== "number" || !Number.isInteger(value) || value < 0 || value > length)
		throw new RangeError(`${name} must be an integer between 0 and ${length}`);
	return value;
}
