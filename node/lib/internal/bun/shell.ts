// internal:bun/shell — Bun Shell (`$`).
//
// The template is rendered into a POSIX sh script and run with `/bin/sh -c`:
// every interpolated value is quoted for the context it appears in (plain
// word, '...' or "..."), arrays become one quoted word per element and
// `{ raw }` is inserted verbatim. JavaScript objects after a redirection
// operator are wired through extra descriptors (`/dev/fd/N`):
//   `< ${buffer | Blob | Response | Request | ReadableStream | Subprocess}` feeds the bytes,
//   `> ${buffer}` fills the buffer, `> ${Bun.file(path)}` / `< ${Bun.file(path)}` use the path,
//   `Bun.file(fd)` passes the descriptor.
// Output is captured into Buffers and also written to the process' stdout /
// stderr unless .quiet(). Commands run with stdin from /dev/null unless the
// ShellPromise's `stdin` WritableStream is used.

import * as core from "qbun:core";
import { invalidArgType } from "internal:errors";
import { bind } from "internal:async-context";
import {
	bunFileTarget,
	bytesOf,
	concatBuffers,
	createPipe,
	envPairs,
	isBlob,
	isBody,
	isBunFile,
	isReadableStream,
	processNative,
	pumpInput,
	readPipe,
	shutdownPipe,
	signals,
	writePipe,
	type NodeBuffer,
	type ProcessHandle,
	type StdioSpec,
	type StreamHandle,
} from "internal:child/common";

type Env = Record<string, string | undefined>;

// ---- output ----

class ShellOutput {
	readonly stdout: NodeBuffer;
	readonly stderr: NodeBuffer;
	readonly exitCode: number;

	constructor(stdout: NodeBuffer, stderr: NodeBuffer, exitCode: number) {
		this.stdout = stdout;
		this.stderr = stderr;
		this.exitCode = exitCode;
	}

	text(encoding?: string): string {
		return this.stdout.toString(encoding);
	}

	json(): unknown {
		return JSON.parse(this.stdout.toString());
	}

	arrayBuffer(): ArrayBuffer {
		const s = this.stdout;
		return s.buffer.slice(s.byteOffset, s.byteOffset + s.byteLength) as ArrayBuffer;
	}

	bytes(): Uint8Array {
		return new Uint8Array(this.arrayBuffer());
	}

	blob(): Blob {
		return new Blob([this.stdout], { type: "text/plain" });
	}
}

function lazyBufferToHumanReadableString(this: NodeBuffer): string {
	return this.toString();
}

class ShellError extends Error {
	#output: ShellOutput | undefined;
	declare info: { exitCode: number; stderr: NodeBuffer; stdout: NodeBuffer };
	exitCode!: number;
	stdout!: NodeBuffer;
	stderr!: NodeBuffer;

	constructor() {
		super("");
	}

	initialize(output: ShellOutput, code: number): void {
		this.message = `Failed with exit code ${code}`;
		this.#output = output;
		this.name = "ShellError";
		Object.defineProperty(this, "info", {
			value: { exitCode: code, stderr: output.stderr, stdout: output.stdout },
			writable: true,
			enumerable: false,
			configurable: true,
		});
		(this.info.stdout as NodeBuffer & { toJSON?: unknown }).toJSON = lazyBufferToHumanReadableString;
		(this.info.stderr as NodeBuffer & { toJSON?: unknown }).toJSON = lazyBufferToHumanReadableString;
		this.stdout = output.stdout;
		this.stderr = output.stderr;
		this.exitCode = code;
	}

	text(encoding?: string): string {
		return this.#output!.text(encoding);
	}

	json(): unknown {
		return this.#output!.json();
	}

	arrayBuffer(): ArrayBuffer {
		return this.#output!.arrayBuffer();
	}

	bytes(): Uint8Array {
		return this.#output!.bytes();
	}

	blob(): Blob {
		return this.#output!.blob();
	}
}

// ---- rendering ----

type Extra = { kind: "input"; source: unknown } | { kind: "output"; target: Uint8Array } | { kind: "fd"; fd: number };

interface Rendered {
	script: string;
	extras: Extra[];
}

function singleQuote(s: string): string {
	return `'${s.replaceAll("'", "'\\''")}'`;
}

function doubleQuoteBody(s: string): string {
	return s.replace(/[\\"$`]/g, "\\$&");
}

function isRaw(value: unknown): value is { raw: unknown } {
	return (
		value !== null && typeof value === "object" && "raw" in value && !Array.isArray(value) && !ArrayBuffer.isView(value)
	);
}

function stringOf(value: unknown): string {
	if (typeof value === "string") return value;
	if (value === null || value === undefined) return "";
	return String(value);
}

/** The words of one interpolated value, quoted for an unquoted context. */
function renderWord(value: unknown): string {
	if (Array.isArray(value)) return value.map(renderWord).join(" ");
	if (isRaw(value)) return stringOf(value.raw);
	return singleQuote(stringOf(value));
}

function renderQuoted(value: unknown, quote: "'" | '"'): string {
	const text = Array.isArray(value)
		? value.map((v) => (isRaw(v) ? stringOf(v.raw) : stringOf(v))).join(" ")
		: isRaw(value)
			? stringOf(value.raw)
			: stringOf(value);
	return quote === "'" ? text.replaceAll("'", "'\\''") : doubleQuoteBody(text);
}

/** A Bun.spawn() Subprocess: its stdout stream is the input. */
function subprocessStdout(value: unknown): ReadableStream<Uint8Array> | undefined {
	if (value === null || typeof value !== "object" || !("pid" in value) || !("exited" in value) || !("stdout" in value))
		return undefined;
	const stdout = value.stdout;
	return isReadableStream(stdout) ? (stdout as ReadableStream<Uint8Array>) : undefined;
}

function render(raw: readonly string[], values: readonly unknown[]): Rendered {
	const extras: Extra[] = [];
	let script = "";
	let quote: "" | "'" | '"' = "";
	const extraFd = (extra: Extra): string => {
		extras.push(extra);
		return `/dev/fd/${2 + extras.length}`;
	};
	for (let i = 0; i < raw.length; i++) {
		const text = raw[i];
		// track the quoting state through the literal text
		for (let j = 0; j < text.length; j++) {
			const c = text[j];
			if (quote === "'") {
				if (c === "'") quote = "";
			} else if (c === "\\") {
				j++;
			} else if (quote === '"') {
				if (c === '"') quote = "";
			} else if (c === "'" || c === '"') {
				quote = c;
			}
		}
		script += text;
		if (i >= values.length) break;
		const value = values[i];
		if (quote) {
			script += renderQuoted(value, quote);
			continue;
		}
		const before = text.trimEnd();
		const redirectIn = before.endsWith("<") && !before.endsWith("<<");
		const redirectOut = before.endsWith(">");
		if (redirectIn || redirectOut) {
			if (isBunFile(value)) {
				const target = bunFileTarget(value);
				script += "fd" in target ? extraFd({ kind: "fd", fd: target.fd }) : singleQuote(target.path);
				continue;
			}
			if (redirectOut) {
				const target = bytesOf(value);
				if (target) {
					script += extraFd({ kind: "output", target });
					continue;
				}
			} else {
				const stream = subprocessStdout(value);
				if (stream) {
					script += extraFd({ kind: "input", source: stream });
					continue;
				}
				if (bytesOf(value) || isBody(value) || isBlob(value) || isReadableStream(value)) {
					script += extraFd({ kind: "input", source: value });
					continue;
				}
			}
		}
		script += renderWord(value);
	}
	return { script, extras };
}

/** Bun's $.escape: the input unchanged, or double-quoted with \ " $ ` escaped. */
function escape(input: string): string {
	if (typeof input !== "string") throw invalidArgType("input", "of type string", input);
	if (!/[\s$><&|=;{},()\\"'`~*?[\]#!^]/.test(input)) return input;
	return `"${doubleQuoteBody(input)}"`;
}

// ---- brace expansion ----

function findBrace(s: string): { open: number; close: number; parts: string[] } | null {
	for (let open = 0; open < s.length; open++) {
		const c = s[open];
		if (c === "\\") {
			open++;
			continue;
		}
		if (c !== "{") continue;
		let depth = 0;
		const parts: string[] = [];
		let start = open + 1;
		for (let k = open + 1; k < s.length; k++) {
			const d = s[k];
			if (d === "\\") {
				k++;
				continue;
			}
			if (d === "{") depth++;
			else if (d === "}") {
				if (depth === 0) {
					parts.push(s.slice(start, k));
					if (parts.length > 1) return { open, close: k, parts };
					const seq = sequence(parts[0]);
					if (seq) return { open, close: k, parts: seq };
					break;
				}
				depth--;
			} else if (d === "," && depth === 0) {
				parts.push(s.slice(start, k));
				start = k + 1;
			}
		}
	}
	return null;
}

function sequence(body: string): string[] | null {
	const m = /^(-?\d+|[A-Za-z])\.\.(-?\d+|[A-Za-z])(?:\.\.(-?\d+))?$/.exec(body);
	if (!m) return null;
	const step = Math.abs(Number(m[3] ?? 1)) || 1;
	const numeric = /\d/.test(m[1]);
	if (numeric !== /\d/.test(m[2])) return null;
	const a = numeric ? Number(m[1]) : m[1].charCodeAt(0);
	const b = numeric ? Number(m[2]) : m[2].charCodeAt(0);
	const width = numeric && (/^-?0\d/.test(m[1]) || /^-?0\d/.test(m[2])) ? Math.max(m[1].length, m[2].length) : 0;
	const out: string[] = [];
	const dir = a <= b ? 1 : -1;
	for (let v = a; dir > 0 ? v <= b : v >= b; v += dir * step) {
		if (!numeric) out.push(String.fromCharCode(v));
		else if (width) out.push((v < 0 ? "-" : "") + String(Math.abs(v)).padStart(width - (v < 0 ? 1 : 0), "0"));
		else out.push(String(v));
	}
	return out;
}

/** Bun's $.braces: bash brace expansion of `pattern`. */
function braces(pattern: string): string[] {
	if (typeof pattern !== "string") throw invalidArgType("pattern", "of type string", pattern);
	const found = findBrace(pattern);
	if (!found) return [pattern];
	const prefix = pattern.slice(0, found.open);
	const suffix = pattern.slice(found.close + 1);
	const out: string[] = [];
	for (const part of found.parts) out.push(...braces(prefix + part + suffix));
	return out;
}

// ---- running ----

interface Script {
	rendered: Rendered;
	cwd: string | undefined;
	env: Env | undefined;
	quiet: boolean;
	stdin: StreamHandle | null;
}

interface RunResult {
	code: number;
	stdout: NodeBuffer;
	stderr: NodeBuffer;
}

function runScript(script: Script, onDone: (result: RunResult) => void, onError: (err: unknown) => void): void {
	const cwd = script.cwd ?? process.cwd();
	const env = envPairs((script.env ?? process.env) as Record<string, unknown>);
	const stdoutPipe = createPipe(false);
	const stderrPipe = createPipe(false);
	const stdio: StdioSpec[] = [
		script.stdin ? { type: "pipe", handle: script.stdin, readable: true, writable: false } : { type: "ignore" },
		{ type: "pipe", handle: stdoutPipe, readable: false, writable: true },
		{ type: "pipe", handle: stderrPipe, readable: false, writable: true },
	];
	const extraHandles: (StreamHandle | null)[] = [];
	for (const extra of script.rendered.extras) {
		if (extra.kind === "fd") {
			stdio.push({ type: "fd", fd: extra.fd });
			extraHandles.push(null);
		} else {
			const handle = createPipe(false);
			extraHandles.push(handle);
			stdio.push(
				extra.kind === "input"
					? { type: "pipe", handle, readable: true, writable: false }
					: { type: "pipe", handle, readable: false, writable: true },
			);
		}
	}

	const stdoutChunks: Uint8Array[] = [];
	const stderrChunks: Uint8Array[] = [];
	const tee = (fd: number, chunk: Uint8Array): void => {
		if (script.quiet) return;
		try {
			core.writeSync(fd, chunk);
		} catch {
			// the process' own stdout/stderr is gone: the output is still captured
		}
	};
	let pending = 3; // exit + stdout EOF + stderr EOF (+ output extras)
	let exitCode = 0;
	const done = (): void => {
		if (--pending !== 0) return;
		onDone({ code: exitCode, stdout: concatBuffers(stdoutChunks), stderr: concatBuffers(stderrChunks) });
	};

	let handle: ProcessHandle | undefined;
	try {
		handle = processNative.spawn(
			{ file: "/bin/sh", args: ["/bin/sh", "-c", script.rendered.script], cwd, env, stdio },
			bind((code: number, signalCode: string | null) => {
				exitCode = signalCode ? 128 + (signals[signalCode] ?? 126) : code;
				handle?.close();
				done();
			}),
		);
	} catch (err) {
		stdoutPipe.close();
		stderrPipe.close();
		script.stdin?.close();
		for (const h of extraHandles) h?.close();
		onError(err);
		return;
	}

	readPipe(
		stdoutPipe,
		bind((chunk: Uint8Array) => {
			stdoutChunks.push(chunk);
			tee(1, chunk);
		}),
		bind(() => done()),
	);
	readPipe(
		stderrPipe,
		bind((chunk: Uint8Array) => {
			stderrChunks.push(chunk);
			tee(2, chunk);
		}),
		bind(() => done()),
	);
	script.rendered.extras.forEach((extra, i) => {
		const h = extraHandles[i];
		if (!h) return;
		if (extra.kind === "input") {
			void pumpInput(h, extra.source).catch(() => {});
		} else if (extra.kind === "output") {
			const target = extra.target;
			let offset = 0;
			pending++;
			readPipe(
				h,
				(chunk) => {
					if (offset >= target.byteLength) return;
					const n = Math.min(chunk.byteLength, target.byteLength - offset);
					target.set(n === chunk.byteLength ? chunk : chunk.subarray(0, n), offset);
					offset += n;
				},
				bind(() => done()),
			);
		}
	});
}

/** The writable end of a script's stdin (ShellPromise#stdin). */
function stdinWritable(handle: StreamHandle): WritableStream<unknown> {
	return new WritableStream<unknown>({
		async write(chunk) {
			if (typeof chunk === "string") {
				if (chunk.length) await writePipe(handle, chunk);
				return;
			}
			const bytes = bytesOf(chunk);
			if (!bytes) throw invalidArgType("chunk", "a string, ArrayBuffer or ArrayBufferView", chunk);
			if (bytes.byteLength) await writePipe(handle, bytes.slice());
		},
		async close() {
			await shutdownPipe(handle);
			handle.close();
		},
		abort() {
			handle.close();
		},
	});
}

// ---- ShellPromise ----

const kCwd = Symbol("cwd");
const kEnv = Symbol("env");
const kThrows = Symbol("throws");

type ShellResolve = (result: RunResult) => void;
type ShellReject = (error: unknown) => void;

class ShellPromise extends Promise<ShellOutput> {
	#script: Script | undefined;
	#hasRun = false;
	#throws = true;
	#resolve!: ShellResolve;
	#reject!: ShellReject;
	#stdinStream: WritableStream<unknown> | undefined;

	constructor(script: Script, throws: boolean) {
		// The error is created here so that its stack points at the $`...` call.
		let potentialError: ShellError | undefined = new ShellError();
		let resolve!: ShellResolve;
		let reject!: ShellReject;
		// A Promise subclass must hand its executor to super(): the
		// Promise.withResolvers() form cannot construct a subclass instance.
		super((res, rej) => {
			resolve = ({ code, stdout, stderr }) => {
				const out = new ShellOutput(stdout, stderr, code);
				if (this.#throws && code !== 0) {
					potentialError!.initialize(out, code);
					rej(potentialError);
				} else {
					potentialError = undefined;
					res(out);
				}
			};
			reject = (error) => {
				potentialError = undefined;
				rej(error);
			};
		});
		this.#throws = throws;
		this.#script = script;
		this.#resolve = resolve;
		this.#reject = reject;
	}

	static get [Symbol.species](): PromiseConstructor {
		return Promise;
	}

	#throwIfRunning(): void {
		if (this.#hasRun) throw new Error("Shell is already running");
	}

	/** A WritableStream feeding the script's stdin (instead of /dev/null). */
	get stdin(): WritableStream<unknown> {
		if (this.#stdinStream) return this.#stdinStream;
		this.#throwIfRunning();
		const handle = createPipe(false);
		this.#script!.stdin = handle;
		this.#stdinStream = stdinWritable(handle);
		return this.#stdinStream;
	}

	cwd(newCwd?: string): this {
		this.#throwIfRunning();
		if (newCwd === undefined || newCwd === "." || newCwd === "" || newCwd === "./") newCwd = process.cwd();
		if (typeof newCwd !== "string") throw new TypeError("cwd must be a string or undefined");
		this.#script!.cwd = newCwd;
		return this;
	}

	env(newEnv?: Env): this {
		this.#throwIfRunning();
		if (newEnv !== undefined && (newEnv === null || typeof newEnv !== "object"))
			throw new TypeError("env must be an object or undefined");
		// undefined: the live process.env
		this.#script!.env = newEnv;
		return this;
	}

	#run(): void {
		if (this.#hasRun) return;
		this.#hasRun = true;
		const script = this.#script!;
		this.#script = undefined;
		try {
			runScript(script, this.#resolve, this.#reject);
		} catch (e) {
			this.#reject(e);
		}
	}

	#quiet(isQuiet = true): this {
		this.#throwIfRunning();
		this.#script!.quiet = isQuiet;
		return this;
	}

	quiet(isQuiet?: boolean): this {
		return this.#quiet(isQuiet ?? true);
	}

	nothrow(): this {
		this.#throws = false;
		return this;
	}

	throws(doThrow?: boolean): this {
		this.#throws = !!doThrow;
		return this;
	}

	async #output(): Promise<ShellOutput> {
		if (!this.#hasRun) this.#quiet(true);
		return await this;
	}

	async text(encoding?: string): Promise<string> {
		return (await this.#output()).stdout.toString(encoding);
	}

	async json(): Promise<unknown> {
		return JSON.parse((await this.#output()).stdout.toString());
	}

	async *lines(): AsyncGenerator<string> {
		const { stdout } = await this.#output();
		yield* stdout.toString().split("\n");
	}

	async arrayBuffer(): Promise<ArrayBuffer> {
		return (await this.#output()).arrayBuffer();
	}

	async bytes(): Promise<Uint8Array> {
		return (await this.#output()).bytes();
	}

	async blob(): Promise<Blob> {
		return (await this.#output()).blob();
	}

	run(): this {
		this.#run();
		return this;
	}

	then<TResult1 = ShellOutput, TResult2 = never>(
		onfulfilled?: ((value: ShellOutput) => TResult1 | PromiseLike<TResult1>) | null,
		onrejected?: ((reason: unknown) => TResult2 | PromiseLike<TResult2>) | null,
	): Promise<TResult1 | TResult2> {
		this.#run();
		return super.then(onfulfilled, onrejected);
	}
}

// ---- $ ----

interface ShellState {
	[kCwd]: string | undefined;
	[kEnv]: Env | undefined;
	[kThrows]: boolean;
}

type ShellFn = ((strings: TemplateStringsArray, ...values: unknown[]) => ShellPromise) & ShellState & ShellPrototype;

class ShellPrototype {
	env(this: ShellFn, newEnv?: Env): ShellFn {
		if (newEnv === undefined || newEnv === process.env) this[kEnv] = undefined;
		else if (newEnv && typeof newEnv === "object") this[kEnv] = Object.assign({}, newEnv);
		else throw new TypeError("env must be an object or undefined");
		return this;
	}

	cwd(this: ShellFn, newCwd?: string): ShellFn {
		if (newCwd === undefined || typeof newCwd === "string") {
			if (newCwd === "." || newCwd === "" || newCwd === "./") newCwd = process.cwd();
			this[kCwd] = newCwd;
		} else {
			throw new TypeError("cwd must be a string or undefined");
		}
		return this;
	}

	nothrow(this: ShellFn): ShellFn {
		this[kThrows] = false;
		return this;
	}

	throws(this: ShellFn, doThrow?: boolean): ShellFn {
		this[kThrows] = !!doThrow;
		return this;
	}
}

function createShell(proto: object): ShellFn {
	const shell = function Shell(first: TemplateStringsArray, ...rest: unknown[]): ShellPromise {
		if (first?.raw === undefined) throw new Error("Please use '$' as a tagged template function: $`cmd arg1 arg2`");
		const rendered = render(first.raw, rest);
		return new ShellPromise(
			{ rendered, cwd: shell[kCwd], env: shell[kEnv], quiet: false, stdin: null },
			shell[kThrows],
		);
	} as ShellFn;
	Object.setPrototypeOf(shell, proto);
	shell[kCwd] = undefined;
	shell[kEnv] = undefined;
	shell[kThrows] = true;
	return shell;
}

/** `new $.Shell()`: an independent `$` with its own cwd/env/throws defaults. */
function Shell(this: unknown): ShellFn {
	if (!new.target) throw new TypeError("Class constructor Shell cannot be invoked without 'new'");
	const prototype = new.target.prototype as unknown;
	const shell = createShell(prototype !== null && typeof prototype === "object" ? prototype : ShellPrototype.prototype);
	Object.defineProperty(shell, "name", { value: "Shell", configurable: true, enumerable: true });
	return shell;
}
Shell.prototype = ShellPrototype.prototype;
Object.setPrototypeOf(Shell, ShellPrototype);

export const $: ShellFn = createShell(ShellPrototype.prototype);
Object.defineProperties($, {
	Shell: { value: Shell, enumerable: true },
	ShellPromise: { value: ShellPromise, enumerable: true },
	ShellError: { value: ShellError, enumerable: true },
	braces: { value: braces, enumerable: true },
	escape: { value: escape, enumerable: true },
});
