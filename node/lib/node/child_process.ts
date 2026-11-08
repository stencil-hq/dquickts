// node:child_process — spawn/exec/execFile/fork and their sync forms, on the
// qbun:process binding (libuv uv_spawn). Ported from Node's lib/child_process.js
// and lib/internal/child_process.js.
//
// PATH lookup is libuv's: a file without a slash is searched in the child's
// PATH. `shell: true` runs '/bin/sh -c'. IPC (fork(), 'ipc' stdio) uses
// internal:child/ipc; handles cannot be sent over the channel.

import process from "node:process";
import type { Timeout } from "node:timers";
import { EventEmitter } from "node:events";
import { Socket } from "node:net";
import { Buffer } from "node:buffer";
import {
	codedError,
	invalidArgType,
	invalidArgValue,
	outOfRange,
	validateFunction,
	validateObject,
	validateString,
	type CodedError,
} from "internal:errors";
import { bind } from "internal:async-context";
import {
	convertToValidSignal,
	createPipe,
	envPairs,
	errnoException,
	errnoName,
	nativeErrno,
	processNative,
	sanitizeKillSignal,
	type ErrnoError,
	type NativeSpawnOptions,
	type ProcessHandle,
	type StdioSpec,
	type StreamHandle,
	type SyncStdio,
	abortError,
} from "internal:child/common";
import {
	setupNodeChannel,
	validateSerialization,
	_forkChild as forkChildChannel,
	type Channel,
	type NodeChannelTarget,
} from "internal:child/ipc";

const MAX_BUFFER = 1024 * 1024;
const promisifyCustom = Symbol.for("nodejs.util.promisify.custom");
const kIsUsedAsStdio = Symbol("kIsUsedAsStdio");

type Encoding = BufferEncoding | "buffer" | null | undefined;
type BufferEncoding =
	| "ascii"
	| "utf8"
	| "utf-8"
	| "utf16le"
	| "utf-16le"
	| "ucs2"
	| "ucs-2"
	| "base64"
	| "base64url"
	| "latin1"
	| "binary"
	| "hex";
type StdioOption =
	| "pipe"
	| "overlapped"
	| "ignore"
	| "inherit"
	| "ipc"
	| number
	| null
	| undefined
	| { fd?: number; _handle?: unknown };
type Serializable = string | number | boolean | object;
type SendHandle = unknown;
type MessageCallback = (error: Error | null) => void;

export interface SpawnOptions {
	cwd?: string | URL;
	env?: Record<string, string | undefined>;
	argv0?: string;
	stdio?: string | StdioOption[];
	detached?: boolean;
	uid?: number;
	gid?: number;
	serialization?: "json" | "advanced";
	shell?: boolean | string;
	windowsVerbatimArguments?: boolean;
	windowsHide?: boolean;
	signal?: AbortSignal;
	timeout?: number;
	killSignal?: string | number;
}

interface NormalizedSpawnOptions extends Omit<SpawnOptions, "cwd" | "stdio"> {
	file: string;
	args: string[];
	cwd?: string;
	envPairs: string[];
	stdio?: string | StdioOption[];
	detached: boolean;
}

// ---- validation helpers ----

function validateArgumentNullCheck(arg: unknown, propName: string): void {
	if (typeof arg === "string" && arg.includes("\u0000")) {
		throw invalidArgValue(propName, arg, "must be a string without null bytes");
	}
}

function validateArgumentsNullCheck(args: readonly unknown[], propName: string): void {
	for (let i = 0; i < args.length; ++i) validateArgumentNullCheck(args[i], `${propName}[${i}]`);
}

function validateBoolean(value: unknown, name: string): void {
	if (typeof value !== "boolean") throw invalidArgType(name, "of type boolean", value);
}

function validateInt32(value: unknown, name: string): void {
	if (typeof value !== "number") throw invalidArgType(name, "of type number", value);
	if (!Number.isInteger(value)) throw outOfRange(name, "an integer", value);
	if (value < -2147483648 || value > 2147483647) throw outOfRange(name, ">= -2147483648 && <= 2147483647", value);
}

function validateTimeout(timeout: unknown): void {
	if (timeout == null) return;
	if (typeof timeout !== "number") throw invalidArgType("timeout", "of type number", timeout);
	if (!Number.isInteger(timeout)) throw outOfRange("timeout", "an integer", timeout);
	if (timeout < 0 || timeout > Number.MAX_SAFE_INTEGER)
		throw outOfRange("timeout", `>= 0 && <= ${Number.MAX_SAFE_INTEGER}`, timeout);
}

function validateMaxBuffer(maxBuffer: unknown): void {
	if (maxBuffer == null) return;
	if (typeof maxBuffer !== "number") throw invalidArgType("options.maxBuffer", "of type number", maxBuffer);
	if (Number.isNaN(maxBuffer) || maxBuffer < 0) throw outOfRange("options.maxBuffer", ">= 0", maxBuffer);
}

function validateAbortSignal(signal: unknown, name: string): void {
	if (signal !== undefined && (signal === null || typeof signal !== "object" || !("aborted" in signal))) {
		throw invalidArgType(name, "an instance of AbortSignal", signal);
	}
}

/** Node's getValidatedPath for options.cwd / modulePath: string, Buffer or file: URL. */
function getValidatedPath(value: unknown, name: string): string {
	let path: string;
	if (typeof value === "string") path = value;
	else if (value instanceof Uint8Array) path = Buffer.from(value).toString();
	else if (value instanceof URL) {
		if (value.protocol !== "file:")
			throw codedError("ERR_INVALID_URL_SCHEME", "The URL must be of scheme file", TypeError);
		path = decodeURIComponent(value.pathname);
	} else throw invalidArgType(name, "of type string or an instance of Buffer or URL", value);
	if (path.includes("\u0000")) {
		throw codedError(
			"ERR_INVALID_ARG_VALUE",
			`The argument '${name}' must be a string, Uint8Array, or URL without null bytes. Received ${JSON.stringify(path)}`,
			TypeError,
		);
	}
	return path;
}

/** Node's genericNodeError: an Error carrying `props`. */
function genericNodeError(message: string, props: object): Error {
	return Object.assign(new Error(message), props);
}

// ---- stdio ----

function stdioStringToArray(stdio: string, channel?: "ipc"): StdioOption[] {
	const options: StdioOption[] = [];
	switch (stdio) {
		case "ignore":
		case "overlapped":
		case "pipe":
			options.push(stdio, stdio, stdio);
			break;
		case "inherit":
			options.push(0, 1, 2);
			break;
		default:
			throw invalidArgValue("stdio", stdio);
	}
	if (channel) options.push(channel);
	return options;
}

interface StdioWrapStream {
	_handle?: { fd?: number; reading?: boolean; readStop?: () => void } | null;
	pause?: () => unknown;
	readableFlowing?: boolean | null;
	[kIsUsedAsStdio]?: boolean;
}

type StdioEntry =
	| { type: "pipe"; handle: StreamHandle; readable: boolean; writable: boolean; ipc?: boolean; socket?: Socket | null }
	| { type: "pipe-sync"; readable: boolean; writable: boolean; input?: Uint8Array }
	| { type: "ignore" }
	| { type: "inherit"; fd: number }
	| { type: "fd"; fd: number; wrap?: StdioWrapStream };

function getValidStdio(
	stdioOption: string | readonly StdioOption[],
	sync: boolean,
): { stdio: StdioEntry[]; ipc?: StreamHandle; ipcFd?: number } {
	let ipc: StreamHandle | undefined;
	let ipcFd: number | undefined;
	let list: StdioOption[];
	if (typeof stdioOption === "string") list = stdioStringToArray(stdioOption);
	else if (Array.isArray(stdioOption)) list = stdioOption.slice();
	else throw invalidArgValue("stdio", stdioOption);
	while (list.length < 3) list.push(undefined);

	const acc: StdioEntry[] = [];
	const cleanup = (): void => {
		for (const entry of acc) if (entry.type === "pipe") entry.handle.close();
	};
	for (let i = 0; i < list.length; i++) {
		const stdio = list[i] ?? (i < 3 ? "pipe" : "ignore");
		if (stdio === "ignore") {
			acc.push({ type: "ignore" });
		} else if (stdio === "pipe" || stdio === "overlapped" || (typeof stdio === "number" && stdio < 0)) {
			if (sync) acc.push({ type: "pipe-sync", readable: i === 0, writable: i !== 0 });
			else acc.push({ type: "pipe", handle: createPipe(false), readable: i === 0, writable: i !== 0 });
		} else if (stdio === "ipc") {
			if (sync || ipc !== undefined) {
				cleanup();
				if (!sync) throw codedError("ERR_IPC_ONE_PIPE", "Child process can have only one IPC pipe");
				throw codedError("ERR_IPC_SYNC_FORK", "IPC cannot be used with synchronous forks");
			}
			ipc = createPipe(true);
			ipcFd = i;
			acc.push({ type: "pipe", handle: ipc, readable: true, writable: true, ipc: true });
		} else if (stdio === "inherit") {
			acc.push({ type: "inherit", fd: i });
		} else if (typeof stdio === "number") {
			acc.push({ type: "fd", fd: stdio });
		} else if (typeof stdio === "object" && typeof stdio.fd === "number") {
			acc.push({ type: "fd", fd: stdio.fd });
		} else if (
			typeof stdio === "object" &&
			typeof (stdio as StdioWrapStream)._handle?.fd === "number" &&
			(stdio as StdioWrapStream)._handle!.fd! >= 0
		) {
			// a net.Socket/tty stream: hand its descriptor to the child
			const wrap: StdioWrapStream = stdio as StdioWrapStream;
			acc.push({ type: "fd", fd: wrap._handle!.fd!, wrap });
		} else if (ArrayBuffer.isView(stdio) || typeof stdio === "string") {
			if (!sync) {
				cleanup();
				throw codedError(
					"ERR_INVALID_SYNC_FORK_INPUT",
					`Asynchronous forks do not support Buffer, TypedArray, DataView or string input: ${String(stdio)}`,
					TypeError,
				);
			}
			const input =
				typeof stdio === "string"
					? Buffer.from(stdio)
					: new Uint8Array(
							(stdio as ArrayBufferView).buffer,
							(stdio as ArrayBufferView).byteOffset,
							(stdio as ArrayBufferView).byteLength,
						);
			acc.push({ type: "pipe-sync", readable: true, writable: false, input });
		} else {
			cleanup();
			throw invalidArgValue("stdio", stdio);
		}
	}
	return { stdio: acc, ipc, ipcFd };
}

// ---- argument normalization ----

let emittedDEP0190 = false;

function normalizeSpawnArguments(file: unknown, args: unknown, options: unknown): NormalizedSpawnOptions {
	validateString(file, "file");
	validateArgumentNullCheck(file, "file");
	if (file.length === 0) throw invalidArgValue("file", file, "cannot be empty");

	let argv: string[];
	if (Array.isArray(args)) argv = args.slice() as string[];
	else if (args == null) argv = [];
	else if (typeof args !== "object") throw invalidArgType("args", "of type object", args);
	else {
		options = args;
		argv = [];
	}
	validateArgumentsNullCheck(argv, "args");

	if (options === undefined) options = {};
	else validateObject(options, "options");
	const opts = { ...(options as SpawnOptions) };

	let cwd: string | undefined;
	if (opts.cwd != null) cwd = getValidatedPath(opts.cwd, "options.cwd");
	if (opts.detached != null) validateBoolean(opts.detached, "options.detached");
	if (opts.uid != null) validateInt32(opts.uid, "options.uid");
	if (opts.gid != null) validateInt32(opts.gid, "options.gid");
	if (opts.shell != null && typeof opts.shell !== "boolean" && typeof opts.shell !== "string") {
		throw invalidArgType("options.shell", "of type boolean or string", opts.shell);
	}
	if (opts.argv0 != null) {
		validateString(opts.argv0, "options.argv0");
		validateArgumentNullCheck(opts.argv0, "options.argv0");
	}
	if (opts.windowsHide != null) validateBoolean(opts.windowsHide, "options.windowsHide");
	if (opts.windowsVerbatimArguments != null)
		validateBoolean(opts.windowsVerbatimArguments, "options.windowsVerbatimArguments");

	let spawnFile: string = file;
	if (opts.shell) {
		validateArgumentNullCheck(opts.shell, "options.shell");
		if (argv.length > 0 && !emittedDEP0190) {
			process.emitWarning(
				"Passing args to a child process with shell option true can lead to security vulnerabilities, as the arguments are not escaped, only concatenated.",
				"DeprecationWarning",
				"DEP0190",
			);
			emittedDEP0190 = true;
		}
		const command = argv.length > 0 ? `${file} ${argv.join(" ")}` : file;
		spawnFile =
			typeof opts.shell === "string" ? opts.shell : process.platform === "android" ? "/system/bin/sh" : "/bin/sh";
		argv = ["-c", command];
	}
	argv.unshift(typeof opts.argv0 === "string" ? opts.argv0 : spawnFile);

	const env = (opts.env ?? process.env) as Record<string, unknown>;
	const pairs = envPairs(env, (key, value) => {
		validateArgumentNullCheck(key, `options.env['${key}']`);
		validateArgumentNullCheck(value, `options.env['${key}']`);
	});

	return {
		...opts,
		args: argv,
		cwd,
		detached: !!opts.detached,
		envPairs: pairs,
		file: spawnFile,
		windowsHide: !!opts.windowsHide,
		windowsVerbatimArguments: !!opts.windowsVerbatimArguments,
	};
}

// ---- ChildProcess ----

function flushStdio(subprocess: ChildProcess): void {
	const stdio = subprocess.stdio;
	if (stdio == null) return;
	for (const stream of stdio) {
		if (!stream || !stream.readable || (stream as unknown as StdioWrapStream)[kIsUsedAsStdio]) continue;
		stream.resume();
	}
}

function maybeClose(subprocess: ChildProcess): void {
	subprocess._closesGot++;
	if (subprocess._closesGot === subprocess._closesNeeded)
		subprocess.emit("close", subprocess.exitCode, subprocess.signalCode);
}

export class ChildProcess extends EventEmitter {
	_closesNeeded = 1;
	_closesGot = 0;
	connected = false;
	signalCode: string | null = null;
	exitCode: number | null = null;
	killed = false;
	timedOut = false;
	spawnfile: string | null = null;
	spawnargs: string[] = [];
	pid: number | undefined = undefined;
	stdin: Socket | null = null;
	stdout: Socket | null = null;
	stderr: Socket | null = null;
	stdio: (Socket | null)[] = [];
	channel: Channel | null | undefined = undefined;
	_handle: ProcessHandle | null = null;
	declare send?: (
		message: Serializable,
		sendHandle?: SendHandle,
		options?: object | MessageCallback,
		callback?: MessageCallback,
	) => boolean;
	declare disconnect?: () => void;

	constructor() {
		super();
	}

	/** Exit (or async spawn failure, with a negative errno) reported by the native handle. */
	_onexit(exitCode: number, signalCode: string | null): void {
		if (signalCode) this.signalCode = signalCode;
		else this.exitCode = exitCode;

		if (this.stdin) this.stdin.destroy();
		if (this._handle) {
			this._handle.close();
			this._handle = null;
		}

		if (exitCode < 0) {
			const syscall = this.spawnfile ? `spawn ${this.spawnfile}` : "spawn";
			const err = errnoException(exitCode, syscall);
			if (this.spawnfile) err.path = this.spawnfile;
			err.spawnargs = this.spawnargs.slice(1);
			this.emit("error", err);
		} else {
			this.emit("exit", this.exitCode, this.signalCode);
		}
		// pull the remaining data of untouched pipes through so they reach EOF and close
		process.nextTick(flushStdio, this);
		maybeClose(this);
	}

	spawn(options: NormalizedSpawnOptions): number {
		validateObject(options, "options");
		const valid = getValidStdio(options.stdio || "pipe", false);
		const ipc = valid.ipc;
		const stdio = valid.stdio;
		const serialization = validateSerialization(options.serialization);

		if (ipc !== undefined) {
			if (options.envPairs === undefined) options.envPairs = [];
			else if (!Array.isArray(options.envPairs))
				throw invalidArgType("options.envPairs", "an instance of Array", options.envPairs);
			options.envPairs.push(`NODE_CHANNEL_FD=${valid.ipcFd}`, `NODE_CHANNEL_SERIALIZATION_MODE=${serialization}`);
		}

		validateString(options.file, "options.file");
		this.spawnfile = options.file;
		if (options.args === undefined) this.spawnargs = [];
		else if (!Array.isArray(options.args)) throw invalidArgType("options.args", "an instance of Array", options.args);
		else this.spawnargs = options.args;

		const nativeStdio: StdioSpec[] = stdio.map((entry): StdioSpec => {
			switch (entry.type) {
				case "pipe":
					return { type: "pipe", handle: entry.handle, readable: entry.readable, writable: entry.writable };
				case "inherit":
					return { type: "inherit", fd: entry.fd };
				case "fd":
					return { type: "fd", fd: entry.fd };
				default:
					return { type: "ignore" };
			}
		});
		const nativeOptions: NativeSpawnOptions = {
			file: options.file,
			args: this.spawnargs,
			env: options.envPairs,
			stdio: nativeStdio,
			detached: options.detached,
		};
		if (options.cwd !== undefined) nativeOptions.cwd = options.cwd;
		if (options.uid != null) nativeOptions.uid = options.uid;
		if (options.gid != null) nativeOptions.gid = options.gid;

		let err = 0;
		try {
			this._handle = processNative.spawn(
				nativeOptions,
				bind((exitCode: number, signalCode: string | null) => this._onexit(exitCode, signalCode)),
			);
		} catch (e) {
			const errno = nativeErrno(e);
			if (errno === undefined) {
				for (const entry of stdio) if (entry.type === "pipe") entry.handle.close();
				throw e;
			}
			err = errno;
		}

		if (err !== 0) {
			// close every pipe: the child never got them
			for (const entry of stdio) if (entry.type === "pipe") entry.handle.close();
			const runtime = ["EACCES", "EAGAIN", "EMFILE", "ENFILE", "ENOENT"].includes(errnoName(err));
			if (!runtime) throw errnoException(err, "spawn");
			// run-time errors are emitted, not thrown
			process.nextTick(() => this._onexit(err, null));
			if (errnoName(err) === "EMFILE" || errnoName(err) === "ENFILE") return err;
		} else {
			process.nextTick(() => this.emit("spawn"));
			this.pid = this._handle!.pid;
		}

		for (let i = 0; i < stdio.length; i++) {
			const entry = stdio[i];
			if (entry.type === "ignore") continue;
			if (entry.type === "pipe" && entry.ipc) {
				if (err === 0) this._closesNeeded++;
				continue;
			}
			if (entry.type === "fd" && entry.wrap) {
				// the stream's descriptor now belongs to the child too: stop reading it here
				const wrap = entry.wrap;
				if (wrap._handle) {
					wrap._handle.reading = false;
					wrap._handle.readStop?.();
				}
				wrap.pause?.();
				wrap.readableFlowing = false;
				wrap[kIsUsedAsStdio] = true;
				continue;
			}
			if (entry.type === "pipe") {
				entry.socket = new Socket(err === 0 ? { handle: entry.handle, readable: i > 0 } : { readable: i > 0 });
				if (i > 0 && err === 0) {
					this._closesNeeded++;
					entry.socket.on("close", () => maybeClose(this));
				}
			}
		}

		const socketAt = (i: number): Socket | null => {
			const entry = stdio[i];
			return entry && entry.type === "pipe" && !entry.ipc && entry.socket ? entry.socket : null;
		};
		this.stdin = socketAt(0);
		this.stdout = socketAt(1);
		this.stderr = socketAt(2);
		this.stdio = stdio.map((_, i) => socketAt(i));

		if (ipc !== undefined && err === 0) {
			setupNodeChannel(this as unknown as NodeChannelTarget, ipc, serialization, () => maybeClose(this));
		}
		return err;
	}

	kill(sig?: string | number): boolean {
		const signal = sig === 0 ? 0 : convertToValidSignal(sig === undefined ? "SIGTERM" : sig);
		if (this._handle) {
			const err = this._handle.kill(signal);
			if (err === 0) {
				this.killed = true;
				return true;
			}
			const name = errnoName(err);
			if (name === "ESRCH") {
				// already dead
			} else if (name === "EINVAL" || name === "ENOSYS") {
				throw errnoException(err, "kill");
			} else {
				this.emit("error", errnoException(err, "kill"));
			}
		}
		return false;
	}

	[Symbol.dispose](): void {
		if (!this.killed) this.kill();
	}

	ref(): void {
		this._handle?.ref();
	}

	unref(): void {
		this._handle?.unref();
	}
}

// ---- spawn ----

function abortChildProcess(child: ChildProcess, killSignal: number | undefined, reason: unknown): void {
	try {
		if (child.kill(killSignal as number)) child.emit("error", abortError(reason));
	} catch (err) {
		child.emit("error", err);
	}
}

export function spawn(file: string, args?: readonly string[] | SpawnOptions, options?: SpawnOptions): ChildProcess {
	const opts = normalizeSpawnArguments(file, args, options);
	validateTimeout(opts.timeout);
	validateAbortSignal(opts.signal, "options.signal");
	const killSignal = sanitizeKillSignal(opts.killSignal);
	const child = new ChildProcess();
	child.spawn(opts);

	if (opts.timeout != null && opts.timeout > 0) {
		let timeoutId: Timeout | null = setTimeout(() => {
			if (timeoutId) {
				try {
					child.timedOut = child.kill(killSignal as number);
				} catch (err) {
					child.emit("error", err);
				}
				timeoutId = null;
			}
		}, opts.timeout);
		child.once("close", () => {
			if (timeoutId) {
				clearTimeout(timeoutId);
				timeoutId = null;
			}
		});
	}

	const signal = opts.signal;
	if (signal) {
		const onAbort = (): void => abortChildProcess(child, killSignal, signal.reason);
		if (signal.aborted) process.nextTick(onAbort);
		else {
			signal.addEventListener("abort", onAbort, { once: true });
			child.once("exit", () => signal.removeEventListener("abort", onAbort));
		}
	}
	return child;
}

// ---- fork ----

export interface ForkOptions extends Omit<SpawnOptions, "shell"> {
	execPath?: string;
	execArgv?: string[];
	silent?: boolean;
}

export function fork(
	modulePath: string | URL,
	args?: readonly string[] | ForkOptions,
	options?: ForkOptions,
): ChildProcess {
	const path = getValidatedPath(modulePath, "modulePath");
	let argv: string[];
	if (args == null) argv = [];
	else if (typeof args === "object" && !Array.isArray(args)) {
		options = args as ForkOptions;
		argv = [];
	} else if (!Array.isArray(args)) throw invalidArgType("args", "an instance of Array", args);
	else argv = args.slice();
	if (options != null) validateObject(options, "options");

	const opts: ForkOptions & { shell: false } = { ...options, shell: false };
	opts.execPath ||= process.execPath;
	validateArgumentNullCheck(opts.execPath, "options.execPath");
	const execArgv = opts.execArgv || process.execArgv;
	validateArgumentsNullCheck(execArgv, "options.execArgv");
	argv = [...execArgv, path, ...argv];

	if (typeof opts.stdio === "string") opts.stdio = stdioStringToArray(opts.stdio, "ipc");
	else if (!Array.isArray(opts.stdio)) opts.stdio = stdioStringToArray(opts.silent ? "pipe" : "inherit", "ipc");
	else if (!opts.stdio.includes("ipc")) {
		throw codedError(
			"ERR_CHILD_PROCESS_IPC_REQUIRED",
			"Forked processes must have an IPC channel, missing value 'ipc' in options.stdio",
		);
	}
	return spawn(opts.execPath, argv, opts);
}

/** Node's internal child side of fork(): process.send() over `fd`. */
export function _forkChild(fd: number, serializationMode: "json" | "advanced"): void {
	forkChildChannel(process as unknown as NodeChannelTarget, fd, serializationMode);
}

// ---- exec / execFile ----

export interface ExecFileOptions extends SpawnOptions {
	encoding?: Encoding;
	maxBuffer?: number;
}

type ExecCallback = (error: ExecError | null, stdout: string | Buffer, stderr: string | Buffer) => void;

interface ExecError extends Error {
	code?: number | string;
	killed?: boolean;
	signal?: string | null;
	cmd?: string;
	timedOut?: boolean;
	stdout?: string | Buffer;
	stderr?: string | Buffer;
}

function normalizeExecArgs(
	command: unknown,
	options: unknown,
	callback: unknown,
): { file: string; options: ExecFileOptions; callback: ExecCallback | undefined } {
	validateString(command, "command");
	validateArgumentNullCheck(command, "command");
	if (typeof options === "function") {
		callback = options;
		options = undefined;
	}
	const opts = { ...(options as ExecFileOptions | undefined) };
	opts.shell = typeof opts.shell === "string" ? opts.shell : true;
	return { file: command, options: opts, callback: callback as ExecCallback | undefined };
}

export function exec(
	command: string,
	options?: ExecFileOptions | ExecCallback | null,
	callback?: ExecCallback,
): ChildProcess {
	const opts = normalizeExecArgs(command, options, callback);
	return execFile(opts.file, opts.options, opts.callback);
}

function normalizeExecFileArgs(
	file: string,
	args: unknown,
	options: unknown,
	callback: unknown,
): { file: string; args: string[]; options: ExecFileOptions; callback: ExecCallback | undefined } {
	let argv: string[] | null = null;
	if (Array.isArray(args)) argv = args.slice();
	else if (args != null && typeof args === "object") {
		callback = options;
		options = args;
	} else if (typeof args === "function") {
		callback = args;
		options = null;
	}
	argv ??= [];
	if (typeof options === "function") callback = options;
	else if (options != null) validateObject(options, "options");
	const opts = (options && typeof options === "object" ? options : {}) as ExecFileOptions;
	if (callback != null) validateFunction(callback, "callback");
	if (opts.argv0 != null) {
		validateString(opts.argv0, "options.argv0");
		validateArgumentNullCheck(opts.argv0, "options.argv0");
	}
	return { file, args: argv, options: opts, callback: (callback ?? undefined) as ExecCallback | undefined };
}

function maxBufferError(name: string): CodedError {
	return codedError("ERR_CHILD_PROCESS_STDIO_MAXBUFFER", `${name} maxBuffer length exceeded`, RangeError);
}

export function execFile(
	file: string,
	args?: readonly string[] | ExecFileOptions | ExecCallback | null,
	options?: ExecFileOptions | ExecCallback | null,
	callback?: ExecCallback,
): ChildProcess {
	const n = normalizeExecFileArgs(file, args, options, callback);
	const cb = n.callback;
	const opts: ExecFileOptions & { encoding: Encoding; timeout: number; maxBuffer: number } = {
		encoding: "utf8",
		timeout: 0,
		maxBuffer: MAX_BUFFER,
		killSignal: "SIGTERM",
		shell: false,
		...n.options,
	} as ExecFileOptions & { encoding: Encoding; timeout: number; maxBuffer: number };
	validateTimeout(opts.timeout);
	validateMaxBuffer(opts.maxBuffer);
	const killSignal = sanitizeKillSignal(opts.killSignal);

	const child = spawn(n.file, n.args, {
		cwd: opts.cwd ?? undefined,
		env: opts.env ?? undefined,
		gid: opts.gid,
		shell: opts.shell,
		signal: opts.signal,
		uid: opts.uid,
		argv0: opts.argv0,
		windowsHide: !!opts.windowsHide,
		windowsVerbatimArguments: !!opts.windowsVerbatimArguments,
	});

	const encoding =
		opts.encoding !== "buffer" && opts.encoding != null && Buffer.isEncoding(opts.encoding) ? opts.encoding : null;
	const stdoutChunks: (string | Buffer)[] = [];
	const stderrChunks: (string | Buffer)[] = [];
	let stdoutLen = 0;
	let stderrLen = 0;
	let killed = false;
	let exited = false;
	let timeoutId: Timeout | null = null;
	let ex: ExecError | null = null;
	let cmd = n.file;

	const join = (chunks: (string | Buffer)[], stream: Socket | null): string | Buffer => {
		if (encoding || stream?.readableEncoding) return chunks.join("");
		return Buffer.concat(chunks as Buffer[]);
	};

	function exithandler(code?: number | null, signal?: string | null): void {
		if (exited) return;
		exited = true;
		if (timeoutId) {
			clearTimeout(timeoutId);
			timeoutId = null;
		}
		if (!cb) return;
		const stdout = join(stdoutChunks, child.stdout);
		const stderr = join(stderrChunks, child.stderr);
		if (!ex && code === 0 && signal === null) {
			cb(null, stdout, stderr);
			return;
		}
		if (n.args.length) cmd += ` ${n.args.join(" ")}`;
		if (!ex) {
			ex = genericNodeError(`Command failed: ${cmd}\n${String(stderr)}`, {
				code: code != null && code < 0 ? errnoName(code) : code,
				killed: child.killed || killed,
				signal: signal ?? null,
			}) as ExecError;
		}
		ex.cmd = cmd;
		ex.timedOut = child.timedOut;
		cb(ex, stdout, stderr);
	}

	function errorhandler(e: ExecError): void {
		ex = e;
		child.stdout?.destroy();
		child.stderr?.destroy();
		exithandler();
	}

	function kill(): boolean {
		child.stdout?.destroy();
		child.stderr?.destroy();
		killed = true;
		try {
			return child.kill(killSignal as number);
		} catch (e) {
			ex = e as ExecError;
			exithandler();
		}
		return false;
	}

	if (opts.timeout > 0) {
		timeoutId = setTimeout(() => {
			child.timedOut = kill();
			timeoutId = null;
		}, opts.timeout);
	}

	const collect = (stream: Socket | null, chunks: (string | Buffer)[], which: "stdout" | "stderr"): void => {
		if (!stream) return;
		if (encoding) stream.setEncoding(encoding);
		stream.on("data", (chunk: string | Buffer) => {
			if (opts.maxBuffer === Infinity) {
				chunks.push(chunk);
				return;
			}
			const enc = stream.readableEncoding;
			const length = enc ? Buffer.byteLength(chunk as string, enc as BufferEncoding) : chunk.length;
			const total = which === "stdout" ? (stdoutLen += length) : (stderrLen += length);
			if (total > opts.maxBuffer) {
				const truncatedLen = opts.maxBuffer - (total - length);
				chunks.push(chunk.slice(0, truncatedLen));
				ex = maxBufferError(which) as ExecError;
				kill();
			} else {
				chunks.push(chunk);
			}
		});
	};
	collect(child.stdout, stdoutChunks, "stdout");
	collect(child.stderr, stderrChunks, "stderr");

	child.addListener("close", exithandler);
	child.addListener("error", errorhandler);
	return child;
}

function customPromiseExecFunction<A extends unknown[]>(orig: (...args: [...A, ExecCallback]) => ChildProcess) {
	return function (
		...args: A
	): Promise<{ stdout: string | Buffer; stderr: string | Buffer }> & { child: ChildProcess } {
		const { promise, resolve, reject } = Promise.withResolvers<{ stdout: string | Buffer; stderr: string | Buffer }>();
		const result = promise as typeof promise & { child: ChildProcess };
		result.child = orig(...args, (err, stdout, stderr) => {
			if (err !== null) {
				err.stdout = stdout;
				err.stderr = stderr;
				reject(err);
			} else {
				resolve({ stdout, stderr });
			}
		});
		return result;
	};
}

Object.defineProperty(exec, promisifyCustom, {
	enumerable: false,
	value: customPromiseExecFunction(exec as (...args: unknown[]) => ChildProcess),
});
Object.defineProperty(execFile, promisifyCustom, {
	enumerable: false,
	value: customPromiseExecFunction(execFile as (...args: unknown[]) => ChildProcess),
});

// ---- sync ----

export interface SpawnSyncOptions extends SpawnOptions {
	input?: string | ArrayBufferView;
	maxBuffer?: number;
	encoding?: Encoding;
}

export interface SpawnSyncReturns<T = Buffer | string> {
	pid: number;
	output: (T | null)[] | null;
	stdout: T | null | undefined;
	stderr: T | null | undefined;
	status: number | null;
	signal: string | null;
	error?: ErrnoError;
}

export function spawnSync(
	file: string,
	args?: readonly string[] | SpawnSyncOptions,
	options?: SpawnSyncOptions,
): SpawnSyncReturns {
	const normalized = normalizeSpawnArguments(file, args, options) as NormalizedSpawnOptions & SpawnSyncOptions;
	const opts = { maxBuffer: MAX_BUFFER, ...normalized };
	validateTimeout(opts.timeout);
	validateMaxBuffer(opts.maxBuffer);
	const killSignal = sanitizeKillSignal(opts.killSignal);
	const stdio = getValidStdio(opts.stdio || "pipe", true).stdio;

	if (opts.input) {
		const value = opts.input;
		let bytes: Uint8Array;
		if (ArrayBuffer.isView(value)) bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
		else if (typeof value === "string")
			bytes = Buffer.from(
				value,
				(opts.encoding && opts.encoding !== "buffer" ? opts.encoding : "utf8") as BufferEncoding,
			);
		else
			throw invalidArgType(
				"options.stdio[0]",
				"of type string or an instance of Buffer, TypedArray, or DataView",
				value,
			);
		stdio[0] = { type: "pipe-sync", readable: true, writable: false, input: bytes };
	}

	let input: Uint8Array | undefined;
	const nativeStdio: SyncStdio[] = stdio.map((entry, i): SyncStdio => {
		switch (entry.type) {
			case "pipe-sync":
				if (entry.input && i === 0) input = entry.input;
				else if (entry.input)
					throw codedError("ERR_NOT_SUPPORTED", `Input on stdio[${i}] is not supported by qbun (only stdio[0])`);
				return "pipe";
			case "inherit":
				return "inherit";
			case "fd":
				return entry.fd;
			default:
				return "ignore";
		}
	});

	const result = processNative.spawnSync({
		file: opts.file,
		args: opts.args,
		cwd: opts.cwd,
		env: opts.envPairs,
		stdio: nativeStdio,
		input,
		timeout: opts.timeout ?? 0,
		killSignal,
		maxBuffer: opts.maxBuffer,
		detached: opts.detached,
		uid: opts.uid ?? undefined,
		gid: opts.gid ?? undefined,
	});

	// the process never ran: Node reports `output: null`
	const ran = !(result.error && result.pid === 0);
	const output: (Buffer | string | null)[] | null =
		ran && result.output
			? result.output.map((chunk) => {
					if (!chunk) return null;
					const buf = Buffer.from(chunk.buffer as ArrayBuffer, chunk.byteOffset, chunk.byteLength);
					return opts.encoding && opts.encoding !== "buffer" ? buf.toString(opts.encoding) : buf;
				})
			: null;
	const ret: SpawnSyncReturns = {
		pid: result.pid,
		output,
		stdout: output?.[1],
		stderr: output?.[2],
		status: result.status,
		signal: result.signal,
	};
	if (result.error) {
		const errno = nativeErrno(result.error);
		const err = errno !== undefined ? errnoException(errno, `spawnSync ${opts.file}`) : (result.error as ErrnoError);
		err.path = opts.file;
		err.spawnargs = opts.args.slice(1);
		ret.error = err;
	}
	return ret;
}

function checkExecSyncError(
	ret: SpawnSyncReturns,
	args: readonly string[] | undefined,
	cmd?: string,
): Error | undefined {
	if (ret.error) return Object.assign(ret.error, ret);
	if (ret.status !== 0) {
		let msg = `Command failed: ${cmd || (args ?? []).join(" ")}`;
		if (ret.stderr && ret.stderr.length > 0) msg += `\n${ret.stderr.toString()}`;
		return genericNodeError(msg, ret);
	}
	return undefined;
}

export function execFileSync(
	file: string,
	args?: readonly string[] | SpawnSyncOptions,
	options?: SpawnSyncOptions,
): Buffer | string | null | undefined {
	const n = normalizeExecFileArgs(file, args, options, undefined);
	const opts = n.options as SpawnSyncOptions;
	const inheritStderr = !opts.stdio;
	const ret = spawnSync(n.file, n.args, opts);
	if (inheritStderr && ret.stderr) process.stderr.write(ret.stderr);
	const err = checkExecSyncError(ret, [opts.argv0 || n.file, ...n.args]);
	if (err) throw err;
	return ret.stdout;
}

export function execSync(command: string, options?: SpawnSyncOptions): Buffer | string | null | undefined {
	const opts = normalizeExecArgs(command, options, null);
	const inheritStderr = !opts.options.stdio;
	const ret = spawnSync(opts.file, opts.options as SpawnSyncOptions);
	if (inheritStderr && ret.stderr) process.stderr.write(ret.stderr);
	const err = checkExecSyncError(ret, undefined, command);
	if (err) throw err;
	return ret.stdout;
}

export default {
	_forkChild,
	ChildProcess,
	exec,
	execFile,
	execFileSync,
	execSync,
	fork,
	spawn,
	spawnSync,
};
