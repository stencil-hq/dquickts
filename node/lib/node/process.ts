// node:process — process state, lifecycle, signals, and lazy standard streams.
import * as core from "qbun:core";
import * as native from "qbun:process";
import * as netBinding from "qbun:net";
import { EventEmitter } from "node:events";
import { codedError, invalidArgType, outOfRange } from "internal:errors";
import { bind } from "internal:async-context";

interface CoreInfo {
	argv: string[];
	execArgv: string[];
	execPath: string;
	mainPath?: string;
	libDir: string;
	isMainThread: boolean;
	pid: number;
	ppid: number;
	platform: string;
	arch: string;
	engine: string;
	uvVersion: string;
}
interface StdioStream extends EventEmitter {
	fd: number;
	isTTY?: boolean;
	columns?: number;
	rows?: number;
	write(
		chunk: string | Uint8Array,
		encodingOrCallback?: string | ((error?: Error | null) => void),
		callback?: (error?: Error | null) => void,
	): boolean;
	pause(): this;
	resume(): this;
	ref(): this;
	unref(): this;
	_refreshSize?(): void;
}
interface SignalHandle {
	stop(): void;
	close(): void;
	unref(): void;
}
const info = core.info() as CoreInfo;
const startTime = core.hrtime() as bigint;
export const BUN_VERSION = "1.4.4";
const NODE_VERSION = "24.3.0";
const envTarget = Object.assign(Object.create(null), core.environ()) as Record<string, string>;
const env = new Proxy(envTarget, {
	set(target, key, value) {
		if (typeof key !== "string") throw invalidArgType("name", "of type string", key);
		const text = String(value);
		core.setenv(key, text);
		target[key] = text;
		return true;
	},
	deleteProperty(target, key) {
		if (typeof key !== "string") return false;
		core.setenv(key, undefined);
		delete target[key];
		return true;
	},
	defineProperty(target, key, descriptor) {
		if (
			typeof key !== "string" ||
			!("value" in descriptor) ||
			descriptor.get ||
			descriptor.set ||
			descriptor.configurable === false ||
			descriptor.enumerable === false ||
			descriptor.writable === false
		)
			throw codedError(
				"ERR_INVALID_OBJECT_DEFINE_PROPERTY",
				"'process.env' only accepts a configurable, writable, and enumerable data descriptor",
				TypeError,
			);
		const text = String(descriptor.value);
		core.setenv(key, text);
		target[key] = text;
		return true;
	},
});
let nextTickQueue: [(...args: unknown[]) => void, unknown[]][] = [];
let tickScheduled = false;
function runTicks(): void {
	tickScheduled = false;
	while (nextTickQueue.length) {
		const queue = nextTickQueue;
		nextTickQueue = [];
		for (let i = 0; i < queue.length; i++) {
			const [fn, args] = queue[i];
			try {
				fn(...args);
			} catch (error) {
				nextTickQueue = queue.slice(i + 1).concat(nextTickQueue);
				if (nextTickQueue.length && !tickScheduled) {
					tickScheduled = true;
					queueMicrotask(runTicks);
				}
				throw error;
			}
		}
	}
}
function int32(value: unknown, name: string): asserts value is number {
	if (typeof value !== "number") throw invalidArgType(name, "of type number", value);
	if (!Number.isInteger(value) || value < -2147483648 || value > 2147483647)
		throw outOfRange(name, "a signed 32-bit integer", value);
}
function validateExitCode(value: unknown): number | undefined {
	if (value === undefined || value === null) return undefined;
	if (typeof value === "string" && value.trim() !== "") value = Number(value);
	int32(value, "code");
	return value;
}
function normalizeExitCode(value: unknown): number {
	return (validateExitCode(value) ?? 0) & 255;
}
function systemError(result: number, syscall: string): Error {
	// node:os pulls node:buffer before bootstrap has installed lazy globals.
	const constants = (require("node:os") as { constants: { errno: Record<string, number> } }).constants;
	let code = "UNKNOWN";
	for (const name in constants.errno)
		if (constants.errno[name] === -result) {
			code = name;
			break;
		}
	const descriptions: Record<string, string> = {
		ESRCH: "no such process",
		EPERM: "operation not permitted",
		EINVAL: "invalid argument",
	};
	return Object.assign(new Error(`${syscall} ${code}${descriptions[code] ? `: ${descriptions[code]}` : ""}`), {
		code,
		errno: result,
		syscall,
	});
}
let uncaughtCapture: ((err: unknown) => void) | null = null;
let storedExitCode: number | undefined;
let stdinCache: StdioStream | undefined, stdoutCache: StdioStream | undefined, stderrCache: StdioStream | undefined;
const signalHandles = new Map<string, SignalHandle>();
function stdio(name: "Stdin" | "Stdout" | "Stderr"): StdioStream {
	// Standard streams pull the buffer/stream/net graph, which needs installed globals.
	const module = require("internal:stdio") as Record<string, () => StdioStream>;
	return module[`get${name}`]();
}
class Process extends EventEmitter {
	version = `v${NODE_VERSION}`;
	versions: Record<string, string> = {
		node: NODE_VERSION,
		bun: BUN_VERSION,
		quickjs: info.engine,
		uv: info.uvVersion,
		modules: "137",
		napi: "10",
	};
	arch = info.arch;
	platform = info.platform;
	release = { name: "node", lts: "", sourceUrl: "", headersUrl: "" };
	argv = info.argv;
	argv0 = info.argv[0] ?? "qbun";
	execArgv = info.execArgv;
	execPath = info.execPath;
	pid = info.pid;
	ppid = info.ppid;
	env = env;
	isBun = true;
	browser = false;
	revision = "qbun";
	config = { variables: {}, target_defaults: {} };
	features = {
		inspector: false,
		debug: false,
		uv: true,
		ipv6: true,
		tls_alpn: true,
		tls_sni: true,
		tls_ocsp: false,
		tls: true,
		cached_builtins: true,
		typescript: "transform",
	};
	allowedNodeEnvironmentFlags = new Set<string>();
	mainModule: unknown = undefined;
	_exiting = false;
	channel = undefined;
	connected = undefined;
	send = undefined;
	disconnect = undefined;
	noDeprecation = false;
	throwDeprecation = false;
	traceDeprecation = false;
	traceProcessWarnings = false;
	debugPort = 9229;
	sourceMapsEnabled = false;
	permission = {
		has(_scope: string, _reference?: string): boolean {
			return true;
		},
	};
	get title(): string {
		return native.getTitle();
	}
	set title(value: string) {
		native.setTitle(String(value));
	}
	get exitCode(): number | undefined {
		return storedExitCode;
	}
	set exitCode(value: number | string | undefined) {
		storedExitCode = validateExitCode(value);
	}
	stdin!: StdioStream;
	stdout!: StdioStream;
	stderr!: StdioStream;
	cwd(): string {
		return core.cwd();
	}
	chdir(directory: string): void {
		if (typeof directory !== "string") throw invalidArgType("directory", "of type string", directory);
		core.chdir(directory);
	}
	exit(code?: number | string): never {
		if (code !== undefined) this.exitCode = code;
		emitExit();
		return core.reallyExit(normalizeExitCode(this.exitCode)) as never;
	}
	reallyExit(code?: number): never {
		return core.reallyExit(normalizeExitCode(code)) as never;
	}
	abort(): never {
		native.kill(this.pid, native.signals.SIGABRT);
		return core.reallyExit(134) as never;
	}
	nextTick(fn: (...args: unknown[]) => void, ...args: unknown[]): void {
		if (typeof fn !== "function") throw invalidArgType("callback", "of type function", fn);
		nextTickQueue.push([bind(fn), args]);
		if (!tickScheduled) {
			tickScheduled = true;
			queueMicrotask(runTicks);
		}
	}
	hrtime(previous?: [number, number]): [number, number] {
		const time = core.hrtime() as bigint;
		let seconds = Number(time / 1000000000n),
			ns = Number(time % 1000000000n);
		if (previous !== undefined) {
			if (!Array.isArray(previous)) throw invalidArgType("time", "an instance of Array", previous);
			if (previous.length !== 2) throw outOfRange("time", "an array of length 2", previous.length);
			seconds -= previous[0];
			ns -= previous[1];
			if (ns < 0) {
				seconds--;
				ns += 1e9;
			}
		}
		return [seconds, ns];
	}
	uptime(): number {
		return Number((core.hrtime() as bigint) - startTime) / 1e9;
	}
	memoryUsage(): { rss: number; heapTotal: number; heapUsed: number; external: number; arrayBuffers: number } {
		return core.memoryUsage();
	}
	cpuUsage(previous?: { user: number; system: number }): { user: number; system: number } {
		const usage = native.cpuUsage();
		if (previous !== undefined) {
			for (const key of ["user", "system"] as const) {
				if (typeof previous?.[key] !== "number")
					throw invalidArgType(`prevValue.${key}`, "of type number", previous?.[key]);
				if (!Number.isFinite(previous[key]) || previous[key] < 0 || previous[key] > Number.MAX_SAFE_INTEGER)
					throw outOfRange(`prevValue.${key}`, ">= 0 && <= 9007199254740991", previous[key]);
			}
			return { user: usage.user - previous.user, system: usage.system - previous.system };
		}
		return usage;
	}
	resourceUsage(): Record<string, number> {
		return native.resourceUsage();
	}
	umask(mask?: number | string): number {
		if (typeof mask === "string") {
			if (!/^[0-7]+$/.test(mask))
				throw codedError("ERR_INVALID_ARG_VALUE", "The argument 'mask' must be an octal string", TypeError);
			mask = parseInt(mask, 8);
		}
		if (mask !== undefined) {
			int32(mask, "mask");
			if (mask < 0) throw outOfRange("mask", ">= 0", mask);
		}
		return native.umask(mask);
	}
	getuid(): number {
		return native.ids().uid;
	}
	geteuid(): number {
		return native.ids().euid;
	}
	getgid(): number {
		return native.ids().gid;
	}
	getegid(): number {
		return native.ids().egid;
	}
	getgroups(): number[] {
		return native.ids().groups;
	}
	kill(pid: number, signal: string | number = "SIGTERM"): true {
		int32(pid, "pid");
		let number: number;
		if (typeof signal === "number") {
			int32(signal, "signal");
			number = signal;
		} else if (typeof signal === "string" && Object.hasOwn(native.signals, signal.toUpperCase()))
			number = native.signals[signal.toUpperCase()];
		else throw codedError("ERR_UNKNOWN_SIGNAL", `Unknown signal: ${String(signal)}`, TypeError);
		const result = native.kill(pid, number);
		if (result < 0) throw systemError(result, "kill");
		return true;
	}
	dlopen(module: { exports: unknown }, filename: string, flags?: number): void {
		// Node-API initialization pulls buffer/URL modules before bootstrap globals exist.
		const napi = require("internal:napi") as { dlopen(m: { exports: unknown }, f: string, flags?: number): void };
		napi.dlopen(module, filename, flags);
	}
	emitWarning(
		warning: string | Error,
		type?: string | { type?: string; code?: string; detail?: string; ctor?: Function },
		code?: string | Function,
		ctor?: Function,
	): void {
		let detail: string | undefined;
		if (typeof type === "object" && type !== null) {
			detail = type.detail;
			ctor = type.ctor;
			code = type.code;
			type = type.type;
		}
		if (typeof type === "function") {
			ctor = type;
			type = undefined;
		}
		if (typeof code === "function") {
			ctor = code;
			code = undefined;
		}
		if (type !== undefined && typeof type !== "string") throw invalidArgType("type", "of type string", type);
		if (code !== undefined && typeof code !== "string") throw invalidArgType("code", "of type string", code);
		if (ctor !== undefined && typeof ctor !== "function") throw invalidArgType("ctor", "of type function", ctor);
		if (detail !== undefined && typeof detail !== "string")
			throw invalidArgType("options.detail", "of type string", detail);
		let error: Error & { code?: string; detail?: string };
		if (typeof warning === "string") {
			error = new Error(warning);
			error.name = type || "Warning";
			if (code !== undefined) error.code = code;
			if (detail !== undefined) error.detail = detail;
			const errorConstructor = Error as ErrorConstructor & {
				captureStackTrace?: (target: Error, constructor?: Function) => void;
			};
			errorConstructor.captureStackTrace?.(error, ctor ?? this.emitWarning);
		} else if (warning instanceof Error) error = warning;
		else throw invalidArgType("warning", "of type string or an instance of Error", warning);
		if (error.name === "DeprecationWarning" && this.noDeprecation) return;
		this.nextTick(() => {
			if (error.name === "DeprecationWarning" && this.throwDeprecation) throw error;
			this.emit("warning", error);
		});
	}
	constrainedMemory(): number {
		return typeof native.constrainedMemory === "function" ? native.constrainedMemory() : 0;
	}
	availableMemory(): number {
		if (typeof native.availableMemory === "function") return native.availableMemory();
	// node:os pulls node:buffer before bootstrap has installed lazy globals.
		const os = require("node:os") as { freemem(): number };
		return os.freemem();
	}
	loadEnvFile(path: string | URL | Uint8Array = "./.env"): void {
		// node:fs pulls streams, whose top-level setup needs installed buffer globals.
		const fs = require("node:fs") as { readFileSync(path: string | URL | Uint8Array, encoding: string): string };
		const source = fs.readFileSync(path, "utf8").replace(/^\uFEFF/, "");
		const pattern =
			/(?:^|\n)\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(?:"((?:\\.|[^"\\])*)"|'([^']*)'|`([^`]*)`|([^\r\n#]*))\s*(?:#[^\n]*)?/g;
		let match: RegExpExecArray | null;
		while ((match = pattern.exec(source))) {
			if (Object.hasOwn(this.env, match[1])) continue;
			const value =
				match[2] !== undefined
					? match[2].replace(/\\n/g, "\n").replace(/\\r/g, "\r")
					: (match[3] ?? match[4] ?? match[5].trim());
			this.env[match[1]] = value;
		}
	}
	getActiveResourcesInfo(): string[] {
		return [];
	}
	ref(object: unknown): void {
		const target = object as { [key: symbol]: (() => void) | undefined; ref?: () => void } | null;
		const fn = target?.[Symbol.for("nodejs.ref")] ?? target?.ref;
		if (typeof fn === "function") fn.call(target);
	}
	unref(object: unknown): void {
		const target = object as { [key: symbol]: (() => void) | undefined; unref?: () => void } | null;
		const fn = target?.[Symbol.for("nodejs.unref")] ?? target?.unref;
		if (typeof fn === "function") fn.call(target);
	}
	binding(name: string): never {
		throw codedError("ERR_INVALID_MODULE", `No such module: ${name}`);
	}
	setSourceMapsEnabled(value: boolean): void {
		if (typeof value !== "boolean") throw invalidArgType("val", "of type boolean", value);
		this.sourceMapsEnabled = value;
	}
	setUncaughtExceptionCaptureCallback(fn: ((err: unknown) => void) | null): void {
		if (fn !== null && typeof fn !== "function") throw invalidArgType("fn", "of type function or null", fn);
		if (fn && uncaughtCapture)
			throw codedError(
				"ERR_UNCAUGHT_EXCEPTION_CAPTURE_ALREADY_SET",
				"`process.setupUncaughtExceptionCapture()` was called while a capture callback was already active",
			);
		uncaughtCapture = fn;
	}
	hasUncaughtExceptionCaptureCallback(): boolean {
		return uncaughtCapture !== null;
	}
	get exiting(): boolean {
		return this._exiting;
	}
	report = {
		directory: "",
		filename: "",
		compact: false,
		excludeEnv: false,
		excludeNetwork: false,
		reportOnFatalError: false,
		reportOnSignal: false,
		reportOnUncaughtException: false,
		signal: "SIGUSR2",
		getReport(error?: Error): Record<string, unknown> {
			return {
				header: {
					reportVersion: 5,
					event: "JavaScript API",
					trigger: "GetReport",
					processId: info.pid,
					commandLine: info.argv,
					nodejsVersion: `v${NODE_VERSION}`,
					arch: info.arch,
					platform: info.platform,
				},
				javascriptStack: {
					message: error?.message ?? "JavaScript Callstack",
					stack: (error ?? new Error()).stack?.split("\n").slice(1) ?? [],
				},
				javascriptHeap: process.memoryUsage(),
				resourceUsage: process.resourceUsage(),
				environmentVariables: this.excludeEnv ? undefined : { ...env },
				sharedObjects: [],
			};
		},
		writeReport(filename?: string | Error, error?: Error): string {
			if (filename instanceof Error) {
				error = filename;
				filename = undefined;
			}
			const name = filename ?? (this.filename || `report.${Date.now()}.${info.pid}.json`);
			const path = this.directory ? `${this.directory}/${name}` : name;
			// node:fs pulls streams, whose top-level setup needs installed buffer globals.
			const fs = require("node:fs") as { writeFileSync(path: string, data: string): void };
			fs.writeFileSync(path, JSON.stringify(this.getReport(error), null, this.compact ? 0 : 2));
			return path;
		},
	};
}
const process = new Process();
(process.hrtime as unknown as { bigint(): bigint }).bigint = () => core.hrtime();
(process.memoryUsage as unknown as { rss(): number }).rss = () => core.memoryUsage().rss;
process.on("warning", (value: unknown) => {
	const warning = value as Error & { code?: string; detail?: string };
	core.writeSync(
		2,
		`(qbun:${info.pid}) ${warning.code ? `[${warning.code}] ` : ""}${process.traceProcessWarnings || (process.traceDeprecation && warning.name === "DeprecationWarning") ? warning.stack : `${warning.name}: ${warning.message}`}${warning.detail ? `\n${warning.detail}` : ""}\n`,
	);
});
function startSignal(name: string): void {
	if (signalHandles.has(name) || !Object.hasOwn(native.signals, name)) return;
	const handle = native.createSignal(
		native.signals[name],
		bind(() => {
			if (name === "SIGWINCH") {
				stdoutCache?._refreshSize?.();
				stderrCache?._refreshSize?.();
			}
			process.emit(name, name);
		}),
	);
	handle.unref();
	signalHandles.set(name, handle);
}
process.on("newListener", (event: unknown) => {
	if (typeof event === "string" && Object.hasOwn(native.signals, event)) startSignal(event);
});
process.on("removeListener", (event: unknown) => {
	if (
		typeof event === "string" &&
		process.listenerCount(event) === 0 &&
		signalHandles.has(event) &&
		!(event === "SIGWINCH" && (stdoutCache?.isTTY || stderrCache?.isTTY))
	) {
		signalHandles.get(event)!.close();
		signalHandles.delete(event);
	}
});
// Accessing a terminal output installs resize handling even without a process SIGWINCH listener.
for (const name of ["stdin", "stdout", "stderr"] as const) {
	Object.defineProperty(process, name, {
		configurable: true,
		enumerable: true,
		get() {
			const stream =
				name === "stdin"
					? (stdinCache ??= stdio("Stdin"))
					: name === "stdout"
						? (stdoutCache ??= stdio("Stdout"))
						: (stderrCache ??= stdio("Stderr"));
			if (name !== "stdin" && stream.isTTY) startSignal("SIGWINCH");
			return stream;
		},
		set(value: StdioStream) {
			if (name === "stdin") stdinCache = value;
			else if (name === "stdout") stdoutCache = value;
			else stderrCache = value;
		},
	});
}
export function emitUncaught(error: unknown): boolean {
	process.emit("uncaughtExceptionMonitor", error, "uncaughtException");
	if (uncaughtCapture) {
		uncaughtCapture(error);
		return true;
	}
	if (!process.listenerCount("uncaughtException")) return false;
	process.emit("uncaughtException", error, "uncaughtException");
	return true;
}
export function emitUnhandledRejection(reason: unknown, promise: Promise<unknown>): boolean {
	if (process.listenerCount("unhandledRejection")) {
		process.emit("unhandledRejection", reason, promise);
		return true;
	}
	return emitUncaught(reason);
}
export function emitBeforeExit(): void {
	if (!process._exiting) process.emit("beforeExit", normalizeExitCode(process.exitCode));
}
export function emitExit(): number {
	if (!process._exiting) {
		process._exiting = true;
		try {
			process.emit("exit", normalizeExitCode(process.exitCode));
		} finally {
			netBinding.resetTTY();
		}
	}
	return normalizeExitCode(process.exitCode);
}
export const argv = process.argv,
	argv0 = process.argv0,
	execArgv = process.execArgv,
	execPath = process.execPath;
export const pid = process.pid,
	ppid = process.ppid,
	arch = process.arch,
	platform = process.platform,
	version = process.version,
	versions = process.versions;
export { env };
export const cwd = process.cwd.bind(process),
	chdir = process.chdir.bind(process),
	exit = process.exit.bind(process),
	abort = process.abort.bind(process);
export const nextTick = process.nextTick.bind(process),
	hrtime = process.hrtime,
	uptime = process.uptime.bind(process),
	memoryUsage = process.memoryUsage;
export const cpuUsage = process.cpuUsage.bind(process),
	resourceUsage = process.resourceUsage.bind(process),
	kill = process.kill.bind(process),
	umask = process.umask.bind(process);
export const getuid = process.getuid.bind(process),
	geteuid = process.geteuid.bind(process),
	getgid = process.getgid.bind(process),
	getegid = process.getegid.bind(process),
	getgroups = process.getgroups.bind(process);
export const emitWarning = process.emitWarning.bind(process),
	dlopen = process.dlopen.bind(process),
	loadEnvFile = process.loadEnvFile.bind(process);
export const constrainedMemory = process.constrainedMemory.bind(process),
	availableMemory = process.availableMemory.bind(process),
	getActiveResourcesInfo = process.getActiveResourcesInfo.bind(process);
export const ref = process.ref.bind(process),
	unref = process.unref.bind(process),
	permission = process.permission,
	report = process.report;
export const setSourceMapsEnabled = process.setSourceMapsEnabled.bind(process),
	setUncaughtExceptionCaptureCallback = process.setUncaughtExceptionCaptureCallback.bind(process),
	hasUncaughtExceptionCaptureCallback = process.hasUncaughtExceptionCaptureCallback.bind(process);
export default process;
