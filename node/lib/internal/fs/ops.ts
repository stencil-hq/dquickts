// internal:fs/ops — the recursive operations of node:fs shared by the sync,
// callback and promise APIs: mkdir -p, rm -r, cp -r, readdir recursive,
// realpath(JS form), and the promise wrapper of the qbun:fs natives.

import * as binding from "qbun:fs";
import * as pathMod from "node:path";
import { Buffer } from "node:buffer";
import { codedError } from "internal:errors";
import {
	type NativePath,
	type StatsArray,
	type BufferEncodingOpt,
	Dirent,
	Stats,
	constants,
	encodePath,
	getStatsFromBinding,
	UV_DIRENT_DIR,
	UV_DIRENT_UNKNOWN,
	direntTypeFromMode,
} from "internal:fs/utils";

type NativeFn = (...args: unknown[]) => unknown;
const natives = binding as unknown as Record<string, NativeFn>;

/** Run native `name` asynchronously → Promise of its result. */
export function call<T>(name: string, ...args: unknown[]): Promise<T> {
	const { promise, resolve, reject } = Promise.withResolvers<T>();
	try {
		natives[name](...args, (err: Error | null, res: T) => (err ? reject(err) : resolve(res)));
	} catch (e) {
		reject(e);
	}
	return promise;
}

/** Run native `name` synchronously. */
export function sync<T>(name: string, ...args: unknown[]): T {
	return natives[name](...args) as T;
}

interface ErrnoError extends Error {
	code?: string;
	syscall?: string;
	path?: string;
}

const S_IFMT = constants.S_IFMT;
const S_IFDIR = constants.S_IFDIR;

function isDirMode(mode: number): boolean {
	return (mode & S_IFMT) === S_IFDIR;
}

// ---------------------------------------------------------------- mkdir -p

/** mkdir -p: returns the first directory created, or undefined. */
export function mkdirpSync(path: string, mode: number): string | undefined {
	try {
		sync("mkdir", path, mode);
		return path;
	} catch (e) {
		const err = e as ErrnoError;
		if (err.code === "EEXIST") {
			if (isDirMode(sync<number[]>("stat", path, false)[1])) return undefined;
			throw err;
		}
		if (err.code !== "ENOENT") throw err;
		const parent = pathMod.dirname(path);
		if (parent === path) throw err;
		const first = mkdirpSync(parent, mode);
		try {
			sync("mkdir", path, mode);
		} catch (e2) {
			const err2 = e2 as ErrnoError;
			if (err2.code !== "EEXIST" || !isDirMode(sync<number[]>("stat", path, false)[1])) throw err2;
			return first;
		}
		return first ?? path;
	}
}

export async function mkdirp(path: string, mode: number): Promise<string | undefined> {
	try {
		await call("mkdir", path, mode);
		return path;
	} catch (e) {
		const err = e as ErrnoError;
		if (err.code === "EEXIST") {
			const st = await call<number[]>("stat", path, false);
			if (isDirMode(st[1])) return undefined;
			throw err;
		}
		if (err.code !== "ENOENT") throw err;
		const parent = pathMod.dirname(path);
		if (parent === path) throw err;
		const first = await mkdirp(parent, mode);
		try {
			await call("mkdir", path, mode);
		} catch (e2) {
			const err2 = e2 as ErrnoError;
			if (err2.code !== "EEXIST") throw err2;
			const st = await call<number[]>("stat", path, false);
			if (!isDirMode(st[1])) throw err2;
			return first;
		}
		return first ?? path;
	}
}

// ---------------------------------------------------------------- readdir

export interface ReaddirOpts {
	encoding?: BufferEncodingOpt;
	withFileTypes?: boolean;
	recursive?: boolean;
}

type Entries = [string[], number[]];

function fixUnknown(dir: string, names: string[], types: number[]): void {
	for (let i = 0; i < types.length; i++) {
		if (types[i] === UV_DIRENT_UNKNOWN) {
			try {
				types[i] = direntTypeFromMode(sync<number[]>("lstat", pathMod.join(dir, names[i]), false)[1]);
			} catch {
				// keep unknown
			}
		}
	}
}

function shapeEntries(
	dir: string,
	[names, types]: Entries,
	opts: ReaddirOpts,
	prefix: string,
): (string | Buffer | Dirent)[] {
	const out: (string | Buffer | Dirent)[] = [];
	for (let i = 0; i < names.length; i++) {
		if (opts.withFileTypes) out.push(new Dirent(encodePath(names[i], opts.encoding), types[i], dir));
		else out.push(encodePath(prefix ? pathMod.join(prefix, names[i]) : names[i], opts.encoding));
	}
	return out;
}

export function readdirSyncImpl(path: NativePath, opts: ReaddirOpts): (string | Buffer | Dirent)[] {
	const entries = sync<Entries>("scandir", path);
	const dir = typeof path === "string" ? path : Buffer.from(path).toString();
	if (opts.withFileTypes) fixUnknown(dir, entries[0], entries[1]);
	if (!opts.recursive) return shapeEntries(dir, entries, opts, "");
	const out: (string | Buffer | Dirent)[] = [];
	const queue: [string, string, Entries][] = [[dir, "", entries]];
	while (queue.length) {
		const [abs, rel, ents] = queue.shift() as [string, string, Entries];
		if (!opts.withFileTypes) fixUnknownDirs(abs, ents);
		out.push(...shapeEntries(abs, ents, opts, rel));
		for (let i = 0; i < ents[0].length; i++) {
			if (ents[1][i] !== UV_DIRENT_DIR) continue;
			const childAbs = pathMod.join(abs, ents[0][i]);
			const childRel = rel ? pathMod.join(rel, ents[0][i]) : ents[0][i];
			let child: Entries;
			try {
				child = sync<Entries>("scandir", childAbs);
			} catch (e) {
				if ((e as ErrnoError).code === "ENOENT") continue;
				throw e;
			}
			if (opts.withFileTypes) fixUnknown(childAbs, child[0], child[1]);
			queue.push([childAbs, childRel, child]);
		}
	}
	return out;
}

function fixUnknownDirs(dir: string, ents: Entries): void {
	if (ents[1].includes(UV_DIRENT_UNKNOWN)) fixUnknown(dir, ents[0], ents[1]);
}

async function fixUnknownAsync(dir: string, names: string[], types: number[]): Promise<void> {
	for (let i = 0; i < types.length; i++) {
		if (types[i] === UV_DIRENT_UNKNOWN) {
			try {
				types[i] = direntTypeFromMode((await call<number[]>("lstat", pathMod.join(dir, names[i]), false))[1]);
			} catch {
				// keep unknown
			}
		}
	}
}

export async function readdirImpl(path: NativePath, opts: ReaddirOpts): Promise<(string | Buffer | Dirent)[]> {
	const entries = await call<Entries>("scandir", path);
	const dir = typeof path === "string" ? path : Buffer.from(path).toString();
	if (!opts.recursive) {
		if (opts.withFileTypes) await fixUnknownAsync(dir, entries[0], entries[1]);
		return shapeEntries(dir, entries, opts, "");
	}
	const out: (string | Buffer | Dirent)[] = [];
	const queue: [string, string, Entries][] = [[dir, "", entries]];
	while (queue.length) {
		const [abs, rel, ents] = queue.shift() as [string, string, Entries];
		if (ents[1].includes(UV_DIRENT_UNKNOWN)) await fixUnknownAsync(abs, ents[0], ents[1]);
		out.push(...shapeEntries(abs, ents, opts, rel));
		const subs: Promise<void>[] = [];
		for (let i = 0; i < ents[0].length; i++) {
			if (ents[1][i] !== UV_DIRENT_DIR) continue;
			const childAbs = pathMod.join(abs, ents[0][i]);
			const childRel = rel ? pathMod.join(rel, ents[0][i]) : ents[0][i];
			const slot: [string, string, Entries] = [childAbs, childRel, [[], []]];
			queue.push(slot);
			subs.push(
				call<Entries>("scandir", childAbs).then(
					(child) => {
						slot[2] = child;
					},
					(e: ErrnoError) => {
						if (e.code !== "ENOENT") throw e;
					},
				),
			);
		}
		await Promise.all(subs);
	}
	return out;
}

// ---------------------------------------------------------------- rm

export interface RmOpts {
	recursive?: boolean;
	force?: boolean;
	maxRetries?: number;
	retryDelay?: number;
}

const retryCodes: Record<string, true> = { EBUSY: true, EMFILE: true, ENFILE: true, ENOTEMPTY: true, EPERM: true };

function sleepSync(ms: number): void {
	const end = Date.now() + ms;
	const sab = new Int32Array(new SharedArrayBuffer(4));
	while (Date.now() < end) Atomics.wait(sab, 0, 0, end - Date.now());
}

function rmTreeSync(path: string, isDir: boolean): void {
	if (!isDir) {
		sync("unlink", path);
		return;
	}
	const [names] = sync<Entries>("scandir", path);
	for (const name of names) {
		const child = pathMod.join(path, name);
		let st: number[];
		try {
			st = sync<number[]>("lstat", child, false);
		} catch (e) {
			if ((e as ErrnoError).code === "ENOENT") continue;
			throw e;
		}
		rmTreeSync(child, isDirMode(st[1]));
	}
	sync("rmdir", path);
}

export function rmSyncImpl(path: string, opts: RmOpts): void {
	let st: number[];
	try {
		st = sync<number[]>("lstat", path, false);
	} catch (e) {
		if ((e as ErrnoError).code === "ENOENT" && opts.force) return;
		throw rmErr(e as ErrnoError);
	}
	const isDir = isDirMode(st[1]);
	if (isDir && !opts.recursive) {
		throw Object.assign(
			codedError("ERR_FS_EISDIR", `Path is a directory: rm returned EISDIR (is a directory) ${path}`, Error),
			{ errno: 21, syscall: "rm", path },
		);
	}
	const tries = (opts.maxRetries ?? 0) + 1;
	for (let i = 1; ; i++) {
		try {
			rmTreeSync(path, isDir);
			return;
		} catch (e) {
			const err = e as ErrnoError;
			if (err.code === "ENOENT" && opts.force) return;
			if (i >= tries || !err.code || !retryCodes[err.code]) throw err;
			sleepSync(i * (opts.retryDelay ?? 100));
		}
	}
}

function rmErr(e: ErrnoError): ErrnoError {
	if (e.syscall === "lstat") e.syscall = "rm";
	return e;
}

async function rmTree(path: string, isDir: boolean): Promise<void> {
	if (!isDir) {
		await call("unlink", path);
		return;
	}
	const [names] = await call<Entries>("scandir", path);
	await Promise.all(
		names.map(async (name) => {
			const child = pathMod.join(path, name);
			let st: number[];
			try {
				st = await call<number[]>("lstat", child, false);
			} catch (e) {
				if ((e as ErrnoError).code === "ENOENT") return;
				throw e;
			}
			await rmTree(child, isDirMode(st[1]));
		}),
	);
	await call("rmdir", path);
}

export async function rmImpl(path: string, opts: RmOpts): Promise<void> {
	let st: number[];
	try {
		st = await call<number[]>("lstat", path, false);
	} catch (e) {
		if ((e as ErrnoError).code === "ENOENT" && opts.force) return;
		throw rmErr(e as ErrnoError);
	}
	const isDir = isDirMode(st[1]);
	if (isDir && !opts.recursive) {
		throw Object.assign(
			codedError("ERR_FS_EISDIR", `Path is a directory: rm returned EISDIR (is a directory) ${path}`, Error),
			{ errno: 21, syscall: "rm", path },
		);
	}
	const tries = (opts.maxRetries ?? 0) + 1;
	for (let i = 1; ; i++) {
		try {
			await rmTree(path, isDir);
			return;
		} catch (e) {
			const err = e as ErrnoError;
			if (err.code === "ENOENT" && opts.force) return;
			if (i >= tries || !err.code || !retryCodes[err.code]) throw err;
			const { promise, resolve } = Promise.withResolvers<void>();
			setTimeout(resolve, i * (opts.retryDelay ?? 100));
			await promise;
		}
	}
}

/** rmdir with { recursive } (deprecated Node behavior: rm -r without force). */
export function rmdirRecursiveSync(path: string, opts: RmOpts): void {
	rmSyncImpl(path, { ...opts, recursive: true, force: false });
}

// ---------------------------------------------------------------- cp

export interface CpOpts {
	dereference?: boolean;
	errorOnExist?: boolean;
	filter?: (src: string, dest: string) => boolean | Promise<boolean>;
	force?: boolean;
	mode?: number;
	preserveTimestamps?: boolean;
	recursive?: boolean;
	verbatimSymlinks?: boolean;
}

type StatT = Stats;

function cpError(code: string, message: string, path: string): Error {
	return Object.assign(codedError(code, `${message}`, Error), { errno: 0, syscall: "cp", path });
}

function isSrcSubdir(src: string, dest: string): boolean {
	const s = pathMod.resolve(src).split(pathMod.sep).filter(Boolean);
	const d = pathMod.resolve(dest).split(pathMod.sep).filter(Boolean);
	return s.every((cur, i) => d[i] === cur);
}

function checkPaths(src: string, dest: string, srcStat: StatT, destStat: StatT | undefined, opts: CpOpts): void {
	if (destStat && srcStat.ino && srcStat.dev && destStat.ino === srcStat.ino && destStat.dev === srcStat.dev) {
		throw cpError(
			"ERR_FS_CP_EINVAL",
			`Invalid src or dest: cp returned EINVAL (src and dest cannot be the same) ${dest}`,
			dest,
		);
	}
	if (srcStat.isDirectory() && destStat && !destStat.isDirectory()) {
		throw cpError(
			"ERR_FS_CP_DIR_TO_NON_DIR",
			`Cannot overwrite non-directory with directory: cp returned EISDIR (cannot overwrite non-directory ${dest} with directory ${src}) ${dest}`,
			dest,
		);
	}
	if (!srcStat.isDirectory() && destStat?.isDirectory()) {
		throw cpError(
			"ERR_FS_CP_NON_DIR_TO_DIR",
			`Cannot overwrite directory with non-directory: cp returned ENOTDIR (cannot overwrite directory ${dest} with non-directory ${src}) ${dest}`,
			dest,
		);
	}
	if (srcStat.isDirectory() && isSrcSubdir(src, dest)) {
		throw cpError(
			"ERR_FS_CP_EINVAL",
			`Invalid src or dest: cp returned EINVAL (cannot copy ${src} to a subdirectory of self ${dest}) ${dest}`,
			dest,
		);
	}
	if (srcStat.isDirectory() && !opts.recursive) {
		throw cpError(
			"ERR_FS_EISDIR",
			`Recursive option is required to copy a directory: cp returned EISDIR (${src} is a directory (not copied)) ${src}`,
			src,
		);
	}
}

function statSyncMaybe(path: string, deref: boolean): StatT | undefined {
	try {
		return getStatsFromBinding(sync<StatsArray>(deref ? "stat" : "lstat", path, false));
	} catch (e) {
		if ((e as ErrnoError).code === "ENOENT") return undefined;
		throw e;
	}
}

function copyFileSyncCp(src: string, dest: string, srcStat: StatT, destExists: boolean, opts: CpOpts): void {
	if (destExists) {
		if (opts.force === false) {
			if (opts.errorOnExist)
				throw cpError(
					"ERR_FS_CP_EEXIST",
					`Target already exists: cp returned EEXIST (${dest} already exists) ${dest}`,
					dest,
				);
			return;
		}
		sync("unlink", dest);
	}
	sync("copyfile", src, dest, opts.mode ?? 0);
	if (opts.preserveTimestamps) {
		if ((srcStat.mode & 0o200) === 0) sync("chmod", dest, srcStat.mode | 0o200);
		sync("utimes", dest, srcStat.atimeMs / 1000, srcStat.mtimeMs / 1000);
	}
	sync("chmod", dest, srcStat.mode & 0o7777);
}

export function cpSyncImpl(src: string, dest: string, opts: CpOpts): void {
	if (opts.filter) {
		const r = opts.filter(src, dest);
		if (r instanceof Promise)
			throw codedError(
				"ERR_INVALID_RETURN_VALUE",
				"Expected a boolean from the filter function, got a Promise",
				TypeError,
			);
		if (!r) return;
	}
	const srcStat = getStatsFromBinding(sync<StatsArray>(opts.dereference ? "stat" : "lstat", src, false)) as StatT;
	const destStat = statSyncMaybe(dest, !!opts.dereference);
	checkPaths(src, dest, srcStat, destStat, opts);
	if (!destStat) {
		const parent = pathMod.dirname(dest);
		if (!statSyncMaybe(parent, true)) mkdirpSync(parent, 0o777);
	}
	if (srcStat.isDirectory()) {
		if (!destStat) sync("mkdir", dest, srcStat.mode & 0o7777);
		for (const name of sync<Entries>("scandir", src)[0]) {
			cpSyncImpl(pathMod.join(src, name), pathMod.join(dest, name), opts);
		}
		if (!destStat) sync("chmod", dest, srcStat.mode & 0o7777);
		return;
	}
	if (srcStat.isSymbolicLink()) {
		let target = sync<string>("readlink", src);
		if (!opts.verbatimSymlinks && !pathMod.isAbsolute(target)) target = pathMod.resolve(pathMod.dirname(src), target);
		if (destStat) {
			let destTarget: string | undefined;
			try {
				destTarget = sync<string>("readlink", dest);
			} catch {
				destTarget = undefined;
			}
			if (destTarget !== undefined) {
				const resolvedDest = pathMod.isAbsolute(destTarget)
					? destTarget
					: pathMod.resolve(pathMod.dirname(dest), destTarget);
				if (srcStat.isDirectory() && isSrcSubdir(target, resolvedDest)) {
					throw cpError(
						"ERR_FS_CP_EINVAL",
						`Invalid src or dest: cp returned EINVAL (cannot copy ${target} to a subdirectory of self ${resolvedDest}) ${dest}`,
						dest,
					);
				}
			}
			sync("unlink", dest);
		}
		sync("symlink", target, dest, 0);
		return;
	}
	if (srcStat.isFile() || srcStat.isCharacterDevice() || srcStat.isBlockDevice()) {
		copyFileSyncCp(src, dest, srcStat, !!destStat, opts);
		return;
	}
	if (srcStat.isSocket())
		throw cpError(
			"ERR_FS_CP_SOCKET",
			`Cannot copy a socket file: cp returned EINVAL (cannot copy a socket file: ${dest}) ${dest}`,
			dest,
		);
	if (srcStat.isFIFO())
		throw cpError(
			"ERR_FS_CP_FIFO_PIPE",
			`Cannot copy a FIFO pipe: cp returned EINVAL (cannot copy a FIFO pipe: ${dest}) ${dest}`,
			dest,
		);
	throw cpError(
		"ERR_FS_CP_UNKNOWN",
		`Cannot copy an unknown file type: cp returned EINVAL (cannot copy an unknown file type: ${dest}) ${dest}`,
		dest,
	);
}

async function statMaybe(path: string, deref: boolean): Promise<StatT | undefined> {
	try {
		return getStatsFromBinding(await call<StatsArray>(deref ? "stat" : "lstat", path, false));
	} catch (e) {
		if ((e as ErrnoError).code === "ENOENT") return undefined;
		throw e;
	}
}

export async function cpImpl(src: string, dest: string, opts: CpOpts): Promise<void> {
	if (opts.filter && !(await opts.filter(src, dest))) return;
	const srcStat = getStatsFromBinding(await call<StatsArray>(opts.dereference ? "stat" : "lstat", src, false)) as StatT;
	const destStat = await statMaybe(dest, !!opts.dereference);
	checkPaths(src, dest, srcStat, destStat, opts);
	if (!destStat) {
		const parent = pathMod.dirname(dest);
		if (!(await statMaybe(parent, true))) await mkdirp(parent, 0o777);
	}
	if (srcStat.isDirectory()) {
		if (!destStat) await call("mkdir", dest, srcStat.mode & 0o7777);
		const [names] = await call<Entries>("scandir", src);
		for (const name of names) await cpImpl(pathMod.join(src, name), pathMod.join(dest, name), opts);
		if (!destStat) await call("chmod", dest, srcStat.mode & 0o7777);
		return;
	}
	if (srcStat.isSymbolicLink()) {
		let target = await call<string>("readlink", src);
		if (!opts.verbatimSymlinks && !pathMod.isAbsolute(target)) target = pathMod.resolve(pathMod.dirname(src), target);
		if (destStat) await call("unlink", dest);
		await call("symlink", target, dest, 0);
		return;
	}
	if (srcStat.isFile() || srcStat.isCharacterDevice() || srcStat.isBlockDevice()) {
		if (destStat) {
			if (opts.force === false) {
				if (opts.errorOnExist)
					throw cpError(
						"ERR_FS_CP_EEXIST",
						`Target already exists: cp returned EEXIST (${dest} already exists) ${dest}`,
						dest,
					);
				return;
			}
			await call("unlink", dest);
		}
		await call("copyfile", src, dest, opts.mode ?? 0);
		if (opts.preserveTimestamps) {
			if ((srcStat.mode & 0o200) === 0) await call("chmod", dest, srcStat.mode | 0o200);
			await call("utimes", dest, srcStat.atimeMs / 1000, srcStat.mtimeMs / 1000);
		}
		await call("chmod", dest, srcStat.mode & 0o7777);
		return;
	}
	if (srcStat.isSocket())
		throw cpError(
			"ERR_FS_CP_SOCKET",
			`Cannot copy a socket file: cp returned EINVAL (cannot copy a socket file: ${dest}) ${dest}`,
			dest,
		);
	if (srcStat.isFIFO())
		throw cpError(
			"ERR_FS_CP_FIFO_PIPE",
			`Cannot copy a FIFO pipe: cp returned EINVAL (cannot copy a FIFO pipe: ${dest}) ${dest}`,
			dest,
		);
	throw cpError(
		"ERR_FS_CP_UNKNOWN",
		`Cannot copy an unknown file type: cp returned EINVAL (cannot copy an unknown file type: ${dest}) ${dest}`,
		dest,
	);
}
