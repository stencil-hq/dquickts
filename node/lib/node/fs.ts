// node:fs — the Node file system API (sync, callback and promises) over the
// qbun:fs natives (libuv uv_fs_*).

import { file } from "internal:bun/file";
import * as binding from "qbun:fs";
import * as pathMod from "node:path";
import { Readable, Writable } from "node:stream";
import { bind } from "internal:async-context";
import { codedError, invalidArgType, outOfRange, validateFunction } from "internal:errors";
import {
	type BaseOptions,
	type BufferEncodingOpt,
	type NativePath,
	type PathLike,
	type StatsArray,
	BigIntStats,
	Dirent,
	StatFs,
	Stats,
	constants,
	encodeBuffer,
	encodePath,
	getOptions,
	getStatFsFromBinding,
	getStatsFromBinding,
	getValidatedFd,
	getValidatedPath,
	parseFileMode,
	pathString,
	stringToFlags,
	toBytes,
	toUnixTimestamp,
	validateInteger,
	validatePosition,
	abortErr,
} from "internal:fs/utils";
import {
	call,
	sync,
	cpImpl,
	cpSyncImpl,
	mkdirp,
	mkdirpSync,
	readdirImpl,
	readdirSyncImpl,
	rmImpl,
	rmSyncImpl,
	type CpOpts,
	type ReaddirOpts,
	type RmOpts,
} from "internal:fs/ops";
import { Dir, opendirImpl, opendirSyncImpl, type OpendirOptions } from "internal:fs/dir";
import {
	FSWatcher,
	StatWatcher,
	watchImpl,
	watchFileImpl,
	unwatchFileImpl,
	type WatchOptions,
} from "internal:fs/watchers";
import { globImpl, globSyncImpl, type NodeGlobOptions } from "internal:fs/node-glob";
import promises, { FileHandle } from "node:fs/promises";

export { constants, Stats, BigIntStats, StatFs, Dirent, Dir, FSWatcher, StatWatcher, promises };

export const F_OK = constants.F_OK;
export const R_OK = constants.R_OK;
export const W_OK = constants.W_OK;
export const X_OK = constants.X_OK;

type Cb<T = void> = (err: Error | null, result?: T) => void;
const lchmodNative = binding.lchmod as (p: NativePath, mode: number) => void;
const existsNative = binding.existsSync as (p: unknown) => boolean;

function maybeCallback<T>(cb: unknown): Cb<T> {
	validateFunction(cb, "cb");
	return bind(cb as Cb<T>);
}

/** Split (options?, callback) trailing arguments. */
function optsAndCb<O>(options: O | Cb<never> | undefined, callback: unknown): [O | undefined, unknown] {
	return typeof options === "function" ? [undefined, options] : [options as O | undefined, callback];
}

/** Run a promise-returning operation with a Node callback. */
function viaPromise<T>(cb: Cb<T>, fn: () => Promise<T>): void {
	fn().then(
		(r) => cb(null, r),
		(e: Error) => cb(e),
	);
}

/** Run native `name` with a mapped callback result. */
function nativeCb<R, T = R>(cb: Cb<T>, name: string, args: unknown[], map?: (r: R) => T): void {
	(binding as unknown as Record<string, (...a: unknown[]) => void>)[name](...args, (err: Error | null, res: R) => {
		if (err) cb(err);
		else cb(null, map ? map(res) : (res as unknown as T));
	});
}

// ---------------------------------------------------------------- access / exists

export function accessSync(path: PathLike, mode: number = F_OK): void {
	sync("access", getValidatedPath(path), mode);
}

export function access(path: PathLike, mode: number | Cb, callback?: Cb): void {
	if (typeof mode === "function") {
		callback = mode;
		mode = F_OK;
	}
	const cb = maybeCallback<void>(callback);
	nativeCb(cb, "access", [getValidatedPath(path), mode]);
}

export function existsSync(path: PathLike): boolean {
	try {
		return existsNative(getValidatedPath(path));
	} catch {
		return false;
	}
}

export function exists(path: PathLike, callback: (exists: boolean) => void): void {
	validateFunction(callback, "cb");
	const cb = bind(callback);
	let p: NativePath;
	try {
		p = getValidatedPath(path);
	} catch {
		process.nextTick(cb, false);
		return;
	}
	(binding.access as (p: NativePath, m: number, cb: (e: Error | null) => void) => void)(p, F_OK, (err) => cb(!err));
}
Object.defineProperty(exists, Symbol.for("nodejs.util.promisify.custom"), {
	value: (path: PathLike) => Promise.resolve(existsSync(path)),
});

// ---------------------------------------------------------------- open / close / read / write

export function openSync(path: PathLike, flags?: string | number, mode?: number | string): number {
	return sync("open", getValidatedPath(path), stringToFlags(flags ?? "r"), parseFileMode(mode, "mode", 0o666));
}

export function open(
	path: PathLike,
	flags?: string | number | Cb<number>,
	mode?: number | string | Cb<number>,
	callback?: Cb<number>,
): void {
	if (typeof flags === "function") {
		callback = flags;
		flags = "r";
		mode = 0o666;
	} else if (typeof mode === "function") {
		callback = mode;
		mode = 0o666;
	}
	const cb = maybeCallback<number>(callback);
	nativeCb(cb, "open", [getValidatedPath(path), stringToFlags(flags ?? "r"), parseFileMode(mode, "mode", 0o666)]);
}

export function openAsBlob(path: PathLike, options?: { type?: string }): Promise<Blob> {
	const p = pathString(path);
	return Promise.resolve(file(p, options?.type ? { type: options.type } : undefined));
}

export function closeSync(fd: number): void {
	sync("close", getValidatedFd(fd));
}

export function close(fd: number, callback?: Cb): void {
	const f = getValidatedFd(fd);
	const cb = callback === undefined ? () => {} : maybeCallback<void>(callback);
	nativeCb(cb, "close", [f]);
}

interface RWOptions {
	buffer?: ArrayBufferView;
	offset?: number;
	length?: number;
	position?: number | bigint | null;
}

function rwArgs(
	buffer: ArrayBufferView,
	offsetOrOptions: number | RWOptions | null | undefined,
	length: number | undefined | null,
	position: number | bigint | null | undefined,
	isRead: boolean,
): [number, number, number | bigint] {
	let offset: number | undefined;
	if (offsetOrOptions !== null && typeof offsetOrOptions === "object") {
		offset = offsetOrOptions.offset;
		length = offsetOrOptions.length;
		position = offsetOrOptions.position;
	} else offset = offsetOrOptions ?? undefined;
	offset ??= 0;
	validateInteger(offset, "offset", 0);
	length ??= buffer.byteLength - offset;
	validateInteger(length, "length", 0);
	if (offset + length > buffer.byteLength) {
		throw outOfRange(isRead ? "length" : "length", `<= ${buffer.byteLength - offset}`, length);
	}
	validatePosition(position);
	return [offset, length, position == null ? -1 : position];
}

export function readSync(
	fd: number,
	buffer: ArrayBufferView,
	offset?: number | RWOptions | null,
	length?: number | null,
	position?: number | bigint | null,
): number {
	getValidatedFd(fd);
	if (!ArrayBuffer.isView(buffer))
		throw invalidArgType("buffer", "an instance of Buffer, TypedArray, or DataView", buffer);
	const [off, len, pos] = rwArgs(buffer, offset, length, position, true);
	if (len === 0) return 0;
	return sync("read", fd, buffer, off, len, pos);
}

export function read(fd: number, ...args: unknown[]): void {
	getValidatedFd(fd);
	const callback = args.pop();
	const cb = maybeCallback<number>(callback);
	let buffer: ArrayBufferView;
	let rest: unknown[] = args;
	if (ArrayBuffer.isView(args[0])) {
		buffer = args[0];
		rest = args.slice(1);
	} else {
		const o = (args[0] ?? {}) as RWOptions;
		buffer = o.buffer ?? Buffer.alloc(16384);
		rest = [o];
	}
	const [off, len, pos] = rwArgs(buffer, rest[0] as number | RWOptions, rest[1] as number, rest[2] as number, true);
	if (len === 0) {
		process.nextTick(() => (cb as unknown as (e: null, n: number, b: ArrayBufferView) => void)(null, 0, buffer));
		return;
	}
	(binding.read as (...a: unknown[]) => void)(fd, buffer, off, len, pos, (err: Error | null, n: number) => {
		(cb as unknown as (e: Error | null, n?: number, b?: ArrayBufferView) => void)(err, n ?? 0, buffer);
	});
}

export function readvSync(fd: number, buffers: ArrayBufferView[], position?: number | null): number {
	getValidatedFd(fd);
	return sync("readv", fd, buffers, position ?? -1);
}

export function readv(
	fd: number,
	buffers: ArrayBufferView[],
	position: number | null | Cb<number>,
	callback?: Cb<number>,
): void {
	if (typeof position === "function") {
		callback = position;
		position = null;
	}
	getValidatedFd(fd);
	const cb = maybeCallback<number>(callback) as unknown as (e: Error | null, n?: number, b?: ArrayBufferView[]) => void;
	(binding.readv as (...a: unknown[]) => void)(fd, buffers, position ?? -1, (err: Error | null, n: number) =>
		cb(err, n ?? 0, buffers),
	);
}

export function writeSync(
	fd: number,
	buffer: ArrayBufferView | string,
	offsetOrOptions?: number | RWOptions | null,
	length?: number | BufferEncoding | null,
	position?: number | null,
): number {
	getValidatedFd(fd);
	if (typeof buffer === "string") {
		// writeSync(fd, string, position?, encoding?)
		const enc = (typeof length === "string" ? length : "utf8") as BufferEncoding;
		const bytes = Buffer.from(buffer, enc);
		const pos = typeof offsetOrOptions === "number" ? offsetOrOptions : -1;
		return sync("write", fd, bytes, 0, bytes.length, pos);
	}
	if (!ArrayBuffer.isView(buffer))
		throw invalidArgType("buffer", "of type string or an instance of Buffer, TypedArray, or DataView", buffer);
	const [off, len, pos] = rwArgs(buffer, offsetOrOptions, length as number | null | undefined, position, false);
	return sync("write", fd, buffer, off, len, pos);
}

export function write(fd: number, buffer: ArrayBufferView | string, ...args: unknown[]): void {
	getValidatedFd(fd);
	const callback = args.pop();
	const cb = maybeCallback<number>(callback) as unknown as (e: Error | null, n?: number, b?: unknown) => void;
	let bytes: ArrayBufferView;
	let off: number;
	let len: number;
	let pos: number | bigint;
	if (typeof buffer === "string") {
		const enc = (typeof args[1] === "string" ? args[1] : "utf8") as BufferEncoding;
		bytes = Buffer.from(buffer, enc);
		off = 0;
		len = bytes.byteLength;
		pos = typeof args[0] === "number" ? args[0] : -1;
	} else {
		if (!ArrayBuffer.isView(buffer))
			throw invalidArgType("buffer", "of type string or an instance of Buffer, TypedArray, or DataView", buffer);
		bytes = buffer;
		[off, len, pos] = rwArgs(buffer, args[0] as number | RWOptions, args[1] as number, args[2] as number, false);
	}
	(binding.write as (...a: unknown[]) => void)(fd, bytes, off, len, pos, (err: Error | null, n: number) =>
		cb(err, n ?? 0, buffer),
	);
}

export function writevSync(fd: number, buffers: ArrayBufferView[], position?: number | null): number {
	getValidatedFd(fd);
	return sync("writev", fd, buffers, position ?? -1);
}

export function writev(
	fd: number,
	buffers: ArrayBufferView[],
	position: number | null | Cb<number>,
	callback?: Cb<number>,
): void {
	if (typeof position === "function") {
		callback = position;
		position = null;
	}
	getValidatedFd(fd);
	const cb = maybeCallback<number>(callback) as unknown as (e: Error | null, n?: number, b?: ArrayBufferView[]) => void;
	(binding.writev as (...a: unknown[]) => void)(fd, buffers, position ?? -1, (err: Error | null, n: number) =>
		cb(err, n ?? 0, buffers),
	);
}

// ---------------------------------------------------------------- readFile / writeFile / appendFile

type ReadFileOpts = { encoding?: BufferEncodingOpt; flag?: string | number; signal?: AbortSignal };

export function readFileSync(path: PathLike | number, options?: ReadFileOpts | BufferEncoding | null): string | Buffer {
	const opts = getOptions(options, { encoding: null as BufferEncodingOpt, flag: "r" as string | number });
	const target = typeof path === "number" ? getValidatedFd(path) : getValidatedPath(path);
	const buf = sync<Buffer>("readFile", target, stringToFlags(opts.flag));
	return encodeBuffer(buf, opts.encoding);
}

export function readFile(
	path: PathLike | number,
	options: ReadFileOpts | BufferEncoding | null | Cb<string | Buffer>,
	callback?: Cb<string | Buffer>,
): void {
	const [o, c] = optsAndCb(options, callback);
	const cb = maybeCallback<string | Buffer>(c);
	const opts = getOptions(o, { encoding: null as BufferEncodingOpt, flag: "r" as string | number });
	const target = typeof path === "number" ? getValidatedFd(path) : getValidatedPath(path);
	if (opts.signal?.aborted) {
		process.nextTick(cb, abortErr(opts.signal));
		return;
	}
	nativeCb<Buffer, string | Buffer>(
		(err, res) => {
			if (!err && opts.signal?.aborted) cb(abortErr(opts.signal));
			else cb(err, res);
		},
		"readFile",
		[target, stringToFlags(opts.flag)],
		(buf) => encodeBuffer(buf, opts.encoding),
	);
}

function writeFileOpts(options: unknown, flag: string): BaseOptions {
	return getOptions(options, { encoding: "utf8" as BufferEncodingOpt, mode: 0o666, flag });
}

function dataBytes(data: unknown, encoding: BufferEncodingOpt): Uint8Array {
	if (typeof data === "string" || ArrayBuffer.isView(data)) return toBytes(data, encoding);
	throw invalidArgType("data", "of type string or an instance of Buffer, TypedArray, or DataView", data);
}

export function writeFileSync(
	path: PathLike | number,
	data: string | ArrayBufferView,
	options?: BaseOptions | BufferEncoding | null,
): void {
	const opts = writeFileOpts(options, "w");
	const target = typeof path === "number" ? getValidatedFd(path) : getValidatedPath(path);
	sync(
		"writeFile",
		target,
		dataBytes(data, opts.encoding),
		stringToFlags(opts.flag),
		parseFileMode(opts.mode, "mode", 0o666),
	);
}

export function writeFile(
	path: PathLike | number,
	data: unknown,
	options: BaseOptions | BufferEncoding | null | Cb,
	callback?: Cb,
): void {
	const [o, c] = optsAndCb(options, callback);
	const cb = maybeCallback<void>(c);
	const opts = writeFileOpts(o, "w");
	if (typeof path === "number") {
		const fd = getValidatedFd(path);
		const bytes = dataBytes(data, opts.encoding);
		nativeCb(cb, "writeFile", [fd, bytes, 0, 0]);
		return;
	}
	if (opts.signal || !(typeof data === "string" || ArrayBuffer.isView(data))) {
		viaPromise(cb, () => promises.writeFile(path, data, opts));
		return;
	}
	nativeCb(cb, "writeFile", [
		getValidatedPath(path),
		dataBytes(data, opts.encoding),
		stringToFlags(opts.flag),
		parseFileMode(opts.mode, "mode", 0o666),
	]);
}

export function appendFileSync(
	path: PathLike | number,
	data: string | ArrayBufferView,
	options?: BaseOptions | BufferEncoding | null,
): void {
	const opts = writeFileOpts(options, "a");
	writeFileSync(path, data, opts);
}

export function appendFile(
	path: PathLike | number,
	data: string | ArrayBufferView,
	options: BaseOptions | BufferEncoding | null | Cb,
	callback?: Cb,
): void {
	const [o, c] = optsAndCb(options, callback);
	writeFile(path, data, writeFileOpts(o, "a"), c as Cb);
}

// ---------------------------------------------------------------- stat family

type StatOpts = { bigint?: boolean; throwIfNoEntry?: boolean };

function statSyncImpl(name: "stat" | "lstat", path: PathLike, options?: StatOpts): Stats | BigIntStats | undefined {
	const bigint = !!options?.bigint;
	try {
		return getStatsFromBinding(sync<StatsArray>(name, getValidatedPath(path), bigint), bigint);
	} catch (e) {
		const code = (e as { code?: string }).code;
		if (options?.throwIfNoEntry === false && (code === "ENOENT" || code === "ENOTDIR")) return undefined;
		throw e;
	}
}

export function statSync(path: PathLike, options?: StatOpts): Stats | BigIntStats | undefined {
	return statSyncImpl("stat", path, options);
}

export function lstatSync(path: PathLike, options?: StatOpts): Stats | BigIntStats | undefined {
	return statSyncImpl("lstat", path, options);
}

export function fstatSync(fd: number, options?: { bigint?: boolean }): Stats | BigIntStats {
	const bigint = !!options?.bigint;
	return getStatsFromBinding(sync<StatsArray>("fstat", getValidatedFd(fd), bigint), bigint);
}

export function statfsSync(path: PathLike, options?: { bigint?: boolean }): StatFs<number | bigint> {
	return getStatFsFromBinding(sync<(number | bigint)[]>("statfs", getValidatedPath(path), !!options?.bigint));
}

function statCb(name: string, target: NativePath | number, options: unknown, callback: unknown): void {
	const [o, c] = optsAndCb(options as { bigint?: boolean } | undefined, callback);
	const cb = maybeCallback<Stats | BigIntStats>(c);
	const bigint = !!o?.bigint;
	nativeCb<StatsArray, Stats | BigIntStats>(cb, name, [target, bigint], (r) => getStatsFromBinding(r, bigint));
}

export function stat(path: PathLike, options?: { bigint?: boolean } | Cb<Stats>, callback?: Cb<Stats>): void {
	statCb("stat", getValidatedPath(path), options, callback);
}

export function lstat(path: PathLike, options?: { bigint?: boolean } | Cb<Stats>, callback?: Cb<Stats>): void {
	statCb("lstat", getValidatedPath(path), options, callback);
}

export function fstat(fd: number, options?: { bigint?: boolean } | Cb<Stats>, callback?: Cb<Stats>): void {
	statCb("fstat", getValidatedFd(fd), options, callback);
}

export function statfs(path: PathLike, options?: { bigint?: boolean } | Cb<StatFs>, callback?: Cb<StatFs>): void {
	const [o, c] = optsAndCb(options as { bigint?: boolean } | undefined, callback);
	const cb = maybeCallback<StatFs<number | bigint>>(c);
	nativeCb<(number | bigint)[], StatFs<number | bigint>>(
		cb,
		"statfs",
		[getValidatedPath(path), !!o?.bigint],
		getStatFsFromBinding,
	);
}

// ---------------------------------------------------------------- directories

type MkdirOpts = { recursive?: boolean; mode?: number | string };

function mkdirOpts(options: unknown): { recursive: boolean; mode: number } {
	const o = (typeof options === "object" && options !== null ? options : { mode: options }) as MkdirOpts;
	if (o.recursive !== undefined && typeof o.recursive !== "boolean")
		throw invalidArgType("options.recursive", "of type boolean", o.recursive);
	return { recursive: !!o.recursive, mode: parseFileMode(o.mode, "mode", 0o777) };
}

export function mkdirSync(path: PathLike, options?: MkdirOpts | number | string): string | undefined {
	const { recursive, mode } = mkdirOpts(options);
	if (recursive) return mkdirpSync(pathMod.resolve(pathString(path)), mode);
	sync("mkdir", getValidatedPath(path), mode);
	return undefined;
}

export function mkdir(
	path: PathLike,
	options?: MkdirOpts | number | string | Cb<string | undefined>,
	callback?: Cb<string | undefined>,
): void {
	const [o, c] = optsAndCb(options, callback);
	const cb = maybeCallback<string | undefined>(c);
	const { recursive, mode } = mkdirOpts(o);
	if (recursive) {
		const p = pathMod.resolve(pathString(path));
		viaPromise(cb, () => mkdirp(p, mode));
		return;
	}
	nativeCb(cb, "mkdir", [getValidatedPath(path), mode], () => undefined);
}

export function mkdtempSync(
	prefix: string | Buffer | URL,
	options?: { encoding?: BufferEncodingOpt } | BufferEncoding,
): string | Buffer {
	const opts = getOptions(options, { encoding: "utf8" as BufferEncodingOpt });
	return encodePath(sync<string>("mkdtemp", pathString(prefix, "prefix")), opts.encoding);
}

export function mkdtemp(
	prefix: string | Buffer | URL,
	options: { encoding?: BufferEncodingOpt } | BufferEncoding | Cb<string | Buffer>,
	callback?: Cb<string | Buffer>,
): void {
	const [o, c] = optsAndCb(options, callback);
	const cb = maybeCallback<string | Buffer>(c);
	const opts = getOptions(o, { encoding: "utf8" as BufferEncodingOpt });
	nativeCb<string, string | Buffer>(cb, "mkdtemp", [pathString(prefix, "prefix")], (p) => encodePath(p, opts.encoding));
}

export function mkdtempDisposableSync(
	prefix: string | Buffer | URL,
	options?: { encoding?: BufferEncodingOpt },
): { path: string | Buffer; remove(): void; [Symbol.dispose](): void } {
	const path = mkdtempSync(prefix, options);
	const p = typeof path === "string" ? path : path.toString();
	const remove = () => rmSync(p, { recursive: true, force: true });
	return { path, remove, [Symbol.dispose]: remove };
}

function readdirOpts(options: unknown): ReaddirOpts {
	return getOptions(options, {
		encoding: "utf8" as BufferEncodingOpt,
		withFileTypes: false,
		recursive: false,
	}) as ReaddirOpts;
}

export function readdirSync(
	path: PathLike,
	options?: ReaddirOpts | BufferEncoding | null,
): (string | Buffer | Dirent)[] {
	return readdirSyncImpl(getValidatedPath(path), readdirOpts(options));
}

export function readdir(
	path: PathLike,
	options: ReaddirOpts | BufferEncoding | null | Cb<(string | Buffer | Dirent)[]>,
	callback?: Cb<(string | Buffer | Dirent)[]>,
): void {
	const [o, c] = optsAndCb(options, callback);
	const cb = maybeCallback<(string | Buffer | Dirent)[]>(c);
	const p = getValidatedPath(path);
	const opts = readdirOpts(o);
	viaPromise(cb, () => readdirImpl(p, opts));
}

function opendirOpts(options: unknown): OpendirOptions {
	const opts = getOptions(options, {
		encoding: "utf8" as BufferEncodingOpt,
		bufferSize: 32,
		recursive: false,
	}) as OpendirOptions;
	validateInteger(opts.bufferSize, "options.bufferSize", 1, 4294967295);
	return opts;
}

export function opendirSync(path: PathLike, options?: OpendirOptions): Dir {
	return opendirSyncImpl(getValidatedPath(path), pathString(path), opendirOpts(options));
}

export function opendir(path: PathLike, options: OpendirOptions | Cb<Dir>, callback?: Cb<Dir>): void {
	const [o, c] = optsAndCb(options, callback);
	const cb = maybeCallback<Dir>(c);
	const opts = opendirOpts(o);
	const p = getValidatedPath(path);
	viaPromise(cb, () => opendirImpl(p, pathString(path), opts));
}

export function rmdirSync(path: PathLike, options?: RmOpts): void {
	if (options?.recursive) {
		rmSyncImpl(pathString(path), { ...options, recursive: true, force: false });
		return;
	}
	sync("rmdir", getValidatedPath(path));
}

export function rmdir(path: PathLike, options?: RmOpts | Cb, callback?: Cb): void {
	const [o, c] = optsAndCb(options, callback);
	const cb = maybeCallback<void>(c);
	if (o?.recursive) {
		const p = pathString(path);
		viaPromise(cb, () => rmImpl(p, { ...o, recursive: true, force: false }));
		return;
	}
	nativeCb(cb, "rmdir", [getValidatedPath(path)]);
}

function validateRmOpts(options: unknown): RmOpts {
	if (options == null) return {};
	if (typeof options !== "object") throw invalidArgType("options", "of type object", options);
	const o = options as RmOpts;
	if (o.maxRetries !== undefined) validateInteger(o.maxRetries, "options.maxRetries", 0);
	if (o.retryDelay !== undefined) validateInteger(o.retryDelay, "options.retryDelay", 0);
	return o;
}

export function rmSync(path: PathLike, options?: RmOpts): void {
	rmSyncImpl(pathString(path), validateRmOpts(options));
}

export function rm(path: PathLike, options?: RmOpts | Cb, callback?: Cb): void {
	const [o, c] = optsAndCb(options, callback);
	const cb = maybeCallback<void>(c);
	const p = pathString(path);
	const opts = validateRmOpts(o);
	viaPromise(cb, () => rmImpl(p, opts));
}

// ---------------------------------------------------------------- links, rename, copy

export function unlinkSync(path: PathLike): void {
	sync("unlink", getValidatedPath(path));
}

export function unlink(path: PathLike, callback: Cb): void {
	nativeCb(maybeCallback<void>(callback), "unlink", [getValidatedPath(path)]);
}

export function renameSync(oldPath: PathLike, newPath: PathLike): void {
	sync("rename", getValidatedPath(oldPath, "oldPath"), getValidatedPath(newPath, "newPath"));
}

export function rename(oldPath: PathLike, newPath: PathLike, callback: Cb): void {
	nativeCb(maybeCallback<void>(callback), "rename", [
		getValidatedPath(oldPath, "oldPath"),
		getValidatedPath(newPath, "newPath"),
	]);
}

export function linkSync(existingPath: PathLike, newPath: PathLike): void {
	sync("link", getValidatedPath(existingPath, "existingPath"), getValidatedPath(newPath, "newPath"));
}

export function link(existingPath: PathLike, newPath: PathLike, callback: Cb): void {
	nativeCb(maybeCallback<void>(callback), "link", [
		getValidatedPath(existingPath, "existingPath"),
		getValidatedPath(newPath, "newPath"),
	]);
}

function symlinkFlags(type: unknown): number {
	if (type == null || type === "file") return 0;
	if (type === "dir") return constants.UV_FS_SYMLINK_DIR;
	if (type === "junction") return constants.UV_FS_SYMLINK_JUNCTION;
	throw codedError(
		"ERR_FS_INVALID_SYMLINK_TYPE",
		`Symlink type must be one of "dir", "file", or "junction". Received "${String(type)}"`,
	);
}

export function symlinkSync(target: PathLike, path: PathLike, type?: string | null): void {
	sync("symlink", getValidatedPath(target, "target"), getValidatedPath(path), symlinkFlags(type));
}

export function symlink(target: PathLike, path: PathLike, type: string | null | undefined | Cb, callback?: Cb): void {
	const [t, c] = optsAndCb(type, callback);
	nativeCb(maybeCallback<void>(c), "symlink", [
		getValidatedPath(target, "target"),
		getValidatedPath(path),
		symlinkFlags(t),
	]);
}

type EncOpts = { encoding?: BufferEncodingOpt } | BufferEncoding | null;

export function readlinkSync(path: PathLike, options?: EncOpts): string | Buffer {
	const opts = getOptions(options, { encoding: "utf8" as BufferEncodingOpt });
	return encodePath(sync<string>("readlink", getValidatedPath(path)), opts.encoding);
}

export function readlink(path: PathLike, options: EncOpts | Cb<string | Buffer>, callback?: Cb<string | Buffer>): void {
	const [o, c] = optsAndCb(options, callback);
	const cb = maybeCallback<string | Buffer>(c);
	const opts = getOptions(o, { encoding: "utf8" as BufferEncodingOpt });
	nativeCb<string, string | Buffer>(cb, "readlink", [getValidatedPath(path)], (p) => encodePath(p, opts.encoding));
}

export function realpathSync(path: PathLike, options?: EncOpts): string | Buffer {
	const opts = getOptions(options, { encoding: "utf8" as BufferEncodingOpt });
	return encodePath(sync<string>("realpath", getValidatedPath(path)), opts.encoding);
}
realpathSync.native = realpathSync;

export function realpath(path: PathLike, options: EncOpts | Cb<string | Buffer>, callback?: Cb<string | Buffer>): void {
	const [o, c] = optsAndCb(options, callback);
	const cb = maybeCallback<string | Buffer>(c);
	const opts = getOptions(o, { encoding: "utf8" as BufferEncodingOpt });
	nativeCb<string, string | Buffer>(cb, "realpath", [getValidatedPath(path)], (p) => encodePath(p, opts.encoding));
}
realpath.native = realpath;

export function copyFileSync(src: PathLike, dest: PathLike, mode = 0): void {
	sync("copyfile", getValidatedPath(src, "src"), getValidatedPath(dest, "dest"), mode);
}

export function copyFile(src: PathLike, dest: PathLike, mode: number | Cb, callback?: Cb): void {
	const [m, c] = optsAndCb(mode, callback);
	nativeCb(maybeCallback<void>(c), "copyfile", [getValidatedPath(src, "src"), getValidatedPath(dest, "dest"), m ?? 0]);
}

function cpOpts(options: unknown): CpOpts {
	if (options == null) return {};
	if (typeof options !== "object") throw invalidArgType("options", "of type object", options);
	const o = options as CpOpts;
	if (o.filter !== undefined) validateFunction(o.filter, "options.filter");
	if (o.dereference && o.verbatimSymlinks) {
		throw codedError(
			"ERR_INCOMPATIBLE_OPTION_PAIR",
			'Option "dereference" cannot be used in combination with option "verbatimSymlinks"',
			TypeError,
		);
	}
	return { force: true, ...o };
}

export function cpSync(src: PathLike, dest: PathLike, options?: CpOpts): void {
	cpSyncImpl(pathMod.resolve(pathString(src, "src")), pathMod.resolve(pathString(dest, "dest")), cpOpts(options));
}

export function cp(src: PathLike, dest: PathLike, options: CpOpts | Cb, callback?: Cb): void {
	const [o, c] = optsAndCb(options, callback);
	const cb = maybeCallback<void>(c);
	const s = pathMod.resolve(pathString(src, "src"));
	const d = pathMod.resolve(pathString(dest, "dest"));
	const opts = cpOpts(o);
	viaPromise(cb, () => cpImpl(s, d, opts));
}

// ---------------------------------------------------------------- permissions, times, sizes

function validId(v: unknown, name: string): number {
	validateInteger(v, name, -1, 0xffffffff);
	return v;
}

export function chmodSync(path: PathLike, mode: number | string): void {
	sync("chmod", getValidatedPath(path), parseFileMode(mode, "mode"));
}

export function chmod(path: PathLike, mode: number | string, callback: Cb): void {
	nativeCb(maybeCallback<void>(callback), "chmod", [getValidatedPath(path), parseFileMode(mode, "mode")]);
}

export function fchmodSync(fd: number, mode: number | string): void {
	sync("fchmod", getValidatedFd(fd), parseFileMode(mode, "mode"));
}

export function fchmod(fd: number, mode: number | string, callback: Cb): void {
	nativeCb(maybeCallback<void>(callback), "fchmod", [getValidatedFd(fd), parseFileMode(mode, "mode")]);
}

export function lchmodSync(path: PathLike, mode: number | string): void {
	lchmodNative(getValidatedPath(path), parseFileMode(mode, "mode"));
}

export function lchmod(path: PathLike, mode: number | string, callback: Cb): void {
	const cb = maybeCallback<void>(callback);
	const p = getValidatedPath(path);
	const m = parseFileMode(mode, "mode");
	process.nextTick(() => {
		try {
			lchmodNative(p, m);
		} catch (e) {
			cb(e as Error);
			return;
		}
		cb(null);
	});
}

export function chownSync(path: PathLike, uid: number, gid: number): void {
	sync("chown", getValidatedPath(path), validId(uid, "uid"), validId(gid, "gid"));
}

export function chown(path: PathLike, uid: number, gid: number, callback: Cb): void {
	nativeCb(maybeCallback<void>(callback), "chown", [getValidatedPath(path), validId(uid, "uid"), validId(gid, "gid")]);
}

export function lchownSync(path: PathLike, uid: number, gid: number): void {
	sync("lchown", getValidatedPath(path), validId(uid, "uid"), validId(gid, "gid"));
}

export function lchown(path: PathLike, uid: number, gid: number, callback: Cb): void {
	nativeCb(maybeCallback<void>(callback), "lchown", [getValidatedPath(path), validId(uid, "uid"), validId(gid, "gid")]);
}

export function fchownSync(fd: number, uid: number, gid: number): void {
	sync("fchown", getValidatedFd(fd), validId(uid, "uid"), validId(gid, "gid"));
}

export function fchown(fd: number, uid: number, gid: number, callback: Cb): void {
	nativeCb(maybeCallback<void>(callback), "fchown", [getValidatedFd(fd), validId(uid, "uid"), validId(gid, "gid")]);
}

type Time = number | string | Date;

export function utimesSync(path: PathLike, atime: Time, mtime: Time): void {
	sync("utimes", getValidatedPath(path), toUnixTimestamp(atime, "atime"), toUnixTimestamp(mtime, "mtime"));
}

export function utimes(path: PathLike, atime: Time, mtime: Time, callback: Cb): void {
	nativeCb(maybeCallback<void>(callback), "utimes", [
		getValidatedPath(path),
		toUnixTimestamp(atime, "atime"),
		toUnixTimestamp(mtime, "mtime"),
	]);
}

export function lutimesSync(path: PathLike, atime: Time, mtime: Time): void {
	sync("lutimes", getValidatedPath(path), toUnixTimestamp(atime, "atime"), toUnixTimestamp(mtime, "mtime"));
}

export function lutimes(path: PathLike, atime: Time, mtime: Time, callback: Cb): void {
	nativeCb(maybeCallback<void>(callback), "lutimes", [
		getValidatedPath(path),
		toUnixTimestamp(atime, "atime"),
		toUnixTimestamp(mtime, "mtime"),
	]);
}

export function futimesSync(fd: number, atime: Time, mtime: Time): void {
	sync("futimes", getValidatedFd(fd), toUnixTimestamp(atime, "atime"), toUnixTimestamp(mtime, "mtime"));
}

export function futimes(fd: number, atime: Time, mtime: Time, callback: Cb): void {
	nativeCb(maybeCallback<void>(callback), "futimes", [
		getValidatedFd(fd),
		toUnixTimestamp(atime, "atime"),
		toUnixTimestamp(mtime, "mtime"),
	]);
}

export function ftruncateSync(fd: number, len = 0): void {
	validateInteger(len, "len");
	sync("ftruncate", getValidatedFd(fd), Math.max(0, len));
}

export function ftruncate(fd: number, len: number | Cb = 0, callback?: Cb): void {
	const [l, c] = optsAndCb(len, callback);
	const n = l ?? 0;
	validateInteger(n, "len");
	nativeCb(maybeCallback<void>(c), "ftruncate", [getValidatedFd(fd), Math.max(0, n)]);
}

export function truncateSync(path: PathLike | number, len = 0): void {
	if (typeof path === "number") {
		ftruncateSync(path, len);
		return;
	}
	validateInteger(len, "len");
	const fd = openSync(path, "r+");
	try {
		ftruncateSync(fd, len);
	} finally {
		closeSync(fd);
	}
}

export function truncate(path: PathLike | number, len: number | Cb = 0, callback?: Cb): void {
	const [l, c] = optsAndCb(len, callback);
	if (typeof path === "number") {
		ftruncate(path, l ?? 0, c as Cb);
		return;
	}
	const cb = maybeCallback<void>(c);
	viaPromise(cb, () => promises.truncate(path, l ?? 0));
}

export function fsyncSync(fd: number): void {
	sync("fsync", getValidatedFd(fd));
}

export function fsync(fd: number, callback: Cb): void {
	nativeCb(maybeCallback<void>(callback), "fsync", [getValidatedFd(fd)]);
}

export function fdatasyncSync(fd: number): void {
	sync("fdatasync", getValidatedFd(fd));
}

export function fdatasync(fd: number, callback: Cb): void {
	nativeCb(maybeCallback<void>(callback), "fdatasync", [getValidatedFd(fd)]);
}

// ---------------------------------------------------------------- watch

export function watch(
	filename: PathLike,
	options?: WatchOptions | BufferEncoding | ((eventType: string, filename: string | Buffer | null) => void),
	listener?: (eventType: string, filename: string | Buffer | null) => void,
): FSWatcher {
	if (typeof options === "function") {
		listener = options;
		options = undefined;
	}
	const opts = getOptions(options, {
		persistent: true,
		recursive: false,
		encoding: "utf8" as BufferEncodingOpt,
	}) as WatchOptions;
	return watchImpl(getValidatedPath(filename, "filename"), opts, listener);
}

type StatListener = (curr: Stats | BigIntStats, prev: Stats | BigIntStats) => void;

export function watchFile(
	filename: PathLike,
	options: { persistent?: boolean; interval?: number; bigint?: boolean } | StatListener,
	listener?: StatListener,
): StatWatcher {
	if (typeof options === "function") {
		listener = options;
		options = {};
	}
	validateFunction(listener, "listener");
	return watchFileImpl(pathString(filename, "filename"), options ?? {}, listener as StatListener);
}

export function unwatchFile(filename: PathLike, listener?: StatListener): void {
	unwatchFileImpl(pathString(filename, "filename"), listener as ((...args: unknown[]) => void) | undefined);
}

// ---------------------------------------------------------------- glob

export function globSync(pattern: string | readonly string[], options?: NodeGlobOptions): (string | Dirent)[] {
	return globSyncImpl(pattern, options);
}

export function glob(
	pattern: string | readonly string[],
	options: NodeGlobOptions | Cb<(string | Dirent)[]>,
	callback?: Cb<(string | Dirent)[]>,
): void {
	const [o, c] = optsAndCb(options, callback);
	const cb = maybeCallback<(string | Dirent)[]>(c);
	viaPromise(cb, async () => {
		const out: (string | Dirent)[] = [];
		for await (const e of globImpl(pattern, o)) out.push(e);
		return out;
	});
}

// ---------------------------------------------------------------- streams

interface StreamOpts {
	flags?: string | number;
	encoding?: BufferEncoding | null;
	fd?: number | FileHandle | null;
	mode?: number;
	autoClose?: boolean;
	emitClose?: boolean;
	start?: number;
	end?: number;
	highWaterMark?: number;
	signal?: AbortSignal;
	flush?: boolean;
	fs?: Record<string, unknown> | null;
}

function streamOpts(options: unknown): StreamOpts {
	if (options == null) return {};
	if (typeof options === "string") return { encoding: options as BufferEncoding };
	if (typeof options !== "object")
		throw invalidArgType("options", "one of type string or an instance of Object", options);
	return options as StreamOpts;
}

type FdOps = {
	read(
		fd: number,
		buf: Buffer,
		off: number,
		len: number,
		pos: number,
		cb: (err: Error | null, n?: number) => void,
	): void;
	write(
		fd: number,
		buf: Uint8Array,
		off: number,
		len: number,
		pos: number,
		cb: (err: Error | null, n?: number) => void,
	): void;
	close(fd: number, cb: (err: Error | null) => void): void;
	open(path: NativePath, flags: number, mode: number, cb: (err: Error | null, fd?: number) => void): void;
};
const fdOps = binding as unknown as FdOps;

export class ReadStream extends Readable {
	path: string | Buffer | undefined;
	fd: number | null = null;
	flags: string | number;
	mode: number;
	start: number | undefined;
	end: number;
	pos: number | undefined;
	bytesRead = 0;
	autoClose: boolean;
	pending = true;
	#handle: FileHandle | null = null;

	constructor(path: PathLike | undefined | null, options?: StreamOpts | BufferEncoding) {
		const opts = streamOpts(options);
		super({
			highWaterMark: opts.highWaterMark ?? 64 * 1024,
			encoding: opts.encoding ?? null,
			emitClose: opts.emitClose ?? true,
			autoDestroy: opts.autoClose ?? true,
			signal: opts.signal,
		} as ConstructorParameters<typeof Readable>[0]);
		const fd = opts.fd;
		if (fd instanceof FileHandle) {
			this.#handle = fd;
			this.fd = fd.fd;
			this.pending = false;
		} else if (typeof fd === "number") {
			this.fd = getValidatedFd(fd);
			this.pending = false;
		} else {
			const p = getValidatedPath(path);
			this.path = typeof p === "string" ? p : Buffer.from(p);
		}
		this.flags = opts.flags ?? "r";
		this.mode = opts.mode ?? 0o666;
		this.start = opts.start;
		this.end = opts.end ?? Number.POSITIVE_INFINITY;
		this.autoClose = opts.autoClose ?? true;
		if (this.start !== undefined) {
			validateInteger(this.start, "start", 0);
			this.pos = this.start;
		}
		if (this.end !== Number.POSITIVE_INFINITY) {
			validateInteger(this.end, "end", 0);
			if (this.start !== undefined && this.start > this.end) {
				throw outOfRange("start", `<= "end" (here: ${this.end})`, this.start);
			}
		}
	}

	override _construct(callback: (err?: Error | null) => void): void {
		if (this.fd !== null) {
			callback();
			return;
		}
		fdOps.open(getValidatedPath(this.path), stringToFlags(this.flags), this.mode, (err, fd) => {
			if (err) {
				callback(err);
				return;
			}
			this.fd = fd as number;
			this.pending = false;
			callback();
			this.emit("open", this.fd);
			this.emit("ready");
		});
	}

	override _read(n: number): void {
		const toRead =
			this.end === Number.POSITIVE_INFINITY ? n : Math.min(n, this.end - ((this.start ?? 0) + this.bytesRead) + 1);
		if (toRead <= 0) {
			this.push(null);
			return;
		}
		const buf = Buffer.allocUnsafe(toRead);
		const pos = this.pos ?? -1;
		fdOps.read(this.fd as number, buf, 0, toRead, pos, (err, bytes) => {
			if (err) {
				this.destroy(err);
				return;
			}
			const got = bytes ?? 0;
			if (got === 0) {
				this.push(null);
				return;
			}
			this.bytesRead += got;
			if (this.pos !== undefined) this.pos += got;
			this.push(got === toRead ? buf : buf.subarray(0, got));
		});
	}

	override _destroy(err: Error | null, callback: (err?: Error | null) => void): void {
		if (this.fd === null || !this.autoClose) {
			callback(err);
			return;
		}
		const fd = this.fd;
		this.fd = null;
		if (this.#handle) {
			this.#handle.close().then(
				() => callback(err),
				(e: Error) => callback(err ?? e),
			);
			return;
		}
		fdOps.close(fd, (e) => callback(err ?? e));
	}

	close(cb?: (err?: Error | null) => void): void {
		if (typeof cb === "function") this.once("close", () => cb());
		this.destroy();
	}
}

export class WriteStream extends Writable {
	path: string | Buffer | undefined;
	fd: number | null = null;
	flags: string | number;
	mode: number;
	start: number | undefined;
	pos: number | undefined;
	bytesWritten = 0;
	autoClose: boolean;
	pending = true;
	#handle: FileHandle | null = null;
	#flush: boolean;

	constructor(path: PathLike | undefined | null, options?: StreamOpts | BufferEncoding) {
		const opts = streamOpts(options);
		super({
			highWaterMark: opts.highWaterMark,
			decodeStrings: true,
			defaultEncoding: opts.encoding ?? "utf8",
			emitClose: opts.emitClose ?? true,
			autoDestroy: opts.autoClose ?? true,
			signal: opts.signal,
		} as ConstructorParameters<typeof Writable>[0]);
		const fd = opts.fd;
		if (fd instanceof FileHandle) {
			this.#handle = fd;
			this.fd = fd.fd;
			this.pending = false;
		} else if (typeof fd === "number") {
			this.fd = getValidatedFd(fd);
			this.pending = false;
		} else {
			const p = getValidatedPath(path);
			this.path = typeof p === "string" ? p : Buffer.from(p);
		}
		this.flags = opts.flags ?? "w";
		this.mode = opts.mode ?? 0o666;
		this.start = opts.start;
		this.autoClose = opts.autoClose ?? true;
		this.#flush = !!opts.flush;
		if (this.start !== undefined) {
			validateInteger(this.start, "start", 0);
			this.pos = this.start;
		}
		if (opts.encoding) this.setDefaultEncoding(opts.encoding);
	}

	override _construct(callback: (err?: Error | null) => void): void {
		if (this.fd !== null) {
			callback();
			return;
		}
		fdOps.open(getValidatedPath(this.path), stringToFlags(this.flags), this.mode, (err, fd) => {
			if (err) {
				callback(err);
				return;
			}
			this.fd = fd as number;
			this.pending = false;
			callback();
			this.emit("open", this.fd);
			this.emit("ready");
		});
	}

	#writeAll(buf: Uint8Array, cb: (err?: Error | null) => void): void {
		let off = 0;
		const step = () => {
			fdOps.write(this.fd as number, buf, off, buf.length - off, this.pos ?? -1, (err, n) => {
				if (err) {
					cb(err);
					return;
				}
				const w = n ?? 0;
				off += w;
				this.bytesWritten += w;
				if (this.pos !== undefined) this.pos += w;
				if (off < buf.length) step();
				else cb();
			});
		};
		if (buf.length === 0) cb();
		else step();
	}

	override _write(chunk: Buffer | string, encoding: BufferEncoding, callback: (err?: Error | null) => void): void {
		const buf = typeof chunk === "string" ? Buffer.from(chunk, encoding) : chunk;
		this.#writeAll(buf, callback);
	}

	override _writev(
		chunks: { chunk: Buffer | string; encoding: BufferEncoding }[],
		callback: (err?: Error | null) => void,
	): void {
		const bufs = chunks.map((c) => (typeof c.chunk === "string" ? Buffer.from(c.chunk, c.encoding) : c.chunk));
		this.#writeAll(Buffer.concat(bufs), callback);
	}

	override _final(callback: (err?: Error | null) => void): void {
		if (!this.#flush || this.fd === null) {
			callback();
			return;
		}
		(binding.fsync as (fd: number, cb: (e: Error | null) => void) => void)(this.fd, (e) => callback(e));
	}

	override _destroy(err: Error | null, callback: (err?: Error | null) => void): void {
		if (this.fd === null || !this.autoClose) {
			callback(err);
			return;
		}
		const fd = this.fd;
		this.fd = null;
		if (this.#handle) {
			this.#handle.close().then(
				() => callback(err),
				(e: Error) => callback(err ?? e),
			);
			return;
		}
		fdOps.close(fd, (e) => callback(err ?? e));
	}

	close(cb?: (err?: Error | null) => void): void {
		if (cb) {
			if (this.closed) {
				process.nextTick(cb);
				return;
			}
			this.on("close", cb);
		}
		if (!this.autoClose) this.on("finish", () => this.destroy());
		this.end();
	}

	destroySoon(): void {
		this.end();
	}
}

export function createReadStream(path: PathLike | null | undefined, options?: StreamOpts | BufferEncoding): ReadStream {
	return new ReadStream(path, options);
}

export function createWriteStream(
	path: PathLike | null | undefined,
	options?: StreamOpts | BufferEncoding,
): WriteStream {
	return new WriteStream(path, options);
}

// Node's legacy aliases
export const FileReadStream = ReadStream;
export const FileWriteStream = WriteStream;

const fs = {
	access,
	accessSync,
	appendFile,
	appendFileSync,
	chmod,
	chmodSync,
	chown,
	chownSync,
	close,
	closeSync,
	constants,
	copyFile,
	copyFileSync,
	cp,
	cpSync,
	createReadStream,
	createWriteStream,
	Dir,
	Dirent,
	exists,
	existsSync,
	fchmod,
	fchmodSync,
	fchown,
	fchownSync,
	fdatasync,
	fdatasyncSync,
	fstat,
	fstatSync,
	fsync,
	fsyncSync,
	ftruncate,
	ftruncateSync,
	futimes,
	futimesSync,
	glob,
	globSync,
	lchmod,
	lchmodSync,
	lchown,
	lchownSync,
	link,
	linkSync,
	lstat,
	lstatSync,
	lutimes,
	lutimesSync,
	mkdir,
	mkdirSync,
	mkdtemp,
	mkdtempSync,
	mkdtempDisposableSync,
	open,
	openAsBlob,
	openSync,
	opendir,
	opendirSync,
	read,
	readSync,
	readv,
	readvSync,
	readdir,
	readdirSync,
	readFile,
	readFileSync,
	readlink,
	readlinkSync,
	realpath,
	realpathSync,
	rename,
	renameSync,
	rm,
	rmSync,
	rmdir,
	rmdirSync,
	stat,
	statSync,
	statfs,
	statfsSync,
	symlink,
	symlinkSync,
	truncate,
	truncateSync,
	unlink,
	unlinkSync,
	unwatchFile,
	utimes,
	utimesSync,
	watch,
	watchFile,
	write,
	writeSync,
	writev,
	writevSync,
	writeFile,
	writeFileSync,
	Stats,
	BigIntStats,
	StatFs,
	StatWatcher,
	FSWatcher,
	ReadStream,
	WriteStream,
	FileReadStream,
	FileWriteStream,
	promises,
	F_OK,
	R_OK,
	W_OK,
	X_OK,
};

export default fs;
