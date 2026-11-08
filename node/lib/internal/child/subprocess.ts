// internal:child/subprocess — Bun.spawn / Bun.spawnSync / Bun.which on the
// qbun:process binding (see internal:bun/spawn for the public group module).
//
// stdout/stderr pipes are read eagerly (like Bun) and exposed as a
// ReadableStream<Uint8Array> created on first access; stdin "pipe" is a
// FileSink-like writer.

import { getBlobBytes } from "node:buffer";
import type { Timeout } from "node:timers";
import { codedError, invalidArgType, invalidArgValue, notSupported } from "internal:errors";
import { bind } from "internal:async-context";
import {
	abortError,
	bytesOf,
	closeFd,
	convertToValidSignal,
	createPipe,
	envPairs,
	envPairsGet,
	errnoException,
	errnoName,
	isBlob,
	isBody,
	isBunFile,
	isReadableStream,
	nativeErrno,
	openBunFile,
	processNative,
	pumpInput,
	readPipe,
	sanitizeKillSignal,
	signals,
	toBuffer,
	which as whichPath,
	type NativeResourceUsage,
	type NativeSpawnSyncResult,
	type NodeBuffer,
	type ProcessHandle,
	type StdioSpec,
	type StreamHandle,
	type SyncStdio,
} from "internal:child/common";
import { Channel, validateSerialization, type Serialization } from "internal:child/ipc";

type Env = Record<string, string | undefined>;

export interface ResourceUsage {
	contextSwitches: { voluntary: number; involuntary: number };
	cpuTime: { user: bigint; system: bigint; total: bigint };
	maxRSS: number;
	messages: { sent: number; received: number };
	ops: { in: number; out: number };
	shmSize: number;
	signalCount: number;
	swapCount: number;
}

/** Bun's ResourceUsage from Node-style rusage fields (maxRSS in KiB, CPU times in µs). */
function toResourceUsage(r: NativeResourceUsage | undefined): ResourceUsage {
	const n = (key: string): number => (r && typeof r[key] === "number" ? r[key] : 0);
	const user = BigInt(Math.trunc(n("userCPUTime")));
	const system = BigInt(Math.trunc(n("systemCPUTime")));
	return {
		contextSwitches: { voluntary: n("voluntaryContextSwitches"), involuntary: n("involuntaryContextSwitches") },
		cpuTime: { user, system, total: user + system },
		maxRSS: n("maxRSS") * 1024,
		messages: { sent: n("ipcSent"), received: n("ipcReceived") },
		ops: { in: n("fsRead"), out: n("fsWrite") },
		shmSize: n("sharedMemorySize"),
		signalCount: n("signalsCount"),
		swapCount: n("swappedOut"),
	};
}

// ---- options ----

type StdioValue = unknown;

export interface SpawnOptions {
	cmd?: readonly string[];
	cwd?: string;
	env?: Env;
	stdio?: readonly StdioValue[];
	stdin?: StdioValue;
	stdout?: StdioValue;
	stderr?: StdioValue;
	onExit?: (
		subprocess: Subprocess,
		exitCode: number | null,
		signalCode: string | null,
		error?: Error,
	) => void | Promise<void>;
	onDisconnect?: () => void | Promise<void>;
	ipc?: (message: unknown, subprocess: Subprocess, handle?: unknown) => void;
	serialization?: Serialization;
	detached?: boolean;
	uid?: number;
	gid?: number;
	cgroup?: string | number;
	windowsHide?: boolean;
	windowsVerbatimArguments?: boolean;
	argv0?: string;
	signal?: AbortSignal;
	timeout?: number;
	killSignal?: string | number;
	maxBuffer?: number;
	lazy?: boolean;
	terminal?: unknown;
}

function normalizeArgs(cmdOrOptions: unknown, maybeOptions: unknown): { cmd: string[]; options: SpawnOptions } {
	let cmd: unknown;
	let options: SpawnOptions;
	if (Array.isArray(cmdOrOptions)) {
		cmd = cmdOrOptions;
		if (maybeOptions != null && typeof maybeOptions !== "object")
			throw invalidArgType("options", "of type object", maybeOptions);
		options = (maybeOptions ?? {}) as SpawnOptions;
	} else if (cmdOrOptions !== null && typeof cmdOrOptions === "object") {
		options = cmdOrOptions as SpawnOptions;
		cmd = options.cmd;
	} else {
		throw invalidArgType("cmd", "an array of strings", cmdOrOptions);
	}
	if (!Array.isArray(cmd)) throw invalidArgType("cmd", "an array of strings", cmd);
	if (cmd.length === 0) throw invalidArgValue("cmd", cmd, "must not be empty");
	const argv = cmd.map((arg, i) => {
		const s = typeof arg === "string" ? arg : String(arg);
		if (s.includes("\u0000")) throw invalidArgValue(`cmd[${i}]`, s, "must be a string without null bytes");
		return s;
	});
	if (argv[0].length === 0) throw invalidArgValue("cmd[0]", argv[0], "must not be empty");
	return { cmd: argv, options };
}

interface Prepared {
	file: string;
	args: string[];
	cwd: string;
	env: string[];
}

function notFound(command: string): Error {
	const err = errnoException(-2, "posix_spawn") as Error & { path?: string };
	err.message = `Executable not found in $PATH: "${command}"`;
	err.path = command;
	return err;
}

function prepare(cmd: string[], options: SpawnOptions): Prepared {
	let cwd: string;
	if (options.cwd === undefined || options.cwd === null) cwd = process.cwd();
	else if (typeof options.cwd !== "string") throw invalidArgType("cwd", "of type string", options.cwd);
	else cwd = options.cwd;
	if (options.env != null && typeof options.env !== "object")
		throw invalidArgType("env", "of type object", options.env);
	const env = envPairs((options.env ?? process.env) as Record<string, unknown>);
	if (options.argv0 != null && typeof options.argv0 !== "string")
		throw invalidArgType("argv0", "of type string", options.argv0);
	let file = cmd[0];
	if (!file.includes("/")) {
		const found = whichPath(file, envPairsGet(env, "PATH") ?? process.env.PATH, cwd);
		if (found === null) throw notFound(file);
		file = found;
	}
	const args = cmd.slice();
	args[0] = options.argv0 ?? cmd[0];
	return { file, args, cwd, env };
}

function validateIdOption(value: unknown, name: string): number | undefined {
	if (value == null) return undefined;
	if (typeof value !== "number" || !Number.isInteger(value)) throw invalidArgType(name, "an integer", value);
	return value;
}

// ---- stdio resolution ----

type Io =
	| { kind: "pipe" }
	| { kind: "inherit" }
	| { kind: "ignore" }
	| { kind: "fd"; fd: number; owned: boolean }
	| { kind: "input"; source: unknown }
	| { kind: "buffer"; target: Uint8Array };

function resolveIo(value: unknown, index: number, defaultKind: "pipe" | "inherit" | "ignore"): Io {
	if (value === undefined) return { kind: defaultKind };
	if (value === null || value === "ignore") return { kind: "ignore" };
	if (value === "pipe" || value === "overlapped" || (value === "socket-fd" && index >= 3)) return { kind: "pipe" };
	if (value === "inherit") return { kind: "inherit" };
	if (typeof value === "number") {
		if (!Number.isInteger(value) || value < 0)
			throw invalidArgValue(`stdio[${index}]`, value, "must be a valid file descriptor");
		return { kind: "fd", fd: value, owned: false };
	}
	if (isBunFile(value)) {
		const { fd, owned } = openBunFile(value, index !== 0);
		return { kind: "fd", fd, owned };
	}
	if (index === 0) {
		if (typeof value === "string" || bytesOf(value) || isBlob(value) || isBody(value) || isReadableStream(value))
			return { kind: "input", source: value };
	} else {
		const target = bytesOf(value);
		if (target) return { kind: "buffer", target };
	}
	throw invalidArgValue(
		`stdio[${index}]`,
		value,
		index === 0
			? "must be 'pipe', 'inherit', 'ignore', null, a file descriptor, a Bun.file(), an ArrayBufferView, a Blob, a Response, a Request or a ReadableStream"
			: "must be 'pipe', 'inherit', 'ignore', null, a file descriptor, a Bun.file() or an ArrayBufferView",
	);
}

function stdioValues(options: SpawnOptions): StdioValue[] {
	let values: StdioValue[];
	if (options.stdio != null) {
		if (!Array.isArray(options.stdio)) throw invalidArgType("stdio", "an array", options.stdio);
		values = options.stdio.slice();
	} else values = [];
	while (values.length < 3) values.push(undefined);
	if (options.stdio == null) {
		values[0] = options.stdin;
		values[1] = options.stdout;
		values[2] = options.stderr;
	}
	return values;
}

function closeOwned(ios: Io[]): void {
	for (const io of ios) if (io.kind === "fd" && io.owned) closeFd(io.fd);
}

// ---- stdin writer (FileSink) ----

/** Bun's FileSink over a pipe to the child's stdin. */
export class PipeSink {
	#handle: StreamHandle | null;
	#pending = 0;
	#pendingBytes = 0;
	#error: Error | null = null;
	#waiters: { resolve(n: number): void; reject(e: unknown): void }[] = [];
	#flushedBytes = 0;
	#ended = false;

	constructor(handle: StreamHandle) {
		this.#handle = handle;
	}

	#settle(): void {
		if (this.#pending !== 0 && this.#error === null) return;
		const waiters = this.#waiters.splice(0, this.#waiters.length);
		const flushed = this.#flushedBytes;
		this.#flushedBytes = 0;
		for (const w of waiters) {
			if (this.#error) w.reject(this.#error);
			else w.resolve(flushed);
		}
	}

	write(chunk: string | ArrayBufferView | ArrayBuffer | SharedArrayBuffer): number {
		if (this.#error) throw this.#error;
		if (this.#ended || !this.#handle)
			throw codedError("ERR_INVALID_STATE", "Invalid state: FileSink is closed", TypeError);
		let data: string | Uint8Array;
		let length: number;
		if (typeof chunk === "string") {
			data = chunk;
			length = Buffer.byteLength(chunk, "utf8");
		} else {
			const bytes = bytesOf(chunk);
			if (!bytes) throw invalidArgType("chunk", "a string, ArrayBuffer or ArrayBufferView", chunk);
			// the caller may reuse its buffer once write() returns
			data = bytes.slice();
			length = data.byteLength;
		}
		if (length === 0) return 0;
		this.#pending++;
		this.#pendingBytes += length;
		this.#handle.write(data, (err) => {
			this.#pending--;
			this.#pendingBytes -= length;
			if (err) {
				const code = (err as { code?: unknown }).code;
				this.#error ??= err;
				if (code === "EPIPE" || code === "ECANCELED") this.#ended = true;
			} else {
				this.#flushedBytes += length;
			}
			this.#settle();
		});
		return length;
	}

	flush(): number | Promise<number> {
		if (this.#error) return Promise.reject(this.#error);
		if (this.#pending === 0) {
			const n = this.#flushedBytes;
			this.#flushedBytes = 0;
			return n;
		}
		const { promise, resolve, reject } = Promise.withResolvers<number>();
		this.#waiters.push({ resolve, reject });
		return promise;
	}

	end(error?: Error): number | Promise<number> {
		if (this.#ended || !this.#handle) return 0;
		this.#ended = true;
		const handle = this.#handle;
		const finish = (n: number): Promise<number> => {
			const { promise, resolve } = Promise.withResolvers<number>();
			this.#handle = null;
			if (error) {
				handle.close();
				resolve(n);
				return promise;
			}
			handle.shutdown(() => {
				handle.close();
				resolve(n);
			});
			return promise;
		};
		const flushed = this.flush();
		if (typeof flushed === "number") return finish(flushed);
		return flushed.then(finish, (err) => {
			this.#handle = null;
			handle.close();
			throw err;
		});
	}

	/** Bun's FileSink.start(): no-op for a pipe (no internal buffering here). */
	start(_options?: { highWaterMark?: number }): void {}

	ref(): void {
		this.#handle?.ref();
	}

	unref(): void {
		this.#handle?.unref();
	}

	/** The pipe closed by the subprocess machinery (process gone). */
	_close(): void {
		if (!this.#handle) return;
		if (this.#pending === 0) {
			this.#handle.close();
			this.#handle = null;
		}
	}
}

// ---- stdout/stderr readers ----

class OutputPipe {
	#handle: StreamHandle;
	#chunks: Uint8Array[] = [];
	#started = false;
	#ended = false;
	#closed = false;
	#error: Error | null = null;
	#controller: ReadableStreamDefaultController<Uint8Array> | null = null;
	#stream: ReadableStream<Uint8Array> | undefined;
	#onBytes: (n: number) => void;

	constructor(handle: StreamHandle, lazy: boolean, onBytes: (n: number) => void) {
		this.#handle = handle;
		this.#onBytes = onBytes;
		if (!lazy) this.#start();
	}

	#start(): void {
		if (this.#started) return;
		this.#started = true;
		readPipe(
			this.#handle,
			bind((chunk: Uint8Array) => {
				this.#onBytes(chunk.byteLength);
				if (this.#controller) this.#controller.enqueue(chunk);
				else this.#chunks.push(chunk);
			}),
			bind((err: Error | null) => {
				this.#ended = true;
				this.#closed = true;
				this.#error = err;
				if (this.#controller) {
					if (err) this.#controller.error(err);
					else this.#controller.close();
				}
			}),
		);
	}

	get stream(): ReadableStream<Uint8Array> {
		if (this.#stream) return this.#stream;
		this.#start();
		this.#stream = new ReadableStream<Uint8Array>({
			start: (controller) => {
				this.#controller = controller;
				for (const chunk of this.#chunks) controller.enqueue(chunk);
				this.#chunks = [];
				if (this.#ended) {
					if (this.#error) controller.error(this.#error);
					else controller.close();
				}
			},
			cancel: () => {
				this.#controller = null;
				this.close();
			},
		});
		return this.#stream;
	}

	close(): void {
		if (this.#closed) return;
		this.#closed = true;
		this.#handle.onread = null;
		this.#handle.readStop();
		this.#handle.close();
		this.#ended = true;
	}

	ref(): void {
		if (!this.#closed) this.#handle.ref();
	}

	unref(): void {
		if (!this.#closed) this.#handle.unref();
	}
}

/** Read a pipe into a caller-provided buffer (stdout: ArrayBufferView). */
function readIntoBuffer(handle: StreamHandle, target: Uint8Array): void {
	let offset = 0;
	readPipe(
		handle,
		(chunk) => {
			if (offset >= target.byteLength) return;
			const n = Math.min(chunk.byteLength, target.byteLength - offset);
			target.set(n === chunk.byteLength ? chunk : chunk.subarray(0, n), offset);
			offset += n;
		},
		() => {},
	);
}

// ---- Subprocess ----

export class Subprocess {
	readonly pid: number;
	#handle: ProcessHandle | null = null;
	#stdin: PipeSink | number | undefined;
	#stdout: OutputPipe | number | undefined;
	#stderr: OutputPipe | number | undefined;
	#extra: (StreamHandle | number | null)[];
	#exitCode: number | null = null;
	#signalCode: string | null = null;
	#killed = false;
	#exited: Promise<number>;
	#resolveExited: (code: number) => void;
	#rusage: ResourceUsage | undefined;
	#channel: Channel | null = null;
	#onExit: SpawnOptions["onExit"];
	#cleanup: (() => void)[] = [];
	#killSignal: number;
	#maxBuffer: number;
	#outputBytes = 0;

	/** @internal use Bun.spawn() */
	constructor(cmd: string[], options: SpawnOptions) {
		if (options.terminal !== undefined) throw notSupported("Bun.spawn({ terminal }) (pseudo-terminals)");
		const killSignal = sanitizeKillSignal(options.killSignal, "killSignal") ?? signals.SIGTERM;
		this.#killSignal = killSignal;
		const signal = options.signal;
		if (signal !== undefined && (signal === null || typeof signal !== "object" || !("aborted" in signal))) {
			throw invalidArgType("signal", "an instance of AbortSignal", signal);
		}
		if (signal?.aborted) throw abortError(signal.reason);
		if (
			options.timeout != null &&
			(typeof options.timeout !== "number" || options.timeout < 0 || Number.isNaN(options.timeout))
		) {
			throw invalidArgType("timeout", "a non-negative number", options.timeout);
		}
		if (
			options.maxBuffer != null &&
			(typeof options.maxBuffer !== "number" || options.maxBuffer < 0 || Number.isNaN(options.maxBuffer))
		) {
			throw invalidArgType("maxBuffer", "a non-negative number", options.maxBuffer);
		}
		this.#maxBuffer = options.maxBuffer ?? Infinity;
		if (options.onExit != null && typeof options.onExit !== "function")
			throw invalidArgType("onExit", "of type function", options.onExit);
		if (options.ipc != null && typeof options.ipc !== "function")
			throw invalidArgType("ipc", "of type function", options.ipc);
		this.#onExit = options.onExit ?? undefined;
		const uid = validateIdOption(options.uid, "uid");
		const gid = validateIdOption(options.gid, "gid");
		const serialization = options.ipc
			? validateSerialization(options.serialization ?? "advanced", "serialization")
			: "advanced";

		const { promise, resolve } = Promise.withResolvers<number>();
		this.#exited = promise;
		this.#resolveExited = resolve;

		const prepared = prepare(cmd, options);
		const values = stdioValues(options);
		const ios: Io[] = [];
		try {
			for (let i = 0; i < values.length; i++)
				ios.push(resolveIo(values[i], i, i === 1 ? "pipe" : i === 2 ? "inherit" : "ignore"));
		} catch (e) {
			closeOwned(ios);
			throw e;
		}

		const handles: (StreamHandle | null)[] = [];
		const native: StdioSpec[] = ios.map((io, i): StdioSpec => {
			handles[i] = null;
			switch (io.kind) {
				case "pipe":
				case "input":
				case "buffer": {
					const handle = createPipe(false);
					handles[i] = handle;
					return { type: "pipe", handle, readable: i === 0, writable: i !== 0 };
				}
				case "inherit":
					return { type: "inherit", fd: i };
				case "fd":
					return { type: "fd", fd: io.fd };
				default:
					return { type: "ignore" };
			}
		});
		let ipcHandle: StreamHandle | null = null;
		if (options.ipc) {
			ipcHandle = createPipe(true);
			const ipcFd = native.length;
			native.push({ type: "pipe", handle: ipcHandle, readable: true, writable: true });
			prepared.env.push(`NODE_CHANNEL_FD=${ipcFd}`, `NODE_CHANNEL_SERIALIZATION_MODE=${serialization}`);
		}

		try {
			this.#handle = processNative.spawn(
				{
					file: prepared.file,
					args: prepared.args,
					cwd: prepared.cwd,
					env: prepared.env,
					stdio: native,
					detached: !!options.detached,
					uid,
					gid,
				},
				bind((exitCode: number, signalCode: string | null, rusage?: NativeResourceUsage) =>
					this.#onexit(exitCode, signalCode, rusage),
				),
			);
		} catch (e) {
			for (const h of handles) h?.close();
			ipcHandle?.close();
			throw e;
		} finally {
			closeOwned(ios);
		}
		this.pid = this.#handle.pid;

		const lazy = !!options.lazy;
		const onBytes = (n: number): void => {
			this.#outputBytes += n;
			if (this.#outputBytes > this.#maxBuffer) this.kill(this.#killSignal);
		};
		this.#extra = [];
		for (let i = 0; i < ios.length; i++) {
			const io = ios[i];
			const handle = handles[i];
			if (i === 0) {
				if (io.kind === "pipe") this.#stdin = new PipeSink(handle!);
				else if (io.kind === "input") void pumpInput(handle!, io.source).catch(() => {});
				else if (io.kind === "fd") this.#stdin = io.fd;
			} else if (i <= 2) {
				let value: OutputPipe | number | undefined;
				if (io.kind === "pipe") value = new OutputPipe(handle!, lazy, onBytes);
				else if (io.kind === "buffer") readIntoBuffer(handle!, io.target);
				else if (io.kind === "fd") value = io.fd;
				if (i === 1) this.#stdout = value;
				else this.#stderr = value;
			} else {
				this.#extra[i - 3] = io.kind === "pipe" ? handle : io.kind === "fd" ? io.fd : null;
				if (io.kind === "buffer") readIntoBuffer(handle!, io.target);
			}
		}

		if (ipcHandle) {
			const onMessage = options.ipc!;
			const onDisconnect = options.onDisconnect;
			this.#channel = new Channel(ipcHandle, serialization, {
				onMessage: (message, internal) => {
					if (!internal) onMessage(message, this);
				},
				onDisconnect: () => {
					this.#channel = null;
					if (typeof onDisconnect === "function") void onDisconnect();
				},
				onError: () => {},
			});
		}

		if (options.timeout != null && options.timeout > 0) {
			const timer: Timeout = setTimeout(() => this.kill(this.#killSignal), options.timeout);
			timer.unref();
			this.#cleanup.push(() => clearTimeout(timer));
		}
		if (signal) {
			const onAbort = (): void => this.kill(this.#killSignal);
			signal.addEventListener("abort", onAbort, { once: true });
			this.#cleanup.push(() => signal.removeEventListener("abort", onAbort));
		}
	}

	#onexit(exitCode: number, signalCode: string | null, rusage?: NativeResourceUsage): void {
		if (signalCode) this.#signalCode = signalCode;
		else this.#exitCode = exitCode;
		this.#rusage = toResourceUsage(rusage);
		if (this.#handle) {
			this.#handle.close();
			this.#handle = null;
		}
		for (const fn of this.#cleanup) fn();
		this.#cleanup = [];
		if (this.#stdin instanceof PipeSink) this.#stdin._close();
		const code = signalCode ? 128 + (signals[signalCode] ?? 126) : exitCode;
		try {
			if (this.#onExit) void this.#onExit(this, this.#exitCode, this.#signalCode);
		} finally {
			this.#resolveExited(code);
		}
	}

	get stdin(): PipeSink | number | undefined {
		return this.#stdin;
	}

	get stdout(): ReadableStream<Uint8Array> | number | undefined {
		const v = this.#stdout;
		return v instanceof OutputPipe ? v.stream : v;
	}

	get stderr(): ReadableStream<Uint8Array> | number | undefined {
		const v = this.#stderr;
		return v instanceof OutputPipe ? v.stream : v;
	}

	/** Same as stdout (for pipeThrough). */
	get readable(): ReadableStream<Uint8Array> | number | undefined {
		return this.stdout;
	}

	get stdio(): (number | null)[] {
		const out: (number | null)[] = [null, null, null];
		for (const v of this.#extra) out.push(v === null || v === undefined ? null : typeof v === "number" ? v : v.fd);
		return out;
	}

	get terminal(): undefined {
		return undefined;
	}

	get exited(): Promise<number> {
		return this.#exited;
	}

	get exitCode(): number | null {
		return this.#exitCode;
	}

	get signalCode(): string | null {
		return this.#signalCode;
	}

	get killed(): boolean {
		return this.#killed;
	}

	get connected(): boolean {
		return this.#channel?.connected ?? false;
	}

	kill(signal?: number | string): void {
		const signum = signal === undefined ? signals.SIGTERM : signal === 0 ? 0 : convertToValidSignal(signal);
		if (!this.#handle) return;
		const err = this.#handle.kill(signum);
		if (err === 0) {
			this.#killed = true;
			return;
		}
		if (errnoName(err) === "ESRCH") return;
		throw errnoException(err, "kill");
	}

	ref(): void {
		this.#handle?.ref();
		this.#channel?.ref();
		if (this.#stdin instanceof PipeSink) this.#stdin.ref();
		if (this.#stdout instanceof OutputPipe) this.#stdout.ref();
		if (this.#stderr instanceof OutputPipe) this.#stderr.ref();
	}

	unref(): void {
		this.#handle?.unref();
		this.#channel?.unref();
		if (this.#stdin instanceof PipeSink) this.#stdin.unref();
		if (this.#stdout instanceof OutputPipe) this.#stdout.unref();
		if (this.#stderr instanceof OutputPipe) this.#stderr.unref();
	}

	send(message: unknown): boolean {
		const channel = this.#channel;
		if (!channel || !channel.connected) throw codedError("ERR_IPC_CHANNEL_CLOSED", "Channel closed");
		return channel.sendInternal(message, undefined, true);
	}

	disconnect(): void {
		const channel = this.#channel;
		if (!channel || !channel.connected) return;
		channel.disconnect();
	}

	resourceUsage(): ResourceUsage | undefined {
		return this.#rusage;
	}

	async [Symbol.asyncDispose](): Promise<void> {
		if (this.#exitCode === null && this.#signalCode === null) this.kill();
		await this.#exited;
	}
}

export function spawn(
	cmdOrOptions: readonly string[] | (SpawnOptions & { cmd: readonly string[] }),
	options?: SpawnOptions,
): Subprocess {
	const { cmd, options: opts } = normalizeArgs(cmdOrOptions, options);
	return new Subprocess(cmd, opts);
}

// ---- spawnSync ----

export interface SyncSubprocess {
	stdout: NodeBuffer | undefined;
	stderr: NodeBuffer | undefined;
	exitCode: number | null;
	success: boolean;
	resourceUsage: ResourceUsage;
	signalCode?: string;
	exitedDueToTimeout?: boolean;
	exitedDueToMaxBuffer?: boolean;
	pid: number;
}

function syncInput(value: unknown): Uint8Array {
	if (typeof value === "string") return Buffer.from(value);
	const bytes = bytesOf(value);
	if (bytes) return bytes;
	if (isBlob(value)) return getBlobBytes(value);
	throw invalidArgType("stdin", "a string, ArrayBufferView or Blob for spawnSync", value);
}

export function spawnSync(
	cmdOrOptions: readonly string[] | (SpawnOptions & { cmd: readonly string[] }),
	options?: SpawnOptions,
): SyncSubprocess {
	const { cmd, options: opts } = normalizeArgs(cmdOrOptions, options);
	const killSignal = sanitizeKillSignal(opts.killSignal, "killSignal");
	const uid = validateIdOption(opts.uid, "uid");
	const gid = validateIdOption(opts.gid, "gid");
	const prepared = prepare(cmd, opts);
	const values = stdioValues(opts);
	const ios: Io[] = [];
	try {
		for (let i = 0; i < values.length; i++) ios.push(resolveIo(values[i], i, i === 0 ? "ignore" : "pipe"));
	} catch (e) {
		closeOwned(ios);
		throw e;
	}
	let input: Uint8Array | undefined;
	const stdio: SyncStdio[] = ios.map((io, i): SyncStdio => {
		switch (io.kind) {
			case "pipe":
				return "pipe";
			case "input":
				input = syncInput(io.source);
				return "pipe";
			case "buffer":
				if (i !== 0) return "pipe";
				return "ignore";
			case "inherit":
				return "inherit";
			case "fd":
				return io.fd;
			default:
				return "ignore";
		}
	});

	let result: NativeSpawnSyncResult;
	try {
		result = processNative.spawnSync({
			file: prepared.file,
			args: prepared.args,
			cwd: prepared.cwd,
			env: prepared.env,
			stdio,
			input,
			timeout: opts.timeout ?? 0,
			killSignal,
			maxBuffer: opts.maxBuffer ?? undefined,
			detached: !!opts.detached,
			uid,
			gid,
		});
	} finally {
		closeOwned(ios);
	}

	let exitedDueToTimeout = false;
	let exitedDueToMaxBuffer = false;
	if (result.error) {
		const errno = nativeErrno(result.error);
		const name = errno !== undefined ? errnoName(errno) : undefined;
		if (name === "ETIMEDOUT") exitedDueToTimeout = true;
		else if (name === "ENOBUFS") exitedDueToMaxBuffer = true;
		else throw result.error;
	}

	const output = (i: number): NodeBuffer | undefined => {
		const chunk = result.output?.[i];
		const io = ios[i];
		if (!chunk) return io.kind === "pipe" ? Buffer.alloc(0) : undefined;
		const buf = toBuffer(chunk);
		if (io.kind === "buffer") {
			io.target.set(buf.byteLength > io.target.byteLength ? buf.subarray(0, io.target.byteLength) : buf);
			return undefined;
		}
		return io.kind === "pipe" ? buf : undefined;
	};
	const exitCode = result.signal ? null : result.status;
	const ret: SyncSubprocess = {
		stdout: output(1),
		stderr: output(2),
		exitCode,
		success: exitCode === 0 && !result.signal,
		resourceUsage: toResourceUsage(result.resourceUsage),
		pid: result.pid,
	};
	if (result.signal) ret.signalCode = result.signal;
	if (exitedDueToTimeout) ret.exitedDueToTimeout = true;
	if (exitedDueToMaxBuffer) ret.exitedDueToMaxBuffer = true;
	return ret;
}

// ---- which ----

export function which(command: string, options?: { PATH?: string; cwd?: string } | null): string | null {
	if (typeof command !== "string") throw invalidArgType("command", "of type string", command);
	if (options != null && typeof options !== "object") throw invalidArgType("options", "of type object", options);
	const PATH = options?.PATH ?? process.env.PATH;
	const cwd = options?.cwd ?? process.cwd();
	return whichPath(command, PATH, cwd);
}
