// node:net — TCP and IPC (unix domain socket) streams over the libuv stream
// handles of qbun:net. Adapted from Node.js lib/net.js (MIT).
//
// A Socket does all its I/O through `this._handle`, an object implementing
// the qbun:net StreamHandle interface. Subclasses (tls.TLSSocket, tty
// streams, child_process pipes) pass their own handle as `{ handle }`.

import * as binding from "qbun:net";
import { chmodSync } from "node:fs";
import { EventEmitter, captureRejectionSymbol } from "node:events";
import { Duplex } from "node:stream";
import { setTimeout as setTimer, clearTimeout as clearTimer, Timeout } from "node:timers";
import * as dns from "node:dns";
import { isIP, isIPv4, isIPv6, parseIPv4, formatIPv4, parseIPv6, formatIPv6 } from "internal:http/ip";
import {
	codedError,
	describeReceived,
	invalidArgType,
	invalidArgValue,
	validateFunction,
	validateNumber,
	validateObject,
	validateString,
	abortError,
} from "internal:errors";

// ---------------------------------------------------------------------------
// handle types (qbun:net)

export interface SockAddr {
	address: string;
	family: "IPv4" | "IPv6";
	port: number;
}

export interface StreamHandle {
	readonly fd: number;
	onread: ((chunk: Buffer | null, err?: Error) => void) | null;
	/** read state, kept on the handle like Node (tty.ReadStream resets it) */
	reading?: boolean;
	readStart(): void;
	readStop(): void;
	write(data: Uint8Array | string, cb: (err: Error | null) => void): void;
	writev(chunks: (Uint8Array | string)[], cb: (err: Error | null) => void): void;
	tryWrite?(data: Uint8Array | string): number;
	readonly writeQueueSize: number;
	shutdown(cb: (err: Error | null) => void): void;
	close(cb?: () => void): void;
	ref(): void;
	unref(): void;
	hasRef(): boolean;
	setBlocking?(blocking: boolean): void;
	bind?(hostOrPath: string, port?: number, ipv6Only?: boolean): void;
	listen?(backlog: number, onconnection: (err: Error | null, client?: StreamHandle) => void): void;
	connect?(hostOrPath: string, port: number | undefined, cb: (err: Error | null) => void): void;
	getsockname?(): SockAddr | null;
	getpeername?(): SockAddr | null;
	setNoDelay?(enable: boolean): void;
	setKeepAlive?(enable: boolean, delaySecs: number): void;
	open?(fd: number): void;
	reset?(cb?: () => void): void;
}

interface NetBinding {
	guessHandleType(fd: number): "TTY" | "PIPE" | "TCP" | "UDP" | "FILE" | "UNKNOWN";
	createTCP(): StreamHandle;
	createPipe(ipc?: boolean): StreamHandle;
	createTTY(fd: number, readable: boolean): StreamHandle;
}

const native = binding as unknown as NetBinding;

export { isIP, isIPv4, isIPv6 };

type SystemError = Error & { code?: string; errno?: number; syscall?: string; address?: string; port?: number };
type Callback = (...args: unknown[]) => void;

// ---------------------------------------------------------------------------
// errors

function invalidIPAddress(ip: unknown): Error {
	return codedError("ERR_INVALID_IP_ADDRESS", `Invalid IP address: ${String(ip)}`, TypeError);
}

function socketClosed(): Error {
	return codedError("ERR_SOCKET_CLOSED", "Socket is closed");
}

/** the libuv message of a qb_uv_error() ("ECODE: message, syscall ...") */
function uvMessage(err: SystemError): string {
	let s = err.message;
	const code = err.code ?? "";
	if (code && s.startsWith(`${code}: `)) s = s.slice(code.length + 2);
	const i = s.lastIndexOf(", ");
	if (i >= 0) s = s.slice(0, i);
	return s;
}

function copySystemFields(from: SystemError, to: SystemError, syscall: string): void {
	to.errno = from.errno;
	to.code = from.code;
	to.syscall = syscall;
}

/** Node's ErrnoException: "<syscall> <CODE>" */
export function errnoException(err: unknown, syscall: string): Error {
	if (!(err instanceof Error)) return new Error(`${syscall} ${String(err)}`);
	const e = err as SystemError;
	if (!e.code) return err;
	const ex = new Error(`${syscall} ${e.code}`) as SystemError;
	copySystemFields(e, ex, syscall);
	return ex;
}

/** Node's ExceptionWithHostPort: "<syscall> <CODE> <address>:<port>" */
export function exceptionWithHostPort(
	err: unknown,
	syscall: string,
	address: string,
	port?: number,
	additional?: string,
): Error {
	if (!(err instanceof Error) || !(err as SystemError).code) return err instanceof Error ? err : new Error(String(err));
	const e = err as SystemError;
	let details = port && port > 0 ? `${address}:${port}` : address;
	if (additional) details += ` - Local (${additional})`;
	const ex = new Error(`${syscall} ${e.code} ${details}`) as SystemError;
	copySystemFields(e, ex, syscall);
	ex.address = address;
	if (port) ex.port = port;
	return ex;
}

/** Node's UVExceptionWithHostPort: "<syscall> <CODE>: <message> <address>:<port>" */
function uvExceptionWithHostPort(err: unknown, syscall: string, address: string | null, port: number): Error {
	if (!(err instanceof Error) || !(err as SystemError).code) return err instanceof Error ? err : new Error(String(err));
	const e = err as SystemError;
	let details = "";
	if (port && port > 0) details = ` ${address}:${port}`;
	else if (address) details = ` ${address}`;
	const ex = new Error(`${syscall} ${e.code}: ${uvMessage(e)}${details}`) as SystemError;
	copySystemFields(e, ex, syscall);
	if (address) ex.address = address;
	if (port) ex.port = port;
	return ex;
}

export function validatePort(port: unknown, name = "Port", allowZero = true): number {
	if (
		(typeof port !== "number" && typeof port !== "string") ||
		(typeof port === "string" && port.trim().length === 0) ||
		+port !== +port >>> 0 ||
		+port > 0xffff ||
		(+port === 0 && !allowZero)
	) {
		const operator = allowZero ? ">=" : ">";
		throw codedError(
			"ERR_SOCKET_BAD_PORT",
			`${name} should be ${operator} 0 and < 65536. ${describeReceived(port)}.`,
			RangeError,
		);
	}
	return +port | 0;
}

// ---------------------------------------------------------------------------
// argument normalization

const kNormalized = Symbol("normalizedArgs");

export interface TcpSocketConnectOpts {
	port?: number | string;
	host?: string;
	localAddress?: string;
	localPort?: number;
	hints?: number;
	family?: number | string;
	lookup?: LookupFunction;
	noDelay?: boolean;
	keepAlive?: boolean;
	keepAliveInitialDelay?: number;
	autoSelectFamily?: boolean;
	autoSelectFamilyAttemptTimeout?: number;
	timeout?: number;
	path?: string;
	onread?: OnReadOpts;
	blockList?: BlockList;
	[key: string]: unknown;
}

interface OnReadOpts {
	buffer: Uint8Array | (() => Uint8Array);
	callback(bytesWritten: number, buf: Uint8Array): boolean | void;
}

type LookupCallback = (
	err: Error | null,
	address: string | { address: string; family: number }[],
	family?: number,
) => void;
export type LookupFunction = (hostname: string, options: Record<string, unknown>, callback: LookupCallback) => void;

type NormalizedArgs = [TcpSocketConnectOpts, Callback | null] & { [kNormalized]?: true };

function toNumber(x: unknown): number | false {
	const n = Number(x);
	return n >= 0 ? n : false;
}

function isPipeName(s: unknown): s is string {
	return typeof s === "string" && toNumber(s) === false;
}

function normalizeArgs(args: unknown[]): NormalizedArgs {
	let arr: NormalizedArgs;
	if (args.length === 0) {
		arr = [{}, null];
		arr[kNormalized] = true;
		return arr;
	}
	const arg0 = args[0];
	let options: TcpSocketConnectOpts = {};
	if (typeof arg0 === "object" && arg0 !== null) {
		options = arg0 as TcpSocketConnectOpts;
	} else if (isPipeName(arg0)) {
		options.path = arg0;
	} else {
		options.port = arg0 as number;
		if (args.length > 1 && typeof args[1] === "string") options.host = args[1];
	}
	const cb = args[args.length - 1];
	arr = typeof cb !== "function" ? [options, null] : [options, cb as Callback];
	arr[kNormalized] = true;
	return arr;
}

// ---------------------------------------------------------------------------
// auto select family (happy eyeballs, sequential)

let autoSelectFamilyDefault = true;
let autoSelectFamilyAttemptTimeoutDefault = 250;

export function getDefaultAutoSelectFamily(): boolean {
	return autoSelectFamilyDefault;
}

export function setDefaultAutoSelectFamily(value: boolean): void {
	if (typeof value !== "boolean") throw invalidArgType("value", "of type boolean", value);
	autoSelectFamilyDefault = value;
}

export function getDefaultAutoSelectFamilyAttemptTimeout(): number {
	return autoSelectFamilyAttemptTimeoutDefault;
}

export function setDefaultAutoSelectFamilyAttemptTimeout(value: number): void {
	validateNumber(value, "value", 1);
	autoSelectFamilyAttemptTimeoutDefault = value < 10 ? 10 : value;
}

// ---------------------------------------------------------------------------
// Socket

export interface SocketConstructorOpts {
	fd?: number;
	handle?: StreamHandle;
	allowHalfOpen?: boolean;
	readable?: boolean;
	writable?: boolean;
	signal?: AbortSignal;
	manualStart?: boolean;
	pauseOnCreate?: boolean;
	onread?: OnReadOpts;
	noDelay?: boolean;
	keepAlive?: boolean;
	keepAliveInitialDelay?: number;
	blockList?: BlockList;
	[key: string]: unknown;
}

type WriteCb = (err?: Error | null) => void;
interface WritevEntry {
	chunk: Uint8Array | string;
	encoding: string;
}

function toWritable(chunk: Uint8Array | string, encoding: string): Uint8Array | string {
	if (typeof chunk !== "string") return chunk;
	if (!encoding || encoding === "utf8" || encoding === "utf-8" || encoding === "buffer") return chunk;
	return Buffer.from(chunk, encoding as BufferEncoding);
}

function chunkLength(chunk: Uint8Array | string, encoding?: string): number {
	return typeof chunk === "string"
		? Buffer.byteLength(chunk, (encoding || "utf8") as BufferEncoding)
		: chunk.byteLength;
}

function writeAfterFIN(this: Socket, chunk: unknown, encoding?: unknown, cb?: unknown): boolean {
	if (!this.writableEnded) {
		return (Duplex.prototype.write as (...a: unknown[]) => boolean).call(this, chunk, encoding, cb);
	}
	if (typeof encoding === "function") {
		cb = encoding;
		encoding = null;
	}
	const er = codedError("EPIPE", "This socket has been ended by the other party");
	if (typeof cb === "function") process.nextTick(cb as Callback, er);
	this.destroy(er);
	return false;
}

function onReadableStreamEnd(this: Socket): void {
	if (!this.allowHalfOpen) {
		(this as unknown as { write: unknown }).write = writeAfterFIN;
	}
}

export class Socket extends Duplex {
	_handle: StreamHandle | null = null;
	connecting = false;
	_hadError = false;
	_parent: Socket | null = null;
	_host: string | null = null;
	_peername: SockAddr | null = null;
	_sockname: SockAddr | null = null;
	_pendingData: unknown = null;
	_pendingEncoding = "";
	_closeAfterHandlingError = false;
	server: EventEmitter | null = null;
	_server: Server | null = null;
	_httpMessage: unknown = null;
	timeout: number | undefined = undefined;
	bytesReadTotal = 0;
	_bytesDispatched = 0;
	autoSelectFamilyAttemptedAddresses: string[] | undefined = undefined;
	resetAndClosing = false;
	blockList: BlockList | undefined = undefined;

	#timer: Timeout | null = null;
	#noDelay: boolean | undefined = undefined;
	#keepAlive = false;
	#keepAliveDelay = 0;
	#onread: OnReadOpts | null = null;
	#ownHandle = false;
	#connectAttempts: { timer: Timeout | null; canceled: boolean } | null = null;

	constructor(options?: SocketConstructorOpts | number) {
		let opts: SocketConstructorOpts;
		if (typeof options === "number") opts = { fd: options };
		else if (options === undefined || options === null) opts = {};
		else {
			validateObject(options, "options");
			opts = { ...options };
		}
		const duplexOpts: Record<string, unknown> = { ...opts };
		delete duplexOpts.signal;
		delete duplexOpts.handle;
		duplexOpts.allowHalfOpen = Boolean(opts.allowHalfOpen);
		duplexOpts.emitClose = false;
		duplexOpts.autoDestroy = true;
		duplexOpts.decodeStrings = false;
		super(duplexOpts);

		if (opts.handle) {
			this._handle = opts.handle;
		} else if (opts.fd !== undefined) {
			const fd = opts.fd;
			validateNumber(fd, "fd", 0);
			this._handle = createHandleForFd(fd, opts.readable !== false);
			this.#ownHandle = true;
		}

		const onread = opts.onread;
		if (onread !== null && typeof onread === "object") {
			if (typeof onread.buffer !== "function" && !(onread.buffer instanceof Uint8Array)) {
				throw invalidArgType("options.onread.buffer", "an instance of Uint8Array or of type function", onread.buffer);
			}
			validateFunction(onread.callback, "options.onread.callback");
			this.#onread = onread;
		}

		this.on("end", onReadableStreamEnd);
		this.#initHandle();

		if (this._handle && opts.readable !== false) {
			if (opts.pauseOnCreate) {
				this._handle.reading = false;
				this._handle.readStop();
				(this as unknown as { readableFlowing: boolean | null }).readableFlowing = false;
			} else if (!opts.manualStart) {
				this.read(0);
			}
		}

		if (opts.noDelay !== undefined) this.#noDelay = Boolean(opts.noDelay);
		if (opts.keepAlive) {
			this.#keepAlive = true;
			this.#keepAliveDelay = ~~((opts.keepAliveInitialDelay ?? 0) / 1000);
		}
		if (opts.blockList) {
			if (!BlockList.isBlockList(opts.blockList))
				throw invalidArgType("options.blockList", "an instance of net.BlockList", opts.blockList);
			this.blockList = opts.blockList;
		}

		const signal = opts.signal;
		if (signal) {
			if (typeof signal !== "object" || !("aborted" in signal))
				throw invalidArgType("options.signal", "an instance of AbortSignal", signal);
			const onAbort = () => this.destroy(abortError(signal.reason));
			if (signal.aborted) process.nextTick(onAbort);
			else {
				signal.addEventListener("abort", onAbort, { once: true });
				this.once("close", () => signal.removeEventListener("abort", onAbort));
			}
		}
	}

	/** hook the current handle's reads up to this socket */
	#initHandle(): void {
		const u = this as unknown as { _undestroy?: () => void };
		if (typeof u._undestroy === "function") u._undestroy();
		this._sockname = null;
		const handle = this._handle;
		if (!handle) return;
		if (handle.reading === undefined) handle.reading = false;
		handle.onread = (chunk, err) => this.#onRead(handle, chunk, err);
	}

	#onRead(handle: StreamHandle, chunk: Buffer | null, err?: Error): void {
		if (handle !== this._handle) return;
		if (err) {
			this.destroy(errnoException(err, "read"));
			return;
		}
		if (chunk === null) {
			// EOF
			handle.reading = false;
			try {
				handle.readStop();
			} catch {}
			this.push(null);
			this.read(0);
			return;
		}
		this._unrefTimer();
		this.bytesReadTotal += chunk.length;
		if (this.#onread) {
			this.#deliverOnread(handle, chunk);
			return;
		}
		if (!this.push(chunk)) {
			handle.reading = false;
			if (!this.destroyed) handle.readStop();
		}
	}

	#deliverOnread(handle: StreamHandle, chunk: Buffer): void {
		const opts = this.#onread as OnReadOpts;
		let off = 0;
		while (off < chunk.length) {
			const buf = typeof opts.buffer === "function" ? opts.buffer() : opts.buffer;
			const n = Math.min(buf.byteLength, chunk.length - off);
			if (n <= 0) break;
			buf.set(chunk.subarray(off, off + n), 0);
			off += n;
			if (opts.callback.call(this, n, buf) === false) {
				handle.reading = false;
				handle.readStop();
				super.pause();
			}
		}
	}

	get bytesRead(): number {
		return this.bytesReadTotal;
	}

	get bytesWritten(): number | undefined {
		let bytes = this._bytesDispatched;
		const buffered = (this as unknown as { writableBuffer?: { chunk: Uint8Array | string; encoding: string }[] })
			.writableBuffer;
		if (buffered) for (const el of buffered) bytes += chunkLength(el.chunk, el.encoding);
		const data = this._pendingData;
		if (Array.isArray(data)) {
			for (const el of data as WritevEntry[]) bytes += chunkLength(el.chunk, el.encoding);
		} else if (data) {
			bytes += chunkLength(data as Uint8Array | string, this._pendingEncoding);
		}
		return bytes;
	}

	get _connecting(): boolean {
		return this.connecting;
	}

	get pending(): boolean {
		return !this._handle || this.connecting;
	}

	get readyState(): string {
		if (this.connecting) return "opening";
		if (this.readable && this.writable) return "open";
		if (this.readable && !this.writable) return "readOnly";
		if (!this.readable && this.writable) return "writeOnly";
		return "closed";
	}

	get bufferSize(): number | undefined {
		return this._handle ? this.writableLength : undefined;
	}

	get remoteAddress(): string | undefined {
		return this._getpeername().address;
	}

	get remoteFamily(): string | undefined {
		return this._getpeername().family;
	}

	get remotePort(): number | undefined {
		return this._getpeername().port;
	}

	get localAddress(): string | undefined {
		return this._getsockname().address;
	}

	get localPort(): number | undefined {
		return this._getsockname().port;
	}

	get localFamily(): string | undefined {
		return this._getsockname().family;
	}

	_getpeername(): Partial<SockAddr> {
		if (!this._handle || typeof this._handle.getpeername !== "function" || this.connecting) {
			return this._peername || {};
		}
		if (!this._peername) {
			const out = this._handle.getpeername();
			if (!out) return {};
			this._peername = out;
		}
		return this._peername;
	}

	_getsockname(): Partial<SockAddr> {
		if (!this._handle || typeof this._handle.getsockname !== "function") return {};
		if (!this._sockname) {
			const out = this._handle.getsockname();
			if (!out) return {};
			this._sockname = out;
		}
		return this._sockname;
	}

	address(): Partial<SockAddr> {
		return this._getsockname();
	}

	// -- timeouts ------------------------------------------------------------

	_unrefTimer(): void {
		for (let s: Socket | null = this; s !== null; s = s._parent) {
			if (#timer in s && s.#timer) s.#timer.refresh();
		}
	}

	setTimeout(msecs: number, callback?: () => void): this {
		if (this.destroyed) return this;
		validateNumber(msecs, "msecs", 0);
		if (!Number.isFinite(msecs)) msecs = 0;
		this.timeout = msecs;
		if (this.#timer) {
			clearTimer(this.#timer);
			this.#timer = null;
		}
		if (msecs === 0) {
			if (callback !== undefined) {
				validateFunction(callback, "callback");
				this.removeListener("timeout", callback);
			}
		} else {
			this.#timer = setTimer(() => this._onTimeout(), msecs);
			this.#timer.unref();
			if (callback !== undefined) {
				validateFunction(callback, "callback");
				this.once("timeout", callback);
			}
		}
		return this;
	}

	_onTimeout(): void {
		this.emit("timeout");
	}

	// -- options -------------------------------------------------------------

	setNoDelay(enable?: boolean): this {
		const on = enable === undefined ? true : Boolean(enable);
		if (!this._handle || this.connecting) {
			this.#noDelay = on;
			return this;
		}
		if (typeof this._handle.setNoDelay === "function" && on !== this.#noDelay) {
			this.#noDelay = on;
			this._handle.setNoDelay(on);
		}
		return this;
	}

	setKeepAlive(enable?: boolean, initialDelayMsecs?: number): this {
		const on = Boolean(enable);
		const delay = ~~((initialDelayMsecs ?? 0) / 1000);
		if (!this._handle || this.connecting) {
			this.#keepAlive = on;
			this.#keepAliveDelay = delay;
			return this;
		}
		if (
			typeof this._handle.setKeepAlive === "function" &&
			(on !== this.#keepAlive || (on && delay !== this.#keepAliveDelay))
		) {
			this.#keepAlive = on;
			this.#keepAliveDelay = delay;
			this._handle.setKeepAlive(on, delay);
		}
		return this;
	}

	setTypeOfService(tos: number): this {
		validateNumber(tos, "tos", 0, 255);
		return this;
	}

	getTypeOfService(): number {
		return 0;
	}

	ref(): this {
		if (!this._handle) {
			this.once("connect", this.ref);
			return this;
		}
		this._handle.ref();
		return this;
	}

	unref(): this {
		if (!this._handle) {
			this.once("connect", this.unref);
			return this;
		}
		this._handle.unref();
		return this;
	}

	// -- reading -------------------------------------------------------------

	_read(n: number): void {
		const handle = this._handle;
		if (this.connecting || !handle) {
			this.once("connect", () => this._read(n));
			return;
		}
		if (!handle.reading) {
			handle.reading = true;
			try {
				handle.readStart();
			} catch (e) {
				handle.reading = false;
				this.destroy(errnoException(e, "read"));
			}
		}
	}

	pause(): this {
		if (this.#onread && !this.connecting && this._handle && this._handle.reading) {
			this._handle.reading = false;
			if (!this.destroyed) this._handle.readStop();
		}
		return super.pause();
	}

	resume(): this {
		if (this.#onread && !this.connecting && this._handle && !this._handle.reading) {
			this._read(0);
		}
		return super.resume();
	}

	read(n?: number): unknown {
		if (this.#onread && !this.connecting && this._handle && !this._handle.reading) {
			this._read(0);
		}
		return super.read(n);
	}

	// -- writing -------------------------------------------------------------

	_writeGeneric(writev: boolean, data: unknown, encoding: string, cb: WriteCb): void {
		if (this.connecting) {
			this._pendingData = data;
			this._pendingEncoding = encoding;
			const onClose = () =>
				cb(codedError("ERR_SOCKET_CLOSED_BEFORE_CONNECTION", "Socket closed before the connection was established"));
			this.once("connect", () => {
				this.off("close", onClose);
				this._writeGeneric(writev, data, encoding, cb);
			});
			this.once("close", onClose);
			return;
		}
		this._pendingData = null;
		this._pendingEncoding = "";
		const handle = this._handle;
		if (!handle) {
			cb(socketClosed());
			return;
		}
		this._unrefTimer();
		let bytes = 0;
		const done = (err: Error | null) => {
			if (err) {
				if (this.destroyed) {
					cb(err);
					return;
				}
				cb(errnoException(err, "write"));
				return;
			}
			this._unrefTimer();
			cb(null);
		};
		try {
			if (writev) {
				const entries = data as WritevEntry[];
				const chunks: (Uint8Array | string)[] = [];
				for (const e of entries) {
					chunks.push(toWritable(e.chunk, e.encoding));
					bytes += chunkLength(e.chunk, e.encoding);
				}
				this._bytesDispatched += bytes;
				handle.writev(chunks, done);
			} else {
				const chunk = toWritable(data as Uint8Array | string, encoding);
				bytes = chunkLength(data as Uint8Array | string, encoding);
				this._bytesDispatched += bytes;
				handle.write(chunk, done);
			}
		} catch (e) {
			this.destroy(errnoException(e, "write"));
			cb(e as Error);
		}
	}

	_writev(chunks: WritevEntry[], cb: WriteCb): void {
		this._writeGeneric(true, chunks, "", cb);
	}

	_write(data: Uint8Array | string, encoding: string, cb: WriteCb): void {
		this._writeGeneric(false, data, encoding, cb);
	}

	_final(cb: WriteCb): void {
		if (this.connecting) {
			this.once("connect", () => this._final(cb));
			return;
		}
		const handle = this._handle;
		if (!handle) {
			cb();
			return;
		}
		try {
			handle.shutdown(() => cb());
		} catch (e) {
			const code = (e as SystemError).code;
			if (code === "ENOTCONN") cb();
			else cb(errnoException(e, "shutdown"));
		}
	}

	// -- lifetime ------------------------------------------------------------

	destroySoon(): void {
		if (this.writable) this.end();
		if (this.writableFinished) this.destroy();
		else this.once("finish", this.destroy);
	}

	resetAndDestroy(): this {
		if (this._handle) {
			if (this.connecting) {
				this.once("connect", () => this._reset());
			} else {
				this._reset();
			}
		} else {
			this.destroy(codedError("ERR_SOCKET_CLOSED", "Socket is closed"));
		}
		return this;
	}

	_reset(): this {
		this.resetAndClosing = true;
		return this.destroy();
	}

	_destroy(exception: Error | null, cb: (err: Error | null) => void): void {
		this.connecting = false;
		this.#cancelConnectAttempts();
		if (this.#timer) {
			clearTimer(this.#timer);
			this.#timer = null;
		}
		const handle = this._handle;
		if (handle) {
			const isException = Boolean(exception);
			handle.onread = null;
			this._handle = null;
			this._sockname = null;
			const onClosed = () => this.emit("close", isException);
			if (this.resetAndClosing && typeof handle.reset === "function") {
				this.resetAndClosing = false;
				handle.reset(onClosed);
			} else {
				handle.close(onClosed);
			}
			cb(exception);
		} else {
			cb(exception);
			process.nextTick(() => this.emit("close", Boolean(exception)));
		}
		const server = this._server;
		if (server) {
			server._connections--;
			server._emitCloseIfDrained();
		}
	}

	// -- connecting ----------------------------------------------------------

	connect(...args: unknown[]): this {
		let normalized: NormalizedArgs;
		if (Array.isArray(args[0]) && (args[0] as NormalizedArgs)[kNormalized]) normalized = args[0] as NormalizedArgs;
		else normalized = normalizeArgs(args);
		const options = normalized[0];
		const cb = normalized[1];

		// undo writeAfterFIN of a previous connection
		if (Object.hasOwn(this, "write")) delete (this as Partial<Socket>).write;
		if (this.destroyed) {
			this._handle = null;
			this._peername = null;
			this._sockname = null;
		}

		const path = options.path;
		const pipe = Boolean(path);
		if (!this._handle) {
			this._handle = pipe ? native.createPipe(false) : native.createTCP();
			this.#ownHandle = true;
			this.#initHandle();
		} else if (this.destroyed) {
			this.#initHandle();
		}
		if (cb !== null) this.once("connect", cb);

		this._unrefTimer();
		this.connecting = true;

		if (options.noDelay !== undefined) this.#noDelay = Boolean(options.noDelay);
		if (options.keepAlive) {
			this.#keepAlive = true;
			this.#keepAliveDelay = ~~((options.keepAliveInitialDelay ?? 0) / 1000);
		}
		if (options.blockList) this.blockList = options.blockList;

		if (pipe) {
			validateString(path, "options.path");
			this.#internalConnect(path as string, undefined, 0);
		} else {
			this.#lookupAndConnect(options);
		}
		return this;
	}

	#lookupAndConnect(options: TcpSocketConnectOpts): void {
		const { localAddress, localPort } = options;
		const host = options.host || "localhost";
		let port = options.port;
		if (localAddress && !isIP(localAddress)) throw invalidIPAddress(localAddress);
		if (localPort) validateNumber(localPort, "options.localPort");
		if (typeof port !== "undefined") {
			if (typeof port !== "number" && typeof port !== "string") {
				throw invalidArgType("options.port", "of type number or string", port);
			}
			validatePort(port);
		}
		const portNum = (port as number) | 0;
		port = portNum;

		let autoSelectFamily = options.autoSelectFamily;
		if (autoSelectFamily !== undefined) {
			if (typeof autoSelectFamily !== "boolean")
				throw invalidArgType("options.autoSelectFamily", "of type boolean", autoSelectFamily);
		} else {
			autoSelectFamily = autoSelectFamilyDefault;
		}
		let attemptTimeout = options.autoSelectFamilyAttemptTimeout;
		if (attemptTimeout !== undefined) {
			validateNumber(attemptTimeout, "options.autoSelectFamilyAttemptTimeout", 1);
			if (attemptTimeout < 10) attemptTimeout = 10;
		} else {
			attemptTimeout = autoSelectFamilyAttemptTimeoutDefault;
		}

		const addressType = isIP(host);
		if (addressType) {
			process.nextTick(() => {
				if (this.connecting) this.#internalConnect(host, portNum, addressType, localAddress, localPort);
			});
			return;
		}

		if (options.lookup != null) validateFunction(options.lookup, "options.lookup");
		const family = socketToDnsFamily(options.family);
		const dnsopts: Record<string, unknown> = { family, hints: options.hints || 0 };
		const lookup = (options.lookup || dns.lookup) as LookupFunction;
		this._host = host;

		if (family !== 4 && family !== 6 && !localAddress && autoSelectFamily && this.#ownHandle) {
			dnsopts.all = true;
			lookup(host, dnsopts, (err, addresses) => {
				if (!this.connecting) return;
				if (err) {
					this.emit("lookup", err, undefined, undefined, host);
					process.nextTick(() => this.destroy(err));
					return;
				}
				const list: { address: string; family: number }[] = [];
				if (Array.isArray(addresses)) {
					for (const a of addresses) {
						if (isIP(a.address) && (a.family === 4 || a.family === 6)) list.push(a);
					}
				} else if (typeof addresses === "string") {
					list.push({ address: addresses, family: isIP(addresses) });
				}
				if (list.length === 0) {
					const e = codedError("ENOTFOUND", `getaddrinfo ENOTFOUND ${host}`);
					this.emit("lookup", e, undefined, undefined, host);
					process.nextTick(() => this.destroy(e));
					return;
				}
				// interleave the families, starting with the first returned
				const first = list[0].family;
				const a = list.filter((x) => x.family === first);
				const b = list.filter((x) => x.family !== first);
				const ordered: { address: string; family: number }[] = [];
				for (let i = 0; i < Math.max(a.length, b.length); i++) {
					if (i < a.length) ordered.push(a[i]);
					if (i < b.length) ordered.push(b[i]);
				}
				this.emit("lookup", null, ordered[0].address, ordered[0].family, host);
				if (ordered.length === 1) {
					this._unrefTimer();
					this.#internalConnect(ordered[0].address, portNum, ordered[0].family as 4 | 6, localAddress, localPort);
					return;
				}
				this.autoSelectFamilyAttemptedAddresses = [];
				this.#connectMultiple(ordered, portNum, attemptTimeout as number, 0, []);
			});
			return;
		}

		lookup(host, dnsopts, (err, ip, addrType) => {
			this.emit("lookup", err, ip, addrType, host);
			if (!this.connecting) return;
			if (err) {
				process.nextTick(() => this.destroy(err));
			} else if (typeof ip !== "string" || !isIP(ip)) {
				const e = invalidIPAddress(ip);
				process.nextTick(() => this.destroy(e));
			} else if (addrType !== 4 && addrType !== 6) {
				const e = codedError(
					"ERR_INVALID_ADDRESS_FAMILY",
					`Invalid address family: ${addrType} ${options.host}:${options.port}`,
					RangeError,
				);
				process.nextTick(() => this.destroy(e));
			} else {
				this._unrefTimer();
				this.#internalConnect(ip, portNum, addrType, localAddress, localPort);
			}
		});
	}

	#cancelConnectAttempts(): void {
		const ctx = this.#connectAttempts;
		if (!ctx) return;
		ctx.canceled = true;
		if (ctx.timer) clearTimer(ctx.timer);
		this.#connectAttempts = null;
	}

	#connectMultiple(
		addresses: { address: string; family: number }[],
		port: number,
		attemptTimeout: number,
		index: number,
		errors: Error[],
	): void {
		if (!this.connecting || this.destroyed) return;
		if (index >= addresses.length) {
			this.#connectAttempts = null;
			const agg = new AggregateError(errors, errors.map((e) => e.message).join("; ")) as AggregateError & {
				code?: string;
			};
			agg.code = (errors[0] as SystemError | undefined)?.code;
			this.destroy(errors.length === 1 ? errors[0] : agg);
			return;
		}
		const { address, family } = addresses[index];
		if (index > 0) {
			// a failed or timed out attempt leaves a used handle behind
			const old = this._handle;
			if (old) {
				old.onread = null;
				old.close();
			}
			this._handle = native.createTCP();
			this.#initHandle();
		}
		const ctx: { timer: Timeout | null; canceled: boolean } = { timer: null, canceled: false };
		this.#connectAttempts = ctx;
		this.autoSelectFamilyAttemptedAddresses?.push(`${address}:${port}`);
		this.emit("connectionAttempt", address, port, family);
		const handle = this._handle as StreamHandle;
		const last = index === addresses.length - 1;
		if (!last) {
			ctx.timer = setTimer(() => {
				if (ctx.canceled) return;
				ctx.canceled = true;
				this.emit("connectionAttemptTimeout", address, port, family);
				const e = codedError("ETIMEDOUT", `connect ETIMEDOUT ${address}:${port}`) as SystemError;
				e.syscall = "connect";
				e.address = address;
				e.port = port;
				errors.push(e);
				this.#connectMultiple(addresses, port, attemptTimeout, index + 1, errors);
			}, attemptTimeout);
			ctx.timer.unref();
		}
		const fail = (err: unknown) => {
			if (ctx.canceled) return;
			ctx.canceled = true;
			if (ctx.timer) clearTimer(ctx.timer);
			const ex = exceptionWithHostPort(err, "connect", address, port);
			this.emit("connectionAttemptFailed", address, port, family, ex);
			errors.push(ex);
			this.#connectMultiple(addresses, port, attemptTimeout, index + 1, errors);
		};
		try {
			(handle.connect as NonNullable<StreamHandle["connect"]>)(address, port, (err) => {
				if (ctx.canceled || handle !== this._handle) return;
				if (err) {
					fail(err);
					return;
				}
				ctx.canceled = true;
				if (ctx.timer) clearTimer(ctx.timer);
				this.#connectAttempts = null;
				this.#afterConnect(null, address, port);
			});
		} catch (e) {
			fail(e);
		}
	}

	#internalConnect(
		address: string,
		port: number | undefined,
		addressType: number,
		localAddress?: string,
		localPort?: number,
	): void {
		const handle = this._handle;
		if (!handle) return;
		if (this.blockList && addressType && this.blockList.check(address, `ipv${addressType}`)) {
			this.destroy(codedError("ERR_IP_BLOCKED", `IP(${address}) is blocked by net.BlockList`));
			return;
		}
		if (localAddress || localPort) {
			const bindAddr = localAddress || (addressType === 6 ? "::" : "0.0.0.0");
			try {
				(handle.bind as NonNullable<StreamHandle["bind"]>)(bindAddr, localPort || 0);
			} catch (e) {
				this.destroy(exceptionWithHostPort(e, "bind", bindAddr, localPort));
				return;
			}
		}
		if (typeof handle.connect !== "function") {
			this.destroy(codedError("ERR_INVALID_HANDLE_TYPE", "This handle type cannot be connected", TypeError));
			return;
		}
		try {
			handle.connect(address, addressType ? port : undefined, (err) => {
				if (handle !== this._handle) return;
				this.#afterConnect(err, address, port, localAddress, localPort);
			});
		} catch (e) {
			let details: string | undefined;
			if (localAddress && localPort) details = `${localAddress}:${localPort}`;
			this.destroy(exceptionWithHostPort(e, "connect", address, port, details));
		}
	}

	#afterConnect(
		err: Error | null,
		address: string,
		port: number | undefined,
		localAddress?: string,
		localPort?: number,
	): void {
		if (this.destroyed) return;
		this.connecting = false;
		this._sockname = null;
		if (err) {
			let details: string | undefined;
			if (localAddress && localPort) details = `${localAddress}:${localPort}`;
			this.destroy(exceptionWithHostPort(err, "connect", address, port, details));
			return;
		}
		const handle = this._handle;
		if (handle) {
			if (this.#noDelay !== undefined && typeof handle.setNoDelay === "function") handle.setNoDelay(this.#noDelay);
			if (this.#keepAlive && typeof handle.setKeepAlive === "function") handle.setKeepAlive(true, this.#keepAliveDelay);
		}
		this._unrefTimer();
		this.emit("connect");
		this.emit("ready");
		if (this.readable && !this.isPaused()) this.read(0);
	}

	/** attach a handle connected elsewhere (used by tls and child_process) */
	_attachHandle(handle: StreamHandle): void {
		this._handle = handle;
		this.#initHandle();
	}
}

function createHandleForFd(fd: number, readable: boolean): StreamHandle {
	const type = native.guessHandleType(fd);
	switch (type) {
		case "PIPE":
		case "TCP": {
			const handle = native.createPipe(false);
			(handle.open as NonNullable<StreamHandle["open"]>)(fd);
			return handle;
		}
		case "TTY":
			return native.createTTY(fd, readable);
	}
	throw codedError("ERR_INVALID_FD_TYPE", `Unsupported fd type: ${type}`, TypeError);
}

function socketToDnsFamily(family: unknown): number {
	switch (family) {
		case "IPv4":
			return 4;
		case "IPv6":
			return 6;
	}
	return (family as number) || 0;
}

export { Socket as Stream };

export function connect(...args: unknown[]): Socket {
	const normalized = normalizeArgs(args);
	const options = normalized[0];
	const socket = new Socket(options as SocketConstructorOpts);
	if (options.timeout) socket.setTimeout(options.timeout);
	return socket.connect(normalized);
}

export const createConnection = connect;

// ---------------------------------------------------------------------------
// Server

export interface ServerOpts {
	allowHalfOpen?: boolean;
	pauseOnConnect?: boolean;
	noDelay?: boolean;
	keepAlive?: boolean;
	keepAliveInitialDelay?: number;
	highWaterMark?: number;
	blockList?: BlockList;
	[key: string]: unknown;
}

export interface ListenOptions {
	port?: number | string | null;
	host?: string;
	path?: string;
	backlog?: number;
	exclusive?: boolean;
	readableAll?: boolean;
	writableAll?: boolean;
	ipv6Only?: boolean;
	reusePort?: boolean;
	signal?: AbortSignal;
	fd?: number;
	handle?: unknown;
	_handle?: unknown;
	[key: string]: unknown;
}

function isStreamHandle(v: unknown): v is StreamHandle {
	return (
		v !== null &&
		typeof v === "object" &&
		typeof (v as StreamHandle).listen === "function" &&
		typeof (v as StreamHandle).close === "function" &&
		typeof (v as StreamHandle).readStart === "function"
	);
}

/** Bun.serve/Bun.listen: listen now, throwing errors instead of emitting them */
export const kListenSync: unique symbol = Symbol.for("qbun.net.listenSync");

export class Server extends EventEmitter {
	_connections = 0;
	_handle: StreamHandle | null = null;
	_usingWorkers = false;
	_workers: unknown[] = [];
	_unref = false;
	_listeningId = 1;
	_pipeName: string | undefined = undefined;
	_connectionKey: string | undefined = undefined;
	allowHalfOpen: boolean;
	pauseOnConnect: boolean;
	noDelay: boolean;
	keepAlive: boolean;
	keepAliveInitialDelay: number;
	highWaterMark: number | undefined;
	maxConnections: number | undefined = undefined;
	blockList: BlockList | undefined = undefined;
	#listening = false;

	constructor(options?: ServerOpts | ((socket: Socket) => void), connectionListener?: (socket: Socket) => void) {
		super();
		let opts: ServerOpts;
		if (typeof options === "function") {
			connectionListener = options;
			opts = {};
		} else if (options == null || typeof options === "object") {
			opts = { ...options };
		} else {
			throw invalidArgType("options", "of type object", options);
		}
		if (typeof connectionListener === "function") this.on("connection", connectionListener);
		this.allowHalfOpen = opts.allowHalfOpen || false;
		this.pauseOnConnect = Boolean(opts.pauseOnConnect);
		this.noDelay = Boolean(opts.noDelay);
		this.keepAlive = Boolean(opts.keepAlive);
		this.keepAliveInitialDelay = ~~((opts.keepAliveInitialDelay ?? 0) / 1000);
		if (opts.highWaterMark !== undefined) validateNumber(opts.highWaterMark, "options.highWaterMark", 0);
		this.highWaterMark = opts.highWaterMark;
		if (opts.blockList) {
			if (!BlockList.isBlockList(opts.blockList))
				throw invalidArgType("options.blockList", "an instance of net.BlockList", opts.blockList);
			this.blockList = opts.blockList;
		}
	}

	get listening(): boolean {
		return Boolean(this._handle);
	}

	listen(...args: unknown[]): this {
		this.#listen(args, false);
		return this;
	}

	[kListenSync](...args: unknown[]): this {
		this.#listen(args, true);
		return this;
	}

	#listen(args: unknown[], sync: boolean): void {
		const normalized = normalizeArgs(args);
		let options = normalized[0] as ListenOptions;
		const cb = normalized[1];
		if (this._handle || this.#listening) {
			throw codedError("ERR_SERVER_ALREADY_LISTEN", "Listen method has been called more than once without closing.");
		}
		if (cb !== null) this.once("listening", cb);
		const backlogFromArgs = toNumber(args.length > 1 && args[1]) || toNumber(args.length > 2 && args[2]) || 0;

		// listen(handle) / listen({ handle }) / listen(serverOrSocket): a handle, or an object owning one
		const handle = options._handle || options.handle || options;
		this._listeningId++;
		if (isStreamHandle(handle)) {
			this._handle = handle;
			this.#listening = true;
			this.#setupListenHandle(null, -1, -1, backlogFromArgs, undefined, false, sync);
			return;
		}
		if (
			handle !== options &&
			typeof handle === "object" &&
			handle !== null &&
			"fd" in handle &&
			typeof handle.fd === "number"
		) {
			options = { fd: handle.fd };
		}

		const signal = options.signal;
		if (signal) {
			if (typeof signal !== "object" || !("aborted" in signal))
				throw invalidArgType("options.signal", "an instance of AbortSignal", signal);
			if (signal.aborted) process.nextTick(() => this.close());
			else {
				const onAborted = () => this.close();
				signal.addEventListener("abort", onAborted, { once: true });
				this.once("close", () => signal.removeEventListener("abort", onAborted));
			}
		}

		if (typeof options.fd === "number" && options.fd >= 0) {
			this.#listening = true;
			this.#setupListenHandle(null, null, null, backlogFromArgs, options.fd, false, sync);
			return;
		}

		if (
			args.length === 0 ||
			typeof args[0] === "function" ||
			(options.port === undefined && "port" in options) ||
			options.port === null
		) {
			options.port = 0;
		}

		if (typeof options.port === "number" || typeof options.port === "string") {
			const port = validatePort(options.port, "options.port");
			const backlog = options.backlog || backlogFromArgs;
			const ipv6Only = Boolean(options.ipv6Only);
			this.#listening = true;
			const host = options.host;
			if (host) {
				const type = isIP(host);
				if (type) {
					this.#setupListenHandle(host, port, type, backlog, undefined, ipv6Only, sync);
				} else if (sync) {
					const ip = host === "localhost" ? "127.0.0.1" : null;
					if (!ip) throw codedError("ERR_INVALID_IP_ADDRESS", `Invalid IP address: ${host}`, TypeError);
					this.#setupListenHandle(ip, port, 4, backlog, undefined, ipv6Only, sync);
				} else {
					this.#lookupAndListen(port, host, backlog, ipv6Only);
				}
			} else {
				this.#setupListenHandle(null, port, 4, backlog, undefined, ipv6Only, sync);
			}
			return;
		}

		if (options.path && isPipeName(options.path)) {
			this._pipeName = options.path;
			const backlog = options.backlog || backlogFromArgs;
			this.#listening = true;
			this.#setupListenHandle(options.path, -1, -1, backlog, undefined, false, sync);
			if (this._handle && (options.readableAll || options.writableAll)) {
				let mode = 0o600;
				if (options.readableAll) mode |= 0o044;
				if (options.writableAll) mode |= 0o022;
				try {
					chmodSync(options.path, mode);
				} catch (e) {
					this._handle.close();
					this._handle = null;
					this.#listening = false;
					throw errnoException(e, "uv_pipe_chmod");
				}
			}
			return;
		}

		if (!("port" in options || "path" in options)) {
			throw invalidArgValue("options", JSON.stringify(options), 'must have the property "port" or "path"');
		}
		throw invalidArgValue("options", JSON.stringify(options));
	}

	#lookupAndListen(port: number, address: string, backlog: number, ipv6Only: boolean): void {
		const listeningId = this._listeningId;
		dns.lookup(address, { all: true }, (err: Error | null, addresses: unknown) => {
			if (listeningId !== this._listeningId) return;
			if (err) {
				this.#listening = false;
				this.emit("error", err);
				return;
			}
			const list = addresses as { address: string; family: number }[];
			let chosen = list[0];
			for (const a of list) {
				if (!/^fe[89ab]/i.test(a.address)) {
					chosen = a;
					break;
				}
			}
			this.#setupListenHandle(chosen.address, port, chosen.family || 4, backlog, undefined, ipv6Only, false);
		});
	}

	#setupListenHandle(
		address: string | null,
		port: number | null,
		addressType: number | null,
		backlog: number,
		fd: number | undefined,
		ipv6Only: boolean,
		sync: boolean,
	): void {
		const fail = (err: Error) => {
			this.#listening = false;
			if (sync) throw err;
			process.nextTick(() => {
				this.#listening = false;
				this.emit("error", err);
			});
		};
		if (!this._handle) {
			let handle: StreamHandle;
			try {
				if (typeof fd === "number" && fd >= 0) {
					handle = createHandleForFd(fd, true);
				} else if (port === -1 && addressType === -1) {
					handle = native.createPipe(false);
					try {
						(handle.bind as NonNullable<StreamHandle["bind"]>)(address as string);
					} catch (e) {
						handle.close();
						throw e;
					}
				} else if (!address) {
					// try the unspecified IPv6 address (dual stack), then IPv4
					handle = native.createTCP();
					try {
						(handle.bind as NonNullable<StreamHandle["bind"]>)("::", port as number, ipv6Only);
						address = "::";
						addressType = 6;
					} catch {
						handle.close();
						handle = native.createTCP();
						address = "0.0.0.0";
						addressType = 4;
						try {
							(handle.bind as NonNullable<StreamHandle["bind"]>)(address, port as number);
						} catch (e) {
							handle.close();
							throw e;
						}
					}
				} else {
					handle = native.createTCP();
					try {
						(handle.bind as NonNullable<StreamHandle["bind"]>)(address, port as number, addressType === 6 && ipv6Only);
					} catch (e) {
						handle.close();
						throw e;
					}
				}
			} catch (e) {
				fail(uvExceptionWithHostPort(e, "listen", address, port ?? 0));
				return;
			}
			this._handle = handle;
		}
		const handle = this._handle;
		try {
			(handle.listen as NonNullable<StreamHandle["listen"]>)(backlog || 511, (err, client) =>
				this.#onConnection(err, client),
			);
		} catch (e) {
			handle.close();
			this._handle = null;
			fail(uvExceptionWithHostPort(e, "listen", address, port ?? 0));
			return;
		}
		this._connectionKey = `${addressType}:${address}:${port}`;
		if (this._unref) this.unref();
		process.nextTick(() => {
			if (this._handle) this.emit("listening");
		});
	}

	#onConnection(err: Error | null, clientHandle?: StreamHandle): void {
		if (err || !clientHandle) {
			this.emit("error", errnoException(err, "accept"));
			return;
		}
		if (this.maxConnections != null && this._connections >= this.maxConnections) {
			const data: Record<string, unknown> = {};
			const local = clientHandle.getsockname?.();
			const remote = clientHandle.getpeername?.();
			if (local) {
				data.localAddress = local.address;
				data.localPort = local.port;
				data.localFamily = local.family;
			}
			if (remote) {
				data.remoteAddress = remote.address;
				data.remotePort = remote.port;
				data.remoteFamily = remote.family;
			}
			this.emit("drop", data);
			clientHandle.close();
			return;
		}
		if (this.blockList && typeof clientHandle.getpeername === "function") {
			const remote = clientHandle.getpeername();
			const type = remote ? isIP(remote.address) : 0;
			if (!remote || !type || this.blockList.check(remote.address, `ipv${type}`)) {
				clientHandle.close();
				return;
			}
		}
		const socket = new Socket({
			handle: clientHandle,
			allowHalfOpen: this.allowHalfOpen,
			pauseOnCreate: this.pauseOnConnect,
			readable: true,
			writable: true,
			readableHighWaterMark: this.highWaterMark,
			writableHighWaterMark: this.highWaterMark,
		});
		if (this.noDelay) socket.setNoDelay(true);
		if (this.keepAlive) socket.setKeepAlive(true, this.keepAliveInitialDelay * 1000);
		this._connections++;
		socket.server = this;
		socket._server = this;
		this.emit("connection", socket);
	}

	address(): SockAddr | string | null {
		if (this._pipeName) return this._handle ? this._pipeName : null;
		if (this._handle && typeof this._handle.getsockname === "function") {
			return this._handle.getsockname() ?? {};
		}
		return null;
	}

	getConnections(cb: (err: Error | null, count: number) => void): this {
		process.nextTick(cb, null, this._connections);
		return this;
	}

	close(cb?: (err?: Error) => void): this {
		this.#listening = false;
		this._listeningId++;
		if (typeof cb === "function") {
			if (!this._handle) {
				this.once("close", () => cb(codedError("ERR_SERVER_NOT_RUNNING", "Server is not running.")));
			} else {
				this.once("close", cb);
			}
		}
		if (this._handle) {
			this._handle.close();
			this._handle = null;
		}
		this._emitCloseIfDrained();
		return this;
	}

	_emitCloseIfDrained(): void {
		if (this._handle || this._connections) return;
		process.nextTick(() => this.emit("close"));
	}

	[Symbol.asyncDispose](): Promise<void> {
		if (!this._handle) return Promise.resolve();
		const { promise, resolve, reject } = Promise.withResolvers<void>();
		this.close((err) => (err ? reject(err) : resolve()));
		return promise;
	}

	ref(): this {
		this._unref = false;
		this._handle?.ref();
		return this;
	}

	unref(): this {
		this._unref = true;
		this._handle?.unref();
		return this;
	}

	[captureRejectionSymbol](err: Error, event: string, sock?: Socket): void {
		if (event === "connection" && sock) sock.destroy(err);
		else this.emit("error", err);
	}
}

export function createServer(
	options?: ServerOpts | ((socket: Socket) => void),
	connectionListener?: (socket: Socket) => void,
): Server {
	return new Server(options, connectionListener);
}

// ---------------------------------------------------------------------------
// SocketAddress

type Family = "ipv4" | "ipv6";

export interface SocketAddressInit {
	address?: string;
	family?: string;
	port?: number;
	flowlabel?: number;
}

const kSocketAddress = Symbol("SocketAddress");

export class SocketAddress {
	#address: string;
	#port: number;
	#family: Family;
	#flowlabel: number;
	[kSocketAddress] = true;

	static isSocketAddress(value: unknown): value is SocketAddress {
		return value !== null && typeof value === "object" && (value as Record<symbol, unknown>)[kSocketAddress] === true;
	}

	static parse(input: string): SocketAddress | undefined {
		validateString(input, "input");
		try {
			const URLCtor = (globalThis as unknown as { URL: typeof URL }).URL;
			const url = new URLCtor(`http://${input}`);
			const address = url.hostname;
			const port = Number(url.port) | 0;
			if (address[0] === "[" && address[address.length - 1] === "]") {
				return new SocketAddress({ address: address.slice(1, -1), port, family: "ipv6" });
			}
			return new SocketAddress({ address, port });
		} catch {
			return undefined;
		}
	}

	constructor(options: SocketAddressInit = {}) {
		validateObject(options, "options");
		let family = options.family ?? "ipv4";
		if (typeof family === "string") family = family.toLowerCase();
		if (family !== "ipv4" && family !== "ipv6") throw invalidArgValue("options.family", options.family);
		const address = options.address ?? (family === "ipv4" ? "127.0.0.1" : "::");
		const port = options.port ?? 0;
		const flowlabel = options.flowlabel ?? 0;
		validateString(address, "options.address");
		validatePort(port, "options.port");
		if (typeof flowlabel !== "number" || !Number.isInteger(flowlabel) || flowlabel < 0 || flowlabel > 0xffffffff) {
			throw invalidArgType("options.flowlabel", "of type number", flowlabel);
		}
		let canonical: string;
		if (family === "ipv4") {
			if (!isIPv4(address)) throw codedError("ERR_INVALID_ADDRESS", "Invalid socket address");
			canonical = formatIPv4(parseIPv4(address));
		} else {
			const v = parseIPv6(address);
			if (v === null) throw codedError("ERR_INVALID_ADDRESS", "Invalid socket address");
			canonical = formatIPv6(v);
		}
		this.#address = canonical;
		this.#port = +port | 0;
		this.#family = family;
		this.#flowlabel = flowlabel | 0;
	}

	get address(): string {
		return this.#address;
	}

	get port(): number {
		return this.#port;
	}

	get family(): Family {
		return this.#family;
	}

	get flowlabel(): number {
		return this.#flowlabel;
	}

	toJSON(): { address: string; port: number; family: Family; flowlabel: number } {
		return { address: this.#address, port: this.#port, family: this.#family, flowlabel: this.#flowlabel };
	}
}

// ---------------------------------------------------------------------------
// BlockList

type Rule =
	| { kind: "address"; family: Family; value: bigint; text: string }
	| { kind: "range"; family: Family; start: bigint; end: bigint; text: string }
	| { kind: "subnet"; family: Family; start: bigint; end: bigint; prefix: number; text: string };

const V4_MAPPED_PREFIX = 0xffff00000000n;

function addressValue(address: string, family: Family): bigint {
	if (family === "ipv4") return BigInt(parseIPv4(address));
	const v = parseIPv6(address);
	if (v === null) throw codedError("ERR_INVALID_ADDRESS", "Invalid socket address");
	return v;
}

function toSocketAddress(address: string | SocketAddress, family: string, name: string): SocketAddress {
	if (SocketAddress.isSocketAddress(address)) return address;
	validateString(address, name);
	validateString(family, "family");
	return new SocketAddress({ address, family });
}

function validateInt32(value: unknown, name: string, min: number, max: number): asserts value is number {
	if (typeof value !== "number") throw invalidArgType(name, "of type number", value);
	if (!Number.isInteger(value)) {
		throw codedError(
			"ERR_OUT_OF_RANGE",
			`The value of "${name}" is out of range. It must be an integer. Received ${value}`,
			RangeError,
		);
	}
	if (value < min || value > max) {
		throw codedError(
			"ERR_OUT_OF_RANGE",
			`The value of "${name}" is out of range. It must be >= ${min} && <= ${max}. Received ${value}`,
			RangeError,
		);
	}
}

function parseCIDR(cidr: string): { address: string; prefix: number; family: Family } {
	validateString(cidr, "cidr");
	const slash = cidr.lastIndexOf("/");
	if (slash === -1) throw invalidArgValue("cidr", cidr, 'must contain a prefix length (e.g. "10.0.0.0/8")');
	const address = cidr.slice(0, slash);
	const prefixStr = cidr.slice(slash + 1);
	const prefix = parseInt(prefixStr, 10);
	if (Number.isNaN(prefix) || `${prefix}` !== prefixStr)
		throw invalidArgValue("cidr", cidr, "prefix length must be a valid integer");
	return { address, prefix, family: address.includes(":") ? "ipv6" : "ipv4" };
}

function familyLabel(f: Family): string {
	return f === "ipv4" ? "IPv4" : "IPv6";
}

function subnetBounds(value: bigint, prefix: number, bits: number): [bigint, bigint] {
	const hostBits = BigInt(bits - prefix);
	const mask = ((1n << BigInt(bits)) - 1n) ^ ((1n << hostBits) - 1n);
	const start = value & mask;
	return [start, start | ((1n << hostBits) - 1n)];
}

export class BlockList {
	#rules: Rule[] = [];

	static PRIVATE_RANGES: readonly string[] = Object.freeze([
		"10.0.0.0/8",
		"172.16.0.0/12",
		"192.168.0.0/16",
		"127.0.0.0/8",
		"::1/128",
		"169.254.0.0/16",
		"fe80::/10",
		"fc00::/7",
	]);

	static isBlockList(value: unknown): value is BlockList {
		return value instanceof BlockList;
	}

	addAddress(address: string | SocketAddress, family = "ipv4"): void {
		const sa = toSocketAddress(address, family, "address");
		this.#rules.unshift({
			kind: "address",
			family: sa.family,
			value: addressValue(sa.address, sa.family),
			text: `Address: ${familyLabel(sa.family)} ${sa.address}`,
		});
	}

	addAddresses(addresses: (string | SocketAddress)[], family = "ipv4"): void {
		if (!Array.isArray(addresses)) throw invalidArgType("addresses", "an instance of Array", addresses);
		validateString(family, "family");
		const list = addresses.map((a, i) => toSocketAddress(a, family, `addresses[${i}]`));
		for (const sa of list) this.addAddress(sa);
	}

	addRange(start: string | SocketAddress, end: string | SocketAddress, family = "ipv4"): void {
		const s = toSocketAddress(start, family, "start");
		const e = toSocketAddress(end, family, "end");
		const sv = addressValue(s.address, s.family);
		const ev = addressValue(e.address, e.family);
		if (s.family !== e.family || sv > ev) throw invalidArgValue("start", s.address, "must come before end");
		this.#rules.unshift({
			kind: "range",
			family: s.family,
			start: sv,
			end: ev,
			text: `Range: ${familyLabel(s.family)} ${s.address}-${e.address}`,
		});
	}

	addSubnet(network: string | SocketAddress, prefix: number, family = "ipv4"): void {
		const sa = toSocketAddress(network, family, "network");
		const bits = sa.family === "ipv4" ? 32 : 128;
		validateInt32(prefix, "prefix", 0, bits);
		prefix += 0;
		const [start, end] = subnetBounds(addressValue(sa.address, sa.family), prefix, bits);
		this.#rules.unshift({
			kind: "subnet",
			family: sa.family,
			start,
			end,
			prefix,
			text: `Subnet: ${familyLabel(sa.family)} ${sa.address}/${prefix}`,
		});
	}

	addCIDR(cidr: string): void {
		const { address, prefix, family } = parseCIDR(cidr);
		this.addSubnet(address, prefix, family);
	}

	addCIDRs(cidrs: string[]): void {
		if (!Array.isArray(cidrs)) throw invalidArgType("cidrs", "an instance of Array", cidrs);
		const parsed = cidrs.map((c, i) => {
			validateString(c, `cidrs[${i}]`);
			return parseCIDR(c);
		});
		for (const p of parsed) this.addSubnet(p.address, p.prefix, p.family);
	}

	removeAddress(address: string | SocketAddress, family = "ipv4"): void {
		const sa = toSocketAddress(address, family, "address");
		const v = addressValue(sa.address, sa.family);
		this.#rules = this.#rules.filter((r) => !(r.kind === "address" && r.family === sa.family && r.value === v));
	}

	removeRange(start: string | SocketAddress, end: string | SocketAddress, family = "ipv4"): void {
		const s = toSocketAddress(start, family, "start");
		const e = toSocketAddress(end, family, "end");
		const sv = addressValue(s.address, s.family);
		const ev = addressValue(e.address, e.family);
		this.#rules = this.#rules.filter(
			(r) => !(r.kind === "range" && r.family === s.family && r.start === sv && r.end === ev),
		);
	}

	removeSubnet(network: string | SocketAddress, prefix: number, family = "ipv4"): void {
		const sa = toSocketAddress(network, family, "network");
		const bits = sa.family === "ipv4" ? 32 : 128;
		validateInt32(prefix, "prefix", 0, bits);
		const [start] = subnetBounds(addressValue(sa.address, sa.family), prefix, bits);
		this.#rules = this.#rules.filter(
			(r) => !(r.kind === "subnet" && r.family === sa.family && r.prefix === prefix && r.start === start),
		);
	}

	removeCIDR(cidr: string): void {
		const { address, prefix, family } = parseCIDR(cidr);
		this.removeSubnet(address, prefix, family);
	}

	check(address: string | SocketAddress, family = "ipv4"): boolean {
		let fam: Family;
		let value: bigint;
		if (SocketAddress.isSocketAddress(address)) {
			fam = address.family;
			value = addressValue(address.address, fam);
		} else {
			validateString(address, "address");
			validateString(family, "family");
			fam = family.toLowerCase() === "ipv4" ? "ipv4" : "ipv6";
			if (fam === "ipv4" ? !isIPv4(address) : parseIPv6(address) === null) return false;
			value = addressValue(address, fam);
		}
		// an IPv4 address also matches IPv6 rules through its mapped form,
		// and a v4-mapped IPv6 address matches IPv4 rules
		const mapped = fam === "ipv4" ? V4_MAPPED_PREFIX | value : null;
		const asV4 = fam === "ipv6" && value >> 32n === 0xffffn ? value & 0xffffffffn : null;
		for (const r of this.#rules) {
			let v: bigint | null;
			if (r.family === fam) v = value;
			else if (r.family === "ipv6") v = mapped;
			else v = asV4;
			if (v === null) continue;
			if (r.kind === "address" ? v === r.value : v >= r.start && v <= r.end) return true;
		}
		return false;
	}

	clear(): void {
		this.#rules = [];
	}

	get rules(): string[] {
		return this.#rules.map((r) => r.text);
	}

	get size(): number {
		return this.#rules.length;
	}

	toJSON(): string[] {
		return this.rules;
	}

	fromJSON(data: string | string[]): void {
		let list: unknown = data;
		if (typeof list === "string") list = JSON.parse(list);
		if (!Array.isArray(list) || list.some((n) => typeof n !== "string")) {
			throw invalidArgType("data", "of type string or an instance of Array", data);
		}
		for (const item of list as string[]) {
			let m: RegExpMatchArray | null;
			if (item.includes("IPv4")) {
				if ((m = item.match(/Subnet: IPv4 (\d{1,3}(?:\.\d{1,3}){3})\/(\d{1,2})/)))
					this.addSubnet(m[1], parseInt(m[2], 10));
				else if ((m = item.match(/Address: IPv4 (\d{1,3}(?:\.\d{1,3}){3})/))) this.addAddress(m[1]);
				else if ((m = item.match(/Range: IPv4 (\d{1,3}(?:\.\d{1,3}){3})-(\d{1,3}(?:\.\d{1,3}){3})/)))
					this.addRange(m[1], m[2]);
				continue;
			}
			if (item.includes("IPv6")) {
				if ((m = item.match(/Subnet: IPv6 ([0-9a-fA-F:.]{1,45})\/([0-9]{1,3})/i)))
					this.addSubnet(m[1], parseInt(m[2], 10), "ipv6");
				else if ((m = item.match(/Address: IPv6 ([0-9a-fA-F:.]{1,45})/i))) this.addAddress(m[1], "ipv6");
				else if ((m = item.match(/Range: IPv6 ([0-9a-fA-F:.]{1,45})-([0-9a-fA-F:.]{1,45})/i)))
					this.addRange(m[1], m[2], "ipv6");
			}
		}
	}
}

/** net.Stream is an alias of Socket; _normalizeArgs/_createServerHandle are Node internals some packages use */
export const _normalizeArgs = normalizeArgs;

export default {
	Socket,
	Stream: Socket,
	Server,
	BlockList,
	SocketAddress,
	connect,
	createConnection,
	createServer,
	isIP,
	isIPv4,
	isIPv6,
	getDefaultAutoSelectFamily,
	setDefaultAutoSelectFamily,
	getDefaultAutoSelectFamilyAttemptTimeout,
	setDefaultAutoSelectFamilyAttemptTimeout,
	_normalizeArgs: normalizeArgs,
};
