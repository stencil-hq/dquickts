// internal:child/common — plumbing shared by node:child_process, Bun.spawn and
// the Bun Shell: the qbun:process / qbun:net handle types, signals, errno
// names, environment blocks, PATH lookup and pipe I/O.

import * as core from "qbun:core";
import * as processBinding from "qbun:process";
import * as netBinding from "qbun:net";
import * as osBinding from "qbun:os";
import { isAbsolute, join, resolve } from "node:path";
import { accessSync, constants as fsConstants, openSync, closeSync } from "node:fs";
import { codedError, invalidArgType } from "internal:errors";

// ---- native handle types (qbun:net / qbun:process contract) ----

export interface StreamHandle {
	readonly fd: number;
	onread: ((chunk: Uint8Array | null, err?: Error) => void) | null;
	reading?: boolean;
	readStart(): void;
	readStop(): void;
	write(data: Uint8Array | string, cb: (err: Error | null) => void): void;
	writev(chunks: (Uint8Array | string)[], cb: (err: Error | null) => void): void;
	tryWrite(data: Uint8Array | string): number;
	readonly writeQueueSize: number;
	shutdown(cb: (err: Error | null) => void): void;
	close(cb?: () => void): void;
	ref(): void;
	unref(): void;
	hasRef(): boolean;
	open(fd: number): void;
}

export interface ProcessHandle {
	readonly pid: number;
	kill(signal: number): number;
	ref(): void;
	unref(): void;
	hasRef(): boolean;
	close(): void;
}

export type StdioSpec =
	| { type: "pipe"; handle: StreamHandle; readable: boolean; writable: boolean }
	| { type: "ignore" }
	| { type: "inherit"; fd: number }
	| { type: "fd"; fd: number };

export interface NativeSpawnOptions {
	file: string;
	/** the full argv, argv[0] included */
	args: string[];
	cwd?: string;
	env: string[];
	stdio: StdioSpec[];
	detached?: boolean;
	uid?: number;
	gid?: number;
}

export type SyncStdio = "pipe" | "inherit" | "ignore" | number;

export interface NativeSpawnSyncOptions {
	file: string;
	args: string[];
	cwd?: string;
	env: string[];
	stdio: SyncStdio[];
	input?: Uint8Array;
	timeout?: number;
	killSignal?: number;
	maxBuffer?: number;
	detached?: boolean;
	uid?: number;
	gid?: number;
}

export interface NativeSpawnSyncResult {
	pid: number;
	status: number | null;
	signal: string | null;
	output: (Uint8Array | null)[] | null;
	error?: Error & { errno?: number; code?: string };
	resourceUsage?: Record<string, number>;
}

/** Child resource usage as qbun:process reports it (Node's process.resourceUsage() fields). */
export type NativeResourceUsage = Record<string, number>;

export type OnExit = (exitCode: number, signalCode: string | null, rusage?: NativeResourceUsage) => void;

interface ProcessNative {
	spawn(options: NativeSpawnOptions, onexit: OnExit): ProcessHandle;
	spawnSync(options: NativeSpawnSyncOptions): NativeSpawnSyncResult;
	kill(pid: number, signal: number): number;
	signals: Record<string, number>;
}

interface NetNative {
	createPipe(ipc?: boolean): StreamHandle;
}

interface OsNative {
	constants: { errno: Record<string, number> };
}

// the C module namespaces, typed by the binding contract
export const processNative = processBinding as unknown as ProcessNative;
const netNative = netBinding as unknown as NetNative;
const osNative = osBinding as unknown as OsNative;

export function createPipe(ipc = false): StreamHandle {
	return netNative.createPipe(ipc);
}

// ---- signals ----

export const signals: Record<string, number> = processNative.signals;
let signalNames: Map<number, string> | undefined;

/** The name of signal number `signum` ("SIGTERM"), or undefined. */
export function signalName(signum: number): string | undefined {
	if (!signalNames) {
		signalNames = new Map();
		for (const name in signals) if (!signalNames.has(signals[name])) signalNames.set(signals[name], name);
	}
	return signalNames.get(signum);
}

/** Node's convertToValidSignal: a known signal number or (case-insensitive) name → number. */
export function convertToValidSignal(signal: unknown): number {
	if (typeof signal === "number" && signalName(signal) !== undefined) return signal;
	if (typeof signal === "string") {
		const signum = signals[signal.toUpperCase()];
		if (signum !== undefined) return signum;
	}
	throw codedError("ERR_UNKNOWN_SIGNAL", `Unknown signal: ${String(signal)}`, TypeError);
}

/** Validate a `killSignal` option (undefined stays undefined). */
export function sanitizeKillSignal(killSignal: unknown, name = "options.killSignal"): number | undefined {
	if (typeof killSignal === "string" || typeof killSignal === "number") return convertToValidSignal(killSignal);
	if (killSignal != null) throw invalidArgType(name, "of type string or number", killSignal);
	return undefined;
}

// ---- errno ----

let errnoNames: Map<number, string> | undefined;

/** The libuv/POSIX name of a negative errno ("ENOENT"). */
export function errnoName(errno: number): string {
	if (!errnoNames) {
		errnoNames = new Map();
		const table = osNative.constants.errno;
		for (const name in table) if (!errnoNames.has(table[name])) errnoNames.set(table[name], name);
	}
	return errnoNames.get(-errno) ?? `Unknown system error ${errno}`;
}

/** The negative errno of a POSIX error name (-ESRCH). */
export function errnoValue(name: string): number {
	const value = osNative.constants.errno[name];
	return value === undefined ? Number.NaN : -value;
}

export interface ErrnoError extends Error {
	errno: number;
	code: string;
	syscall: string;
	path?: string;
	spawnargs?: string[];
}

/** Node's ErrnoException: message `${syscall} ${code}`. */
export function errnoException(errno: number, syscall: string, original?: string): ErrnoError {
	const code = errnoName(errno);
	const err = new Error(original ? `${syscall} ${code} ${original}` : `${syscall} ${code}`) as ErrnoError;
	err.errno = errno;
	err.code = code;
	err.syscall = syscall;
	return err;
}

/** The (negative) errno carried by a system error from a native binding, or undefined. */
export function nativeErrno(err: unknown): number | undefined {
	if (err === null || typeof err !== "object") return undefined;
	const { errno, code } = err as { errno?: unknown; code?: unknown };
	if (typeof errno === "number") return errno;
	if (typeof code === "string") {
		const value = errnoValue(code);
		if (!Number.isNaN(value)) return value;
	}
	return undefined;
}

// ---- environment ----

/** "K=V" pairs of every defined entry of `env` (prototype keys included, like Node). */
export function envPairs(env: Record<string, unknown>, check?: (key: string, value: string) => void): string[] {
	const pairs: string[] = [];
	for (const key in env) {
		const value = env[key];
		if (value === undefined) continue;
		const s = String(value);
		if (check) check(key, s);
		pairs.push(`${key}=${s}`);
	}
	return pairs;
}

/** The value of `name` in "K=V" pairs (last one wins), or undefined. */
export function envPairsGet(pairs: readonly string[], name: string): string | undefined {
	const prefix = `${name}=`;
	for (let i = pairs.length - 1; i >= 0; i--) if (pairs[i].startsWith(prefix)) return pairs[i].slice(prefix.length);
	return undefined;
}

// ---- PATH lookup ----

function isExecutableFile(path: string): boolean {
	try {
		if (core.statKind(path) !== 1) return false;
		accessSync(path, fsConstants.X_OK);
		return true;
	} catch {
		return false;
	}
}

/** Bun.which: the executable `command` resolves to through `PATH` (relative entries from `cwd`), or null. */
export function which(command: string, PATH: string | undefined, cwd: string): string | null {
	if (command.length === 0) return null;
	if (command.includes("/")) {
		const full = isAbsolute(command) ? command : resolve(cwd, command);
		return isExecutableFile(full) ? full : null;
	}
	if (!PATH) return null;
	for (const dir of PATH.split(":")) {
		if (dir.length === 0) continue;
		const full = isAbsolute(dir) ? join(dir, command) : resolve(cwd, dir, command);
		if (isExecutableFile(full)) return full;
	}
	return null;
}

// ---- Buffer / Blob ----

export type NodeBuffer = Uint8Array & { toString(encoding?: string): string };

/** Concatenate chunks into one Buffer. */
export function concatBuffers(chunks: readonly Uint8Array[], total?: number): NodeBuffer {
	return Buffer.concat(chunks, total);
}

/** A Buffer viewing `bytes` (no copy). */
export function toBuffer(bytes: Uint8Array): NodeBuffer {
	return Buffer.from(bytes.buffer as ArrayBuffer, bytes.byteOffset, bytes.byteLength);
}

// ---- JS values used as stdio ----

/** The bytes viewed by an ArrayBuffer, SharedArrayBuffer or ArrayBufferView, or undefined. */
export function bytesOf(value: unknown): Uint8Array | undefined {
	if (
		value instanceof ArrayBuffer ||
		(typeof SharedArrayBuffer !== "undefined" && value instanceof SharedArrayBuffer)
	) {
		return new Uint8Array(value);
	}
	if (ArrayBuffer.isView(value)) return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
	return undefined;
}

/** Descriptor of a descriptor-backed Bun.file(fd) (getter defined by internal:bun/file). */
const kBunFileFd = Symbol.for("qbun.BunFile.fd");

export interface BunFileLike {
	readonly name?: string | number;
	readonly [kBunFileFd]?: number;
	exists(): Promise<boolean>;
	writer(): unknown;
}

/** A Bun.file() instance (duck-typed: BunFile-only methods). */
export function isBunFile(value: unknown): value is BunFileLike {
	if (value === null || typeof value !== "object") return false;
	const v = value as Partial<BunFileLike>;
	return typeof v.exists === "function" && typeof v.writer === "function";
}

export function isBlob(value: unknown): value is Blob {
	return (
		value !== null &&
		typeof value === "object" &&
		typeof (value as Blob).arrayBuffer === "function" &&
		typeof (value as Blob).stream === "function" &&
		typeof (value as Blob).size === "number"
	);
}

export interface BodyLike {
	readonly body: ReadableStream<Uint8Array> | null;
	readonly headers: object;
	arrayBuffer(): Promise<ArrayBuffer>;
}

/** A fetch Response or Request (duck-typed so that internal:fetch is not loaded). */
export function isBody(value: unknown): value is BodyLike {
	return (
		value !== null &&
		typeof value === "object" &&
		"body" in value &&
		typeof (value as BodyLike).headers === "object" &&
		typeof (value as BodyLike).arrayBuffer === "function"
	);
}

export function isReadableStream(value: unknown): value is ReadableStream<unknown> {
	return value !== null && typeof value === "object" && typeof (value as ReadableStream).getReader === "function";
}

/**
 * Open a Bun.file() for a child's stdio: a descriptor the parent closes after
 * spawning (`owned`), or the file's own descriptor (Bun.file(fd)).
 */
export function openBunFile(file: BunFileLike, write: boolean): { fd: number; owned: boolean } {
	const target = bunFileTarget(file);
	if ("fd" in target) return { fd: target.fd, owned: false };
	return { fd: openSync(target.path, write ? "w" : "r", 0o666), owned: true };
}

/** The path or descriptor behind a Bun.file(). */
export function bunFileTarget(file: BunFileLike): { path: string } | { fd: number } {
	const name = file.name;
	if (typeof name === "number") return { fd: name };
	const fd = file[kBunFileFd];
	if (typeof fd === "number" && fd >= 0) return { fd };
	if (typeof name !== "string" || name.length === 0)
		throw invalidArgType("file", "a Bun.file() with a path or a file descriptor", file);
	return { path: name };
}

export function closeFd(fd: number): void {
	try {
		closeSync(fd);
	} catch {
		// already closed
	}
}

// ---- pipe I/O ----

/**
 * Read `handle` to the end: `onChunk` per chunk, then `onEnd` once (with the
 * read error, if any). The handle is closed at the end.
 */
export function readPipe(
	handle: StreamHandle,
	onChunk: (chunk: Uint8Array) => void,
	onEnd: (err: Error | null) => void,
): void {
	handle.onread = (chunk, err) => {
		if (chunk) {
			onChunk(chunk);
			return;
		}
		handle.onread = null;
		handle.close();
		onEnd(err ?? null);
	};
	handle.reading = true;
	handle.readStart();
}

export function writePipe(handle: StreamHandle, data: Uint8Array | string): Promise<void> {
	const { promise, resolve, reject } = Promise.withResolvers<void>();
	handle.write(data, (err) => (err ? reject(err) : resolve()));
	return promise;
}

export function shutdownPipe(handle: StreamHandle): Promise<void> {
	const { promise, resolve } = Promise.withResolvers<void>();
	// a failed shutdown (the child closed its end) changes nothing for the writer
	handle.shutdown(() => resolve());
	return promise;
}

/** Write every chunk of `stream` to `handle`. */
async function pumpStream(handle: StreamHandle, stream: ReadableStream<unknown>): Promise<void> {
	const reader = stream.getReader();
	try {
		for (;;) {
			const { done, value } = await reader.read();
			if (done) break;
			if (typeof value === "string") {
				if (value.length) await writePipe(handle, value);
				continue;
			}
			const bytes = bytesOf(value);
			if (!bytes) throw invalidArgType("chunk", "a string, ArrayBuffer or ArrayBufferView", value);
			if (bytes.byteLength) await writePipe(handle, bytes);
		}
	} catch (err) {
		await reader.cancel(err).catch(() => {});
		throw err;
	} finally {
		reader.releaseLock();
	}
}

/**
 * Feed `source` (string, bytes, Blob, Response/Request body or ReadableStream)
 * to the child through `handle`, then close it. A child that stops reading
 * (EPIPE) just ends the transfer.
 */
export async function pumpInput(handle: StreamHandle, source: unknown): Promise<void> {
	try {
		if (typeof source === "string") {
			if (source.length) await writePipe(handle, source);
		} else {
			const bytes = bytesOf(source);
			if (bytes) {
				if (bytes.byteLength) await writePipe(handle, bytes);
			} else if (isBody(source)) {
				if (source.body) await pumpStream(handle, source.body);
			} else if (isBlob(source)) {
				await pumpStream(handle, source.stream());
			} else if (isReadableStream(source)) {
				await pumpStream(handle, source);
			} else {
				throw invalidArgType(
					"stdin",
					"a string, ArrayBuffer, ArrayBufferView, Blob, Response, Request or ReadableStream",
					source,
				);
			}
		}
		await shutdownPipe(handle);
	} catch (err) {
		const code = (err as { code?: unknown } | null)?.code;
		if (code !== "EPIPE" && code !== "ECONNRESET" && code !== "ECANCELED") throw err;
	} finally {
		handle.close();
	}
}

// ---- errors ----

export function abortError(reason: unknown): Error & { code: string } {
	const err = new Error("The operation was aborted", reason === undefined ? undefined : { cause: reason }) as Error & {
		code: string;
	};
	err.name = "AbortError";
	err.code = "ABORT_ERR";
	return err;
}
