// node:fs/promises — the promise API of node:fs and FileHandle.

import * as readline from "node:readline";
import type * as FsModule from "node:fs";
import type { ReadStream, WriteStream } from "node:fs";
import * as binding from "qbun:fs";
import * as pathMod from "node:path";
import { EventEmitter } from "node:events";
import { codedError, invalidArgType, outOfRange } from "internal:errors";
import {
	type BufferEncodingOpt,
	type NativePath,
	type StatsArray,
	type BaseOptions,
	type PathLike,
	BigIntStats,
	Dirent,
	Stats,
	StatFs,
	constants,
	encodeBuffer,
	encodePath,
	getOptions,
	getStatFsFromBinding,
	getStatsFromBinding,
	getValidatedPath,
	parseFileMode,
	pathString,
	stringToFlags,
	toBytes,
	toUnixTimestamp,
	validatePosition,
	checkAborted,
	abortErr,
	validateInteger,
} from "internal:fs/utils";
import { call, cpImpl, mkdirp, readdirImpl, rmImpl, type CpOpts, type RmOpts, type ReaddirOpts } from "internal:fs/ops";
import { Dir, opendirImpl, type OpendirOptions } from "internal:fs/dir";
import { watchAsync, type WatchOptions } from "internal:fs/watchers";
import { globImpl, type NodeGlobOptions } from "internal:fs/node-glob";

export { constants };

const kReadFileUnknownBufferLength = 64 * 1024;

interface ErrnoError extends Error {
	code?: string;
}

// ---------------------------------------------------------------- FileHandle

interface ReadResult<T> {
	bytesRead: number;
	buffer: T;
}
interface WriteResult<T> {
	bytesWritten: number;
	buffer: T;
}

interface ReadOptions<T extends ArrayBufferView> {
	buffer?: T;
	offset?: number;
	length?: number;
	position?: number | bigint | null;
}

function closedErr(syscall: string): Error {
	return Object.assign(new Error(`EBADF: bad file descriptor, ${syscall}`), { code: "EBADF", errno: -9, syscall });
}

export class FileHandle extends EventEmitter {
	#fd: number;
	#closing: Promise<void> | null = null;
	#refs = 1;
	#closeResolvers: PromiseWithResolvers<void> | null = null;

	constructor(fd: number) {
		super();
		this.#fd = fd;
	}

	get fd(): number {
		return this.#fd;
	}

	async #op<T>(syscall: string, fn: (fd: number) => Promise<T>): Promise<T> {
		if (this.#fd < 0 || this.#closing) throw closedErr(syscall);
		this.#refs++;
		try {
			return await fn(this.#fd);
		} finally {
			if (--this.#refs === 0 && this.#closeResolvers) this.#doClose();
		}
	}

	#doClose(): void {
		const r = this.#closeResolvers as PromiseWithResolvers<void>;
		const fd = this.#fd;
		this.#fd = -1;
		call("close", fd).then(
			() => {
				r.resolve();
				this.emit("close");
			},
			(e: Error) => r.reject(e),
		);
	}

	close(): Promise<void> {
		if (this.#closing) return this.#closing;
		if (this.#fd < 0) return Promise.resolve();
		this.#closeResolvers = Promise.withResolvers<void>();
		this.#closing = this.#closeResolvers.promise;
		if (--this.#refs === 0) this.#doClose();
		return this.#closing;
	}

	async [Symbol.asyncDispose](): Promise<void> {
		await this.close();
	}

	appendFile(data: string | ArrayBufferView, options?: BaseOptions | BufferEncoding): Promise<void> {
		const opts = getOptions(options, { encoding: "utf8" as BufferEncodingOpt });
		return this.#op("write", (fd) => writeAll(fd, toBytes(data, opts.encoding), null, opts.signal));
	}

	chmod(mode: number | string): Promise<void> {
		const m = parseFileMode(mode, "mode");
		return this.#op("fchmod", (fd) => call("fchmod", fd, m));
	}

	chown(uid: number, gid: number): Promise<void> {
		validateInteger(uid, "uid", -1, 0xffffffff);
		validateInteger(gid, "gid", -1, 0xffffffff);
		return this.#op("fchown", (fd) => call("fchown", fd, uid, gid));
	}

	datasync(): Promise<void> {
		return this.#op("fdatasync", (fd) => call("fdatasync", fd));
	}

	sync(): Promise<void> {
		return this.#op("fsync", (fd) => call("fsync", fd));
	}

	read<T extends ArrayBufferView = Buffer>(
		buffer?: T | ReadOptions<T>,
		offset?: number | ReadOptions<T>,
		length?: number,
		position?: number | bigint | null,
	): Promise<ReadResult<T>> {
		let buf: T;
		let off = 0;
		let len: number;
		let pos: number | bigint | null | undefined = position;
		if (buffer === undefined || !ArrayBuffer.isView(buffer)) {
			const o = (buffer ?? {}) as ReadOptions<T>;
			buf = o.buffer ?? (Buffer.alloc(16384) as unknown as T);
			off = o.offset ?? 0;
			len = o.length ?? buf.byteLength - off;
			pos = o.position ?? null;
		} else {
			buf = buffer;
			if (offset !== null && typeof offset === "object") {
				off = offset.offset ?? 0;
				len = offset.length ?? buf.byteLength - off;
				pos = offset.position ?? null;
			} else {
				off = offset ?? 0;
				len = length ?? buf.byteLength - off;
			}
		}
		validateInteger(off, "offset", 0);
		validateInteger(len, "length", 0);
		if (off + len > buf.byteLength) throw outOfRange("length", `<= ${buf.byteLength - off}`, len);
		validatePosition(pos);
		const p = pos == null ? -1 : pos;
		if (len === 0) return Promise.resolve({ bytesRead: 0, buffer: buf });
		return this.#op("read", async (fd) => ({
			bytesRead: await call<number>("read", fd, buf, off, len, p),
			buffer: buf,
		}));
	}

	readv<T extends ArrayBufferView[]>(buffers: T, position?: number | null): Promise<{ bytesRead: number; buffers: T }> {
		return this.#op("read", async (fd) => ({
			bytesRead: await call<number>("readv", fd, buffers, position ?? -1),
			buffers,
		}));
	}

	async readFile(
		options?: { encoding?: BufferEncodingOpt; signal?: AbortSignal } | BufferEncoding | null,
	): Promise<string | Buffer> {
		const opts = getOptions(options, { encoding: null as BufferEncodingOpt });
		return this.#op("read", async (fd) => encodeBuffer(await readFdAll(fd, opts.signal), opts.encoding));
	}

	readLines(options?: {
		encoding?: BufferEncoding;
		start?: number;
		end?: number;
		highWaterMark?: number;
	}): AsyncIterable<string> & EventEmitter {
		return readline.createInterface({ input: this.createReadStream(options), crlfDelay: Number.POSITIVE_INFINITY });
	}

	createReadStream(options?: Record<string, unknown>): ReadStream {
		// node:fs reads this module's promises export during its top-level initialization.
		const fs = require("node:fs") as typeof FsModule;
		return new fs.ReadStream(undefined, { ...options, fd: this });
	}

	createWriteStream(options?: Record<string, unknown>): WriteStream {
		// node:fs reads this module's promises export during its top-level initialization.
		const fs = require("node:fs") as typeof FsModule;
		return new fs.WriteStream(undefined, { ...options, fd: this });
	}

	readableWebStream(options?: { type?: "bytes" }): ReadableStream<Uint8Array> {
		if (this.#fd < 0 || this.#closing) throw closedErr("read");
		const handle = this;
		const RS = (globalThis as unknown as { ReadableStream: typeof ReadableStream }).ReadableStream;
		let position = 0;
		const source: UnderlyingDefaultSource<Uint8Array> = {
			async pull(controller) {
				const buf = Buffer.allocUnsafe(64 * 1024);
				const { bytesRead } = await handle.read(buf, 0, buf.length, position);
				if (bytesRead === 0) {
					controller.close();
					return;
				}
				position += bytesRead;
				controller.enqueue(new Uint8Array(buf.buffer, buf.byteOffset, bytesRead));
			},
		};
		void options;
		return new RS(source);
	}

	stat(options?: { bigint?: boolean }): Promise<Stats | BigIntStats> {
		const bigint = !!options?.bigint;
		return this.#op("fstat", async (fd) => getStatsFromBinding(await call<StatsArray>("fstat", fd, bigint), bigint));
	}

	truncate(len = 0): Promise<void> {
		validateInteger(len, "len");
		return this.#op("ftruncate", (fd) => call("ftruncate", fd, Math.max(0, len)));
	}

	utimes(atime: number | string | Date, mtime: number | string | Date): Promise<void> {
		const a = toUnixTimestamp(atime, "atime");
		const m = toUnixTimestamp(mtime, "mtime");
		return this.#op("futime", (fd) => call("futimes", fd, a, m));
	}

	write<T extends ArrayBufferView | string>(
		data: T,
		offsetOrPosition?: number | null | { offset?: number; length?: number; position?: number | null },
		lengthOrEncoding?: number | BufferEncoding | null,
		position?: number | null,
	): Promise<WriteResult<T>> {
		if (typeof data === "string") {
			const pos = typeof offsetOrPosition === "number" ? offsetOrPosition : null;
			const enc = typeof lengthOrEncoding === "string" ? lengthOrEncoding : "utf8";
			const bytes = Buffer.from(data, enc);
			return this.#op("write", async (fd) => ({
				bytesWritten: await call<number>("write", fd, bytes, 0, bytes.length, pos ?? -1),
				buffer: data,
			}));
		}
		if (!ArrayBuffer.isView(data))
			throw invalidArgType("buffer", "of type string or an instance of Buffer, TypedArray, or DataView", data);
		let off = 0;
		let len: number;
		let pos: number | null | undefined;
		if (offsetOrPosition !== null && typeof offsetOrPosition === "object") {
			off = offsetOrPosition.offset ?? 0;
			len = offsetOrPosition.length ?? data.byteLength - off;
			pos = offsetOrPosition.position;
		} else {
			off = offsetOrPosition ?? 0;
			len = typeof lengthOrEncoding === "number" ? lengthOrEncoding : data.byteLength - off;
			pos = position;
		}
		validateInteger(off, "offset", 0);
		validateInteger(len, "length", 0);
		if (off + len > data.byteLength) throw outOfRange("length", `<= ${data.byteLength - off}`, len);
		return this.#op("write", async (fd) => ({
			bytesWritten: await call<number>("write", fd, data, off, len, pos ?? -1),
			buffer: data,
		}));
	}

	writev<T extends ArrayBufferView[]>(
		buffers: T,
		position?: number | null,
	): Promise<{ bytesWritten: number; buffers: T }> {
		return this.#op("write", async (fd) => ({
			bytesWritten: await call<number>("writev", fd, buffers, position ?? -1),
			buffers,
		}));
	}

	writeFile(data: unknown, options?: BaseOptions | BufferEncoding): Promise<void> {
		const opts = getOptions(options, { encoding: "utf8" as BufferEncodingOpt });
		return this.#op("write", (fd) => writeData(fd, data, opts.encoding, null, opts.signal));
	}
}

// ---------------------------------------------------------------- helpers

/** Write all `bytes` to fd (at `position` and onward, or the current position). */
async function writeAll(fd: number, bytes: Uint8Array, position: number | null, signal?: AbortSignal): Promise<void> {
	let off = 0;
	let pos = position;
	while (off < bytes.length) {
		checkAborted(signal);
		const n = await call<number>("write", fd, bytes, off, bytes.length - off, pos ?? -1);
		off += n;
		if (pos !== null) pos += n;
	}
}

async function readFdAll(fd: number, signal?: AbortSignal): Promise<Buffer> {
	checkAborted(signal);
	const chunks: Buffer[] = [];
	let total = 0;
	for (;;) {
		checkAborted(signal);
		const buf = Buffer.allocUnsafe(kReadFileUnknownBufferLength);
		const n = await call<number>("read", fd, buf, 0, buf.length, -1);
		if (n === 0) break;
		chunks.push(n === buf.length ? buf : buf.subarray(0, n));
		total += n;
	}
	return Buffer.concat(chunks, total);
}

function isIterableData(data: unknown): data is Iterable<unknown> | AsyncIterable<unknown> {
	return (
		data !== null &&
		typeof data === "object" &&
		(typeof (data as Iterable<unknown>)[Symbol.iterator] === "function" ||
			typeof (data as AsyncIterable<unknown>)[Symbol.asyncIterator] === "function")
	);
}

async function writeData(
	fd: number,
	data: unknown,
	encoding: BufferEncodingOpt,
	position: number | null,
	signal?: AbortSignal,
): Promise<void> {
	if (typeof data === "string" || ArrayBuffer.isView(data)) {
		await writeAll(fd, toBytes(data, encoding), position, signal);
		return;
	}
	if (isIterableData(data)) {
		for await (const chunk of data as AsyncIterable<unknown>) {
			checkAborted(signal);
			await writeAll(fd, toBytes(chunk, encoding, "chunk"), null, signal);
		}
		return;
	}
	throw invalidArgType(
		"data",
		"of type string or an instance of Buffer, TypedArray, DataView, AsyncIterable, Iterable, or Stream",
		data,
	);
}

function rejected<T>(fn: () => Promise<T>): Promise<T> {
	try {
		return fn();
	} catch (e) {
		return Promise.reject(e);
	}
}

// ---------------------------------------------------------------- API

export function access(path: PathLike, mode: number = constants.F_OK): Promise<void> {
	return rejected(() => call("access", getValidatedPath(path), mode));
}

export function open(path: PathLike, flags?: string | number, mode?: number | string): Promise<FileHandle> {
	return rejected(async () => {
		const p = getValidatedPath(path);
		const fd = await call<number>("open", p, stringToFlags(flags ?? "r"), parseFileMode(mode, "mode", 0o666));
		return new FileHandle(fd);
	});
}

export function readFile(
	path: PathLike | FileHandle,
	options?: { encoding?: BufferEncodingOpt; flag?: string | number; signal?: AbortSignal } | BufferEncoding | null,
): Promise<string | Buffer> {
	return rejected(async () => {
		const opts = getOptions(options, { encoding: null as BufferEncodingOpt, flag: "r" as string | number });
		checkAborted(opts.signal);
		if (path instanceof FileHandle) return path.readFile(opts);
		const buf = await call<Buffer>("readFile", getValidatedPath(path), stringToFlags(opts.flag));
		checkAborted(opts.signal);
		return encodeBuffer(buf, opts.encoding);
	});
}

export function writeFile(
	path: PathLike | FileHandle,
	data: unknown,
	options?: BaseOptions | BufferEncoding | null,
): Promise<void> {
	return rejected(async () => {
		const opts = getOptions(options, {
			encoding: "utf8" as BufferEncodingOpt,
			mode: 0o666,
			flag: "w" as string | number,
		});
		checkAborted(opts.signal);
		if (path instanceof FileHandle) return path.writeFile(data, opts);
		const flags = stringToFlags(opts.flag);
		const mode = parseFileMode(opts.mode, "mode", 0o666);
		if (typeof data === "string" || ArrayBuffer.isView(data)) {
			if (!opts.signal) {
				await call("writeFile", getValidatedPath(path), toBytes(data, opts.encoding), flags, mode);
				return;
			}
		}
		const fh = await open(path as PathLike, flags, mode);
		try {
			await writeData(fh.fd, data, opts.encoding, null, opts.signal);
		} finally {
			await fh.close();
		}
	});
}

export function appendFile(
	path: PathLike | FileHandle,
	data: unknown,
	options?: BaseOptions | BufferEncoding | null,
): Promise<void> {
	const opts = getOptions(options, {
		encoding: "utf8" as BufferEncodingOpt,
		mode: 0o666,
		flag: "a" as string | number,
	});
	return writeFile(path, data, opts);
}

export function stat(path: PathLike, options?: { bigint?: boolean }): Promise<Stats | BigIntStats> {
	const bigint = !!options?.bigint;
	return rejected(async () =>
		getStatsFromBinding(await call<StatsArray>("stat", getValidatedPath(path), bigint), bigint),
	);
}

export function lstat(path: PathLike, options?: { bigint?: boolean }): Promise<Stats | BigIntStats> {
	const bigint = !!options?.bigint;
	return rejected(async () =>
		getStatsFromBinding(await call<StatsArray>("lstat", getValidatedPath(path), bigint), bigint),
	);
}

export function statfs(path: PathLike, options?: { bigint?: boolean }): Promise<StatFs<number | bigint>> {
	return rejected(async () =>
		getStatFsFromBinding(await call<(number | bigint)[]>("statfs", getValidatedPath(path), !!options?.bigint)),
	);
}

export function mkdir(
	path: PathLike,
	options?: { recursive?: boolean; mode?: number | string } | number | string,
): Promise<string | undefined> {
	return rejected(async () => {
		const opts = typeof options === "object" && options !== null ? options : { mode: options };
		const mode = parseFileMode(opts.mode, "mode", 0o777);
		const recursive = "recursive" in opts ? opts.recursive : false;
		if (recursive !== undefined && typeof recursive !== "boolean")
			throw invalidArgType("options.recursive", "of type boolean", recursive);
		if (recursive) return mkdirp(pathMod.resolve(pathString(path)), mode);
		await call("mkdir", getValidatedPath(path), mode);
		return undefined;
	});
}

export function mkdtemp(
	prefix: string | Buffer | URL,
	options?: { encoding?: BufferEncodingOpt } | BufferEncoding,
): Promise<string | Buffer> {
	return rejected(async () => {
		const opts = getOptions(options, { encoding: "utf8" as BufferEncodingOpt });
		return encodePath(await call<string>("mkdtemp", pathString(prefix, "prefix")), opts.encoding);
	});
}

export function mkdtempDisposable(
	prefix: string | Buffer | URL,
	options?: { encoding?: BufferEncodingOpt },
): Promise<{ path: string | Buffer; remove(): Promise<void>; [Symbol.asyncDispose](): Promise<void> }> {
	return rejected(async () => {
		const path = await mkdtemp(prefix, options);
		const p = typeof path === "string" ? path : path.toString();
		const remove = () => rm(p, { recursive: true, force: true });
		return { path, remove, [Symbol.asyncDispose]: remove };
	});
}

export function readdir(
	path: PathLike,
	options?: ReaddirOpts | BufferEncoding | null,
): Promise<(string | Buffer | Dirent)[]> {
	return rejected(() => {
		const opts = getOptions(options, { encoding: "utf8" as BufferEncodingOpt, withFileTypes: false, recursive: false });
		return readdirImpl(getValidatedPath(path), opts);
	});
}

export function readlink(
	path: PathLike,
	options?: { encoding?: BufferEncodingOpt } | BufferEncoding,
): Promise<string | Buffer> {
	return rejected(async () => {
		const opts = getOptions(options, { encoding: "utf8" as BufferEncodingOpt });
		return encodePath(await call<string>("readlink", getValidatedPath(path)), opts.encoding);
	});
}

export function realpath(
	path: PathLike,
	options?: { encoding?: BufferEncodingOpt } | BufferEncoding,
): Promise<string | Buffer> {
	return rejected(async () => {
		const opts = getOptions(options, { encoding: "utf8" as BufferEncodingOpt });
		return encodePath(await call<string>("realpath", getValidatedPath(path)), opts.encoding);
	});
}

export function rename(oldPath: PathLike, newPath: PathLike): Promise<void> {
	return rejected(() => call("rename", getValidatedPath(oldPath, "oldPath"), getValidatedPath(newPath, "newPath")));
}

export function rmdir(path: PathLike, options?: RmOpts): Promise<void> {
	return rejected(async () => {
		if (options?.recursive) return rmImpl(pathString(path), { ...options, recursive: true, force: false });
		await call("rmdir", getValidatedPath(path));
	});
}

export function rm(path: PathLike, options?: RmOpts): Promise<void> {
	return rejected(() => rmImpl(pathString(path), options ?? {}));
}

export function unlink(path: PathLike): Promise<void> {
	return rejected(() => call("unlink", getValidatedPath(path)));
}

export function copyFile(src: PathLike, dest: PathLike, mode = 0): Promise<void> {
	return rejected(() => call("copyfile", getValidatedPath(src, "src"), getValidatedPath(dest, "dest"), mode));
}

export function cp(src: PathLike, dest: PathLike, options?: CpOpts): Promise<void> {
	return rejected(() =>
		cpImpl(pathMod.resolve(pathString(src, "src")), pathMod.resolve(pathString(dest, "dest")), options ?? {}),
	);
}

export function link(existingPath: PathLike, newPath: PathLike): Promise<void> {
	return rejected(() =>
		call("link", getValidatedPath(existingPath, "existingPath"), getValidatedPath(newPath, "newPath")),
	);
}

export function symlink(target: PathLike, path: PathLike, type?: string | null): Promise<void> {
	return rejected(() => {
		const flags =
			type === "dir" ? constants.UV_FS_SYMLINK_DIR : type === "junction" ? constants.UV_FS_SYMLINK_JUNCTION : 0;
		return call("symlink", getValidatedPath(target, "target"), getValidatedPath(path), flags);
	});
}

export function chmod(path: PathLike, mode: number | string): Promise<void> {
	return rejected(() => call("chmod", getValidatedPath(path), parseFileMode(mode, "mode")));
}

export function lchmod(path: PathLike, mode: number | string): Promise<void> {
	return rejected(async () => {
		const p = getValidatedPath(path);
		const m = parseFileMode(mode, "mode");
		if (process.platform !== "darwin")
			throw codedError("ERR_METHOD_NOT_IMPLEMENTED", "The lchmod() method is not implemented");
		(binding.lchmod as (p: NativePath, m: number) => void)(p, m);
	});
}

export function chown(path: PathLike, uid: number, gid: number): Promise<void> {
	return rejected(() => {
		validateInteger(uid, "uid", -1, 0xffffffff);
		validateInteger(gid, "gid", -1, 0xffffffff);
		return call("chown", getValidatedPath(path), uid, gid);
	});
}

export function lchown(path: PathLike, uid: number, gid: number): Promise<void> {
	return rejected(() => {
		validateInteger(uid, "uid", -1, 0xffffffff);
		validateInteger(gid, "gid", -1, 0xffffffff);
		return call("lchown", getValidatedPath(path), uid, gid);
	});
}

export function utimes(path: PathLike, atime: number | string | Date, mtime: number | string | Date): Promise<void> {
	return rejected(() =>
		call("utimes", getValidatedPath(path), toUnixTimestamp(atime, "atime"), toUnixTimestamp(mtime, "mtime")),
	);
}

export function lutimes(path: PathLike, atime: number | string | Date, mtime: number | string | Date): Promise<void> {
	return rejected(() =>
		call("lutimes", getValidatedPath(path), toUnixTimestamp(atime, "atime"), toUnixTimestamp(mtime, "mtime")),
	);
}

export function truncate(path: PathLike, len = 0): Promise<void> {
	return rejected(async () => {
		validateInteger(len, "len");
		const fh = await open(path, "r+");
		try {
			await fh.truncate(len);
		} finally {
			await fh.close();
		}
	});
}

export function opendir(path: PathLike, options?: OpendirOptions): Promise<Dir> {
	return rejected(() => {
		const opts = getOptions(options, { encoding: "utf8" as BufferEncodingOpt, bufferSize: 32, recursive: false });
		if (opts.bufferSize !== undefined) validateInteger(opts.bufferSize, "options.bufferSize", 1, 4294967295);
		return opendirImpl(getValidatedPath(path), pathString(path), opts);
	});
}

export function watch(
	filename: PathLike,
	options?: (WatchOptions & { maxQueue?: number; overflow?: "ignore" | "throw" }) | BufferEncoding,
): AsyncIterableIterator<{ eventType: string; filename: string | Buffer | null }> {
	const opts = getOptions(options, { persistent: true, recursive: false, encoding: "utf8" as BufferEncodingOpt });
	if (opts.signal?.aborted) {
		const err = abortErr(opts.signal);
		return {
			next: () => Promise.reject(err),
			return: () => Promise.resolve({ value: undefined, done: true }),
			[Symbol.asyncIterator]() {
				return this;
			},
		};
	}
	return watchAsync(getValidatedPath(filename, "filename"), opts);
}

export function glob(pattern: string | readonly string[], options?: NodeGlobOptions): AsyncGenerator<string | Dirent> {
	return globImpl(pattern, options);
}

const promises = {
	access,
	appendFile,
	chmod,
	chown,
	constants,
	copyFile,
	cp,
	glob,
	lchmod,
	lchown,
	link,
	lstat,
	lutimes,
	mkdir,
	mkdtemp,
	mkdtempDisposable,
	open,
	opendir,
	readdir,
	readFile,
	readlink,
	realpath,
	rename,
	rm,
	rmdir,
	stat,
	statfs,
	symlink,
	truncate,
	unlink,
	utimes,
	watch,
	writeFile,
	FileHandle,
};

export default promises;
