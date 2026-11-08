// internal:fs/utils — shared helpers of node:fs and node:fs/promises:
// argument validation, flags/modes, Stats/BigIntStats/StatFs/Dirent.

import * as binding from "qbun:fs";
import { fileURLToPath } from "node:url";
import { codedError, invalidArgType, invalidArgValue, outOfRange } from "internal:errors";

export const constants = binding.constants as Record<string, number>;

const {
	O_RDONLY,
	O_WRONLY,
	O_RDWR,
	O_CREAT,
	O_EXCL,
	O_TRUNC,
	O_APPEND,
	O_SYNC,
	S_IFMT,
	S_IFREG,
	S_IFDIR,
	S_IFCHR,
	S_IFBLK,
	S_IFIFO,
	S_IFLNK,
	S_IFSOCK,
	UV_DIRENT_UNKNOWN,
	UV_DIRENT_FILE,
	UV_DIRENT_DIR,
	UV_DIRENT_LINK,
	UV_DIRENT_FIFO,
	UV_DIRENT_SOCKET,
	UV_DIRENT_CHAR,
	UV_DIRENT_BLOCK,
} = constants;

export const kUsePromises = Symbol("fs_use_promises");

export type PathLike = string | Uint8Array | URL;
export type NativePath = string | Uint8Array;
export type BufferEncodingOpt = BufferEncoding | "buffer" | null | undefined;
export type Callback<T = void> = (err: NodeJS.ErrnoException | null, result?: T) => void;

interface URLLike {
	href: string;
	protocol: string;
	pathname: string;
	hostname: string;
}

function isURL(v: unknown): v is URLLike {
	return (
		v !== null &&
		typeof v === "object" &&
		typeof (v as URLLike).href === "string" &&
		typeof (v as URLLike).protocol === "string" &&
		(v as URLLike).pathname !== undefined
	);
}

function nullCheck(p: string | Uint8Array, name: string): void {
	const bad = typeof p === "string" ? p.includes("\u0000") : p.includes(0);
	if (bad) {
		throw codedError(
			"ERR_INVALID_ARG_VALUE",
			`The argument '${name}' must be a string, Uint8Array, or URL without null bytes. Received ${JSON.stringify(String(p))}`,
			TypeError,
		);
	}
}

/** Validate a path argument: string, Buffer/Uint8Array or file: URL → string | bytes. */
export function getValidatedPath(p: unknown, name = "path"): NativePath {
	let out: NativePath;
	if (typeof p === "string") out = p;
	else if (p instanceof Uint8Array) out = p;
	else if (isURL(p)) {
		if (p.protocol !== "file:") throw codedError("ERR_INVALID_URL_SCHEME", "The URL must be of scheme file", TypeError);
		out = fileURLToPath(p);
	} else throw invalidArgType(name, "of type string or an instance of Buffer or URL", p);
	nullCheck(out, name);
	return out;
}

/** A validated path as a string (Buffers decoded as UTF-8). */
export function pathString(p: unknown, name = "path"): string {
	const v = getValidatedPath(p, name);
	return typeof v === "string" ? v : Buffer.from(v.buffer, v.byteOffset, v.byteLength).toString("utf8");
}

export function getValidatedFd(fd: unknown, name = "fd"): number {
	if (typeof fd !== "number") throw invalidArgType(name, "of type number", fd);
	if (!Number.isInteger(fd)) throw outOfRange(name, "an integer", fd);
	if (fd < 0 || fd > 0x7fffffff) throw outOfRange(name, ">= 0 && <= 2147483647", fd);
	return fd;
}

export function stringToFlags(flags: unknown, name = "flags"): number {
	if (typeof flags === "number") return flags;
	if (flags == null) return O_RDONLY;
	switch (flags) {
		case "r":
			return O_RDONLY;
		case "rs":
		case "sr":
			return O_RDONLY | O_SYNC;
		case "r+":
			return O_RDWR;
		case "rs+":
		case "sr+":
			return O_RDWR | O_SYNC;
		case "w":
			return O_TRUNC | O_CREAT | O_WRONLY;
		case "wx":
		case "xw":
			return O_TRUNC | O_CREAT | O_WRONLY | O_EXCL;
		case "w+":
			return O_TRUNC | O_CREAT | O_RDWR;
		case "wx+":
		case "xw+":
			return O_TRUNC | O_CREAT | O_RDWR | O_EXCL;
		case "a":
			return O_APPEND | O_CREAT | O_WRONLY;
		case "ax":
		case "xa":
			return O_APPEND | O_CREAT | O_WRONLY | O_EXCL;
		case "as":
		case "sa":
			return O_APPEND | O_CREAT | O_WRONLY | O_SYNC;
		case "a+":
			return O_APPEND | O_CREAT | O_RDWR;
		case "ax+":
		case "xa+":
			return O_APPEND | O_CREAT | O_RDWR | O_EXCL;
		case "as+":
		case "sa+":
			return O_APPEND | O_CREAT | O_RDWR | O_SYNC;
	}
	throw invalidArgValue(name, flags);
}

const octalReg = /^[0-7]+$/;

export function parseFileMode(value: unknown, name: string, def?: number): number {
	if (value == null && def !== undefined) return def;
	if (typeof value === "string") {
		if (!octalReg.test(value)) {
			throw invalidArgValue(name, value, "must be a 32-bit unsigned integer or an octal string");
		}
		return Number.parseInt(value, 8);
	}
	if (typeof value !== "number") throw invalidArgType(name, "of type number", value);
	if (!Number.isInteger(value)) throw outOfRange(name, "an integer", value);
	if (value < 0 || value > 0xffffffff) throw outOfRange(name, ">= 0 && <= 4294967295", value);
	return value & 0o7777;
}

export function validateInteger(
	value: unknown,
	name: string,
	min = Number.MIN_SAFE_INTEGER,
	max = Number.MAX_SAFE_INTEGER,
): asserts value is number {
	if (typeof value !== "number") throw invalidArgType(name, "of type number", value);
	if (!Number.isInteger(value)) throw outOfRange(name, "an integer", value);
	if (value < min || value > max) throw outOfRange(name, `>= ${min} && <= ${max}`, value);
}

export function validateBoolean(value: unknown, name: string): asserts value is boolean {
	if (typeof value !== "boolean") throw invalidArgType(name, "of type boolean", value);
}

export function validateEncoding(enc: unknown): void {
	if (enc == null || enc === "buffer") return;
	if (typeof enc !== "string" || !Buffer.isEncoding(enc)) {
		throw codedError(
			"ERR_INVALID_ARG_VALUE",
			`The argument 'encoding' is invalid encoding. Received ${String(enc)}`,
			TypeError,
		);
	}
}

export interface BaseOptions {
	encoding?: BufferEncodingOpt;
	flag?: string | number;
	mode?: number | string;
	signal?: AbortSignal;
	[k: string]: unknown;
}

/** Normalize `options` (a string encoding, an object, or nothing). */
export function getOptions<T extends BaseOptions>(options: unknown, defaults: T): T {
	if (options == null || typeof options === "function") return defaults;
	let out: T;
	if (typeof options === "string") {
		out = { ...defaults, encoding: options as BufferEncoding };
	} else if (typeof options === "object") {
		out = { ...defaults, ...(options as T) };
	} else {
		throw invalidArgType("options", "one of type string or an instance of Object", options);
	}
	if (out.encoding !== "buffer") validateEncoding(out.encoding);
	if (out.signal !== undefined) validateAbortSignal(out.signal, "options.signal");
	return out;
}

export function validateAbortSignal(signal: unknown, name: string): void {
	if (signal !== undefined && (signal === null || typeof signal !== "object" || !("aborted" in signal))) {
		throw invalidArgType(name, "an instance of AbortSignal", signal);
	}
}

export function abortErr(signal?: AbortSignal): Error {
	const err = new Error("The operation was aborted") as Error & { code: string; cause?: unknown };
	err.name = "AbortError";
	err.code = "ABORT_ERR";
	if (signal && signal.reason !== undefined) err.cause = signal.reason;
	return err;
}

export function checkAborted(signal: AbortSignal | undefined): void {
	if (signal?.aborted) throw abortErr(signal);
}

/** Encode bytes as a string per `encoding`, or keep the Buffer. */
export function encodeBuffer(buf: Buffer, encoding: BufferEncodingOpt): string | Buffer {
	if (encoding == null || encoding === "buffer") return buf;
	return buf.toString(encoding);
}

/** Encode a path string per `encoding` ("buffer" → Buffer). */
export function encodePath(p: string, encoding: BufferEncodingOpt): string | Buffer {
	if (encoding === "buffer") return Buffer.from(p);
	if (encoding == null || encoding === "utf8" || encoding === "utf-8") return p;
	return Buffer.from(p).toString(encoding);
}

/** Data argument of write/writeFile/appendFile → bytes. */
export function toBytes(data: unknown, encoding: BufferEncodingOpt, name = "data"): Uint8Array {
	if (typeof data === "string")
		return Buffer.from(data, (encoding && encoding !== "buffer" ? encoding : "utf8") as BufferEncoding);
	if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
	throw invalidArgType(name, "of type string or an instance of Buffer, TypedArray, or DataView", data);
}

export function isArrayBufferView(v: unknown): v is ArrayBufferView {
	return ArrayBuffer.isView(v);
}

/** Seconds since the epoch for utimes. */
export function toUnixTimestamp(time: unknown, name = "time"): number {
	if (typeof time === "string" && +time === +time) return +time;
	if (typeof time === "number") {
		if (!Number.isFinite(time) || time < 0) return Date.now() / 1000;
		return time;
	}
	if (typeof time === "bigint") return Number(time);
	if (time instanceof Date) return time.getTime() / 1000;
	throw invalidArgType(name, "of type number or an instance of Date", time);
}

// ---------------------------------------------------------------- Stats

const kNsPerMsBigInt = 1_000_000n;
const kNsPerSecBigInt = 1_000_000_000n;

function dateFromMs(ms: number): Date {
	return new Date(Math.round(Number(ms)));
}

abstract class StatsBase<N extends number | bigint> {
	dev!: N;
	mode!: N;
	nlink!: N;
	uid!: N;
	gid!: N;
	rdev!: N;
	blksize!: N;
	ino!: N;
	size!: N;
	blocks!: N;

	protected checkModeProperty(property: number): boolean {
		return (Number(this.mode) & S_IFMT) === property;
	}
	isDirectory(): boolean {
		return this.checkModeProperty(S_IFDIR);
	}
	isFile(): boolean {
		return this.checkModeProperty(S_IFREG);
	}
	isBlockDevice(): boolean {
		return this.checkModeProperty(S_IFBLK);
	}
	isCharacterDevice(): boolean {
		return this.checkModeProperty(S_IFCHR);
	}
	isSymbolicLink(): boolean {
		return this.checkModeProperty(S_IFLNK);
	}
	isFIFO(): boolean {
		return this.checkModeProperty(S_IFIFO);
	}
	isSocket(): boolean {
		return this.checkModeProperty(S_IFSOCK);
	}
}

function defineDates(proto: object): void {
	for (const name of ["atime", "mtime", "ctime", "birthtime"]) {
		const ms = `${name}Ms`;
		const cache = Symbol(name);
		Object.defineProperty(proto, name, {
			enumerable: true,
			configurable: true,
			get(this: Record<string | symbol, unknown>): Date {
				let d = this[cache] as Date | undefined;
				if (d === undefined) {
					d = dateFromMs(Number(this[ms]));
					this[cache] = d;
				}
				return d;
			},
			set(this: Record<string | symbol, unknown>, v: unknown) {
				Object.defineProperty(this, name, { value: v, writable: true, enumerable: true, configurable: true });
			},
		});
	}
}

export class Stats extends StatsBase<number> {
	atimeMs: number;
	mtimeMs: number;
	ctimeMs: number;
	birthtimeMs: number;
	declare atime: Date;
	declare mtime: Date;
	declare ctime: Date;
	declare birthtime: Date;

	constructor(
		dev: number,
		mode: number,
		nlink: number,
		uid: number,
		gid: number,
		rdev: number,
		blksize: number,
		ino: number,
		size: number,
		blocks: number,
		atimeMs: number,
		mtimeMs: number,
		ctimeMs: number,
		birthtimeMs: number,
	) {
		super();
		this.dev = dev;
		this.mode = mode;
		this.nlink = nlink;
		this.uid = uid;
		this.gid = gid;
		this.rdev = rdev;
		this.blksize = blksize;
		this.ino = ino;
		this.size = size;
		this.blocks = blocks;
		this.atimeMs = atimeMs;
		this.mtimeMs = mtimeMs;
		this.ctimeMs = ctimeMs;
		this.birthtimeMs = birthtimeMs;
	}
}
defineDates(Stats.prototype);

export class BigIntStats extends StatsBase<bigint> {
	atimeMs: bigint;
	mtimeMs: bigint;
	ctimeMs: bigint;
	birthtimeMs: bigint;
	atimeNs: bigint;
	mtimeNs: bigint;
	ctimeNs: bigint;
	birthtimeNs: bigint;
	declare atime: Date;
	declare mtime: Date;
	declare ctime: Date;
	declare birthtime: Date;

	constructor(
		dev: bigint,
		mode: bigint,
		nlink: bigint,
		uid: bigint,
		gid: bigint,
		rdev: bigint,
		blksize: bigint,
		ino: bigint,
		size: bigint,
		blocks: bigint,
		atimeNs: bigint,
		mtimeNs: bigint,
		ctimeNs: bigint,
		birthtimeNs: bigint,
	) {
		super();
		this.dev = dev;
		this.mode = mode;
		this.nlink = nlink;
		this.uid = uid;
		this.gid = gid;
		this.rdev = rdev;
		this.blksize = blksize;
		this.ino = ino;
		this.size = size;
		this.blocks = blocks;
		this.atimeMs = atimeNs / kNsPerMsBigInt;
		this.mtimeMs = mtimeNs / kNsPerMsBigInt;
		this.ctimeMs = ctimeNs / kNsPerMsBigInt;
		this.birthtimeMs = birthtimeNs / kNsPerMsBigInt;
		this.atimeNs = atimeNs;
		this.mtimeNs = mtimeNs;
		this.ctimeNs = ctimeNs;
		this.birthtimeNs = birthtimeNs;
	}
	protected override checkModeProperty(property: number): boolean {
		return (Number(this.mode) & S_IFMT) === property;
	}
}
defineDates(BigIntStats.prototype);

export type StatsArray = (number | bigint)[];

/** Build Stats / BigIntStats from the native 18-field array. */
export function getStatsFromBinding(a: StatsArray, bigint = false): Stats | BigIntStats {
	if (bigint) {
		const b = a as bigint[];
		const ns = (i: number) => b[i] * kNsPerSecBigInt + b[i + 1];
		return new BigIntStats(b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], ns(10), ns(12), ns(14), ns(16));
	}
	const n = a as number[];
	const ms = (i: number) => n[i] * 1e3 + n[i + 1] / 1e6;
	return new Stats(n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], n[9], ms(10), ms(12), ms(14), ms(16));
}

export class StatFs<N extends number | bigint = number> {
	type: N;
	bsize: N;
	blocks: N;
	bfree: N;
	bavail: N;
	files: N;
	ffree: N;
	constructor(type: N, bsize: N, blocks: N, bfree: N, bavail: N, files: N, ffree: N) {
		this.type = type;
		this.bsize = bsize;
		this.blocks = blocks;
		this.bfree = bfree;
		this.bavail = bavail;
		this.files = files;
		this.ffree = ffree;
	}
}

export function getStatFsFromBinding(a: (number | bigint)[]): StatFs<number | bigint> {
	return new StatFs(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
}

// ---------------------------------------------------------------- Dirent

const kType = Symbol("type");

export class Dirent {
	name: string | Buffer;
	parentPath: string;
	[kType]: number;

	constructor(name: string | Buffer, type: number, path: string) {
		this.name = name;
		this.parentPath = path;
		this[kType] = type;
	}
	/** Node's legacy alias of parentPath */
	get path(): string {
		return this.parentPath;
	}
	set path(v: string) {
		this.parentPath = v;
	}
	isDirectory(): boolean {
		return this[kType] === UV_DIRENT_DIR;
	}
	isFile(): boolean {
		return this[kType] === UV_DIRENT_FILE;
	}
	isBlockDevice(): boolean {
		return this[kType] === UV_DIRENT_BLOCK;
	}
	isCharacterDevice(): boolean {
		return this[kType] === UV_DIRENT_CHAR;
	}
	isSymbolicLink(): boolean {
		return this[kType] === UV_DIRENT_LINK;
	}
	isFIFO(): boolean {
		return this[kType] === UV_DIRENT_FIFO;
	}
	isSocket(): boolean {
		return this[kType] === UV_DIRENT_SOCKET;
	}
}

/** The dirent type of a mode (for lstat fallbacks of UV_DIRENT_UNKNOWN). */
export function direntTypeFromMode(mode: number): number {
	switch (mode & S_IFMT) {
		case S_IFREG:
			return UV_DIRENT_FILE;
		case S_IFDIR:
			return UV_DIRENT_DIR;
		case S_IFLNK:
			return UV_DIRENT_LINK;
		case S_IFIFO:
			return UV_DIRENT_FIFO;
		case S_IFSOCK:
			return UV_DIRENT_SOCKET;
		case S_IFCHR:
			return UV_DIRENT_CHAR;
		case S_IFBLK:
			return UV_DIRENT_BLOCK;
	}
	return UV_DIRENT_UNKNOWN;
}

export { UV_DIRENT_UNKNOWN, UV_DIRENT_DIR, UV_DIRENT_LINK, UV_DIRENT_FILE };

export function validateOffsetLengthRead(offset: number, length: number, bufferLength: number): void {
	if (offset < 0) throw outOfRange("offset", ">= 0", offset);
	if (length < 0) throw outOfRange("length", ">= 0", length);
	if (offset + length > bufferLength) throw outOfRange("length", `<= ${bufferLength - offset}`, length);
}

export function validatePosition(position: unknown, name = "position"): void {
	if (typeof position === "number") {
		if (!Number.isInteger(position)) throw outOfRange(name, "an integer", position);
		return;
	}
	if (typeof position === "bigint") {
		if (position < -(2n ** 63n) || position > 2n ** 63n - 1n)
			throw outOfRange(name, ">= -2 ** 63 && <= 2 ** 63 - 1", position);
		return;
	}
	if (position == null) return;
	throw invalidArgType(name, "of type number or bigint", position);
}

export { O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_EXCL, O_TRUNC, O_APPEND };
