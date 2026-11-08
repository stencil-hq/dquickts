// Tar codec for Bun.Archive: ustar, GNU long names, and POSIX PAX metadata.
// Device nodes and GNU sparse extensions are deliberately rejected, never extracted.
import { codedError } from "internal:errors";

export interface TarEntry {
	path: string;
	type: "file" | "directory" | "symlink" | "hardlink";
	data: Uint8Array;
	linkpath: string;
	mode: number;
	mtime: number;
}

const BLOCK = 512;
const EMPTY = new Uint8Array(0);
type Pax = Record<string, string>;

export function readError(message: string): Error {
	const error = codedError("ERR_ARCHIVE_INVALID", message);
	error.name = "ReadError";
	return error;
}

function text(bytes: Uint8Array): string {
	return new TextDecoder("utf-8", { ignoreBOM: true }).decode(bytes);
}

function field(bytes: Uint8Array, start: number, length: number): string {
	let end = start;
	while (end < start + length && bytes[end] !== 0) end++;
	return text(bytes.subarray(start, end));
}

function number(bytes: Uint8Array, start: number, length: number): number {
	if (bytes[start] & 0x80) {
		let value = BigInt(bytes[start] & 0x7f);
		for (let i = start + 1; i < start + length; i++) value = (value << 8n) | BigInt(bytes[i]);
		if (bytes[start] & 0x40) value -= 1n << BigInt(length * 8 - 1);
		const result = Number(value);
		if (!Number.isSafeInteger(result)) throw readError("Tar numeric field exceeds the safe integer range");
		return result;
	}
	const value = field(bytes, start, length).trim();
	if (value === "") return 0;
	if (!/^[+-]?[0-7]+$/.test(value)) throw readError("Invalid tar numeric field");
	const result = parseInt(value, 8);
	if (!Number.isSafeInteger(result)) throw readError("Tar numeric field exceeds the safe integer range");
	return result;
}

function parsePax(data: Uint8Array): Pax {
	const result: Pax = Object.create(null);
	let offset = 0;
	while (offset < data.length) {
		let space = offset;
		while (space < data.length && data[space] >= 48 && data[space] <= 57) space++;
		if (space === offset || data[space] !== 32) throw readError("Invalid PAX record length");
		const length = Number(text(data.subarray(offset, space)));
		const end = offset + length;
		if (!Number.isSafeInteger(length) || end > data.length || end <= space + 2 || data[end - 1] !== 10) {
			throw readError("Truncated or invalid PAX record");
		}
		let equals = space + 1;
		while (equals < end - 1 && data[equals] !== 61) equals++;
		if (equals === space + 1 || equals === end - 1) throw readError("Invalid PAX key/value pair");
		const key = text(data.subarray(space + 1, equals));
		if (
			key.startsWith("GNU.sparse.") ||
			key === "SCHILY.realsize" ||
			(key === "SCHILY.filetype" && text(data.subarray(equals + 1, end - 1)) === "sparse")
		) {
			throw codedError("ERR_NOT_SUPPORTED", "Bun.Archive does not support sparse tar entries");
		}
		result[key] = text(data.subarray(equals + 1, end - 1));
		offset = end;
	}
	return result;
}

function paxNumber(value: string, key: string, integer: boolean): number {
	if (!/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)$/.test(value)) throw readError(`Invalid PAX ${key}`);
	const result = Number(value);
	if (
		!Number.isFinite(result) ||
		Math.abs(result) > Number.MAX_SAFE_INTEGER ||
		(integer && (!Number.isSafeInteger(result) || result < 0))
	) {
		throw readError(`Invalid PAX ${key}`);
	}
	return result;
}

function validPath(path: string): string {
	if (!path || path.includes("\0")) throw readError("Tar entry has an empty or invalid path");
	return path.replace(/\\/g, "/");
}

export function readTar(bytes: Uint8Array): TarEntry[] {
	const entries: TarEntry[] = [];
	let offset = 0;
	const globalPax: Pax = Object.create(null);
	let localPax: Pax = Object.create(null);
	let longPath: string | undefined;
	let longLink: string | undefined;
	if (bytes.length < BLOCK) throw readError("Truncated tar archive");
	while (offset < bytes.length) {
		if (bytes.length - offset < BLOCK) throw readError("Truncated tar header");
		const header = bytes.subarray(offset, offset + BLOCK);
		let zero = true;
		for (const byte of header)
			if (byte !== 0) {
				zero = false;
				break;
			}
		if (zero) break;
		let checksum = 0;
		let signedChecksum = 0;
		for (let i = 0; i < BLOCK; i++) {
			const byte = i >= 148 && i < 156 ? 32 : header[i];
			checksum += byte;
			signedChecksum += byte > 127 ? byte - 256 : byte;
		}
		const expected = number(header, 148, 8);
		if (checksum !== expected && signedChecksum !== expected) throw readError("Invalid tar header checksum");
		const kind = header[156];
		let size = number(header, 124, 12);
		let path = field(header, 0, 100);
		if (field(header, 257, 6) === "ustar") {
			const prefix = field(header, 345, 155);
			if (prefix) path = `${prefix}/${path}`;
		}
		const extension = kind === 120 || kind === 103 || kind === 76 || kind === 75;
		const pax: Pax = Object.create(null);
		if (!extension) {
			for (const key in globalPax) pax[key] = globalPax[key];
			for (const key in localPax) {
				if (localPax[key] === "") delete pax[key];
				else pax[key] = localPax[key];
			}
			if (pax.size !== undefined) size = paxNumber(pax.size, "size", true);
		}
		if (!Number.isSafeInteger(size) || size < 0) throw readError("Invalid tar entry size");
		offset += BLOCK;
		const padded = Math.ceil(size / BLOCK) * BLOCK;
		if (!Number.isSafeInteger(padded) || padded > bytes.length - offset) throw readError("Truncated tar entry data");
		const data = bytes.subarray(offset, offset + size);
		offset += padded;
		if (kind === 120 || kind === 103) {
			const values = parsePax(data);
			if (kind === 103) {
				for (const key in values) {
					if (values[key] === "") delete globalPax[key];
					else globalPax[key] = values[key];
				}
			} else {
				for (const key in values) localPax[key] = values[key];
			}
			continue;
		}
		if (kind === 76 || kind === 75) {
			let end = data.indexOf(0);
			if (end === -1) end = data.length;
			const value = text(data.subarray(0, end));
			if (kind === 76) longPath = value;
			else longLink = value;
			continue;
		}
		path = validPath(pax.path ?? longPath ?? path);
		const linkpath = (pax.linkpath ?? longLink ?? field(header, 157, 100)).replace(/\\/g, "/");
		const mtime = pax.mtime === undefined ? number(header, 136, 12) : paxNumber(pax.mtime, "mtime", false);
		const mode = number(header, 100, 8) & 0o7777;
		localPax = Object.create(null);
		longPath = undefined;
		longLink = undefined;
		let type: TarEntry["type"];
		if (kind === 0 || kind === 48 || kind === 55) type = path.endsWith("/") ? "directory" : "file";
		else if (kind === 53) type = "directory";
		else if (kind === 50) type = "symlink";
		else if (kind === 49) type = "hardlink";
		else
			throw codedError(
				"ERR_NOT_SUPPORTED",
				`Bun.Archive does not support tar entry type ${String.fromCharCode(kind)} (${path})`,
			);
		entries.push({ path, type, data: type === "file" ? data : EMPTY, linkpath, mode, mtime });
	}
	return entries;
}

function utf8(value: string): Uint8Array {
	return new TextEncoder().encode(value);
}

function putText(header: Uint8Array, offset: number, value: string): void {
	header.set(utf8(value), offset);
}

function putNumber(header: Uint8Array, offset: number, length: number, value: number): void {
	const octal = Math.floor(value).toString(8);
	if (value < 0 || octal.length >= length)
		throw codedError("ERR_OUT_OF_RANGE", "Tar number cannot be encoded in an octal header");
	putText(header, offset, octal.padStart(length - 1, "0"));
}

function splitPath(path: string): { name: string; prefix: string } | undefined {
	if (utf8(path).length <= 100) return { name: path, prefix: "" };
	for (let i = path.length - 1; i > 0; i--) {
		if (path[i] !== "/" || i === path.length - 1) continue;
		const prefix = path.slice(0, i);
		const name = path.slice(i + 1);
		if (utf8(prefix).length <= 155 && utf8(name).length <= 100) return { name, prefix };
	}
	return undefined;
}

function headerFor(
	name: string,
	prefix: string,
	type: string,
	size: number,
	mode: number,
	mtime: number,
	linkpath: string,
): Uint8Array {
	const header = new Uint8Array(BLOCK);
	putText(header, 0, name);
	putNumber(header, 100, 8, mode & 0o7777);
	putNumber(header, 108, 8, 0);
	putNumber(header, 116, 8, 0);
	putNumber(header, 124, 12, size);
	putNumber(header, 136, 12, mtime);
	header.fill(32, 148, 156);
	putText(header, 156, type);
	putText(header, 157, linkpath);
	putText(header, 257, "ustar");
	putText(header, 263, "00");
	putText(header, 345, prefix);
	let checksum = 0;
	for (const byte of header) checksum += byte;
	putText(header, 148, checksum.toString(8).padStart(6, "0"));
	header[154] = 0;
	header[155] = 32;
	return header;
}

function paxRecord(key: string, value: string): Uint8Array {
	const body = `${key}=${value}\n`;
	const bodyLength = utf8(body).length;
	let length = bodyLength + 3;
	while (true) {
		const next = bodyLength + String(length).length + 1;
		if (next === length) break;
		length = next;
	}
	return utf8(`${length} ${body}`);
}

export function writeTar(entries: readonly TarEntry[]): Uint8Array {
	const chunks: Uint8Array[] = [];
	let total = 0;
	const add = (bytes: Uint8Array): void => {
		chunks.push(bytes);
		total += bytes.length;
	};
	const addData = (bytes: Uint8Array): void => {
		add(bytes);
		const padding = (BLOCK - (bytes.length % BLOCK)) % BLOCK;
		if (padding) add(new Uint8Array(padding));
	};
	for (let index = 0; index < entries.length; index++) {
		const entry = entries[index];
		const path = validPath(entry.path);
		const split = splitPath(path);
		const pax: Uint8Array[] = [];
		if (!split) pax.push(paxRecord("path", path));
		if (utf8(entry.linkpath).length > 100) pax.push(paxRecord("linkpath", entry.linkpath));
		const size = entry.type === "file" ? entry.data.length : 0;
		const maxOctal = 0o77777777777;
		if (size > maxOctal) pax.push(paxRecord("size", String(size)));
		if (!Number.isInteger(entry.mtime) || entry.mtime < 0 || entry.mtime > maxOctal)
			pax.push(paxRecord("mtime", String(entry.mtime)));
		if (pax.length) {
			let length = 0;
			for (const record of pax) length += record.length;
			const data = new Uint8Array(length);
			let offset = 0;
			for (const record of pax) {
				data.set(record, offset);
				offset += record.length;
			}
			add(headerFor(`PaxHeaders/${index}`, "", "x", length, 0o644, 0, ""));
			addData(data);
		}
		const type = entry.type === "file" ? "0" : entry.type === "directory" ? "5" : entry.type === "symlink" ? "2" : "1";
		add(
			headerFor(
				split?.name ?? `PaxFile/${index}`,
				split?.prefix ?? "",
				type,
				Math.min(size, maxOctal),
				entry.mode,
				entry.mtime < 0 || entry.mtime > maxOctal ? 0 : Math.floor(entry.mtime),
				utf8(entry.linkpath).length <= 100 ? entry.linkpath : "",
			),
		);
		if (entry.type === "file") addData(entry.data);
	}
	add(new Uint8Array(BLOCK * 2));
	const result = new Uint8Array(total);
	let offset = 0;
	for (const chunk of chunks) {
		result.set(chunk, offset);
		offset += chunk.length;
	}
	return result;
}
