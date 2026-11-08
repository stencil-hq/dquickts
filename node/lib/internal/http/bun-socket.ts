// Bun.connect / Bun.listen: Bun's handler-based TCP and unix socket API
// over node:net (and node:tls for `tls`).

import * as tls from "node:tls";
import { readFileSync } from "node:fs";
import * as net from "node:net";
import { kListenSync } from "node:net";
import { codedError, invalidArgType, validateObject } from "internal:errors";

type BinaryType = "buffer" | "arraybuffer" | "uint8array";

export interface SocketHandler {
	open?(socket: BunSocket): unknown;
	close?(socket: BunSocket, error?: Error): unknown;
	error?(socket: BunSocket, error: Error): unknown;
	data?(socket: BunSocket, data: Buffer | ArrayBuffer | Uint8Array): unknown;
	drain?(socket: BunSocket): unknown;
	handshake?(socket: BunSocket, success: boolean, authorizationError: Error | null): unknown;
	end?(socket: BunSocket): unknown;
	connectError?(socket: BunSocket, error: Error): unknown;
	timeout?(socket: BunSocket): unknown;
	binaryType?: BinaryType;
}

export interface SocketOptions {
	socket: SocketHandler;
	data?: unknown;
	allowHalfOpen?: boolean;
	hostname?: string;
	port?: number;
	unix?: string;
	fd?: number;
	tls?: boolean | Record<string, unknown>;
	exclusive?: boolean;
	reusePort?: boolean;
	ipv6Only?: boolean;
}

type TlsSocketLike = net.Socket & {
	authorized?: boolean;
	authorizationError?: Error | string | null;
	alpnProtocol?: string | false | null;
	encrypted?: boolean;
	[key: string]: unknown;
};

/** read a Bun TLS option value: string, Buffer, TypedArray, BunFile, or an array of them */
function tlsValue(v: unknown): unknown {
	if (Array.isArray(v)) return v.map(tlsValue);
	if (v && typeof v === "object" && !ArrayBuffer.isView(v) && !(v instanceof ArrayBuffer)) {
		const name = (v as { name?: unknown }).name;
		if (typeof name === "string") {
			return readFileSync(name);
		}
	}
	return v;
}

/** Bun's `tls` options as node:tls options */
export function bunTlsOptions(tls: unknown): Record<string, unknown> {
	if (tls === true || tls === undefined || tls === null) return {};
	validateObject(tls, "tls");
	const out: Record<string, unknown> = {};
	const src = tls as Record<string, unknown>;
	for (const k in src) {
		const v = src[k];
		if (v === undefined) continue;
		switch (k) {
			case "serverName":
				out.servername = v;
				break;
			case "key":
			case "cert":
			case "ca":
			case "dhParamsFile":
				out[k === "dhParamsFile" ? "dhparam" : k] = tlsValue(v);
				break;
			case "lowMemoryMode":
				break;
			default:
				out[k] = v;
		}
	}
	return out;
}

function toError(e: unknown): Error {
	return e instanceof Error ? e : new Error(String(e));
}

function readyStateOf(s: net.Socket, detached: boolean): -2 | -1 | 0 | 1 | 2 {
	if (detached) return -2;
	if (s.destroyed) return -1;
	if (s.connecting) return 0;
	if (s.writableEnded) return 2;
	return 1;
}

export class BunSocket {
	data: unknown;
	readonly listener: BunListener | undefined;
	#sock: TlsSocketLike;
	#handlers: SocketHandler;
	#detached = false;
	#opened = false;

	constructor(
		sock: net.Socket,
		handlers: SocketHandler,
		data: unknown,
		listener: BunListener | undefined,
		allowHalfOpen: boolean,
	) {
		this.#sock = sock as TlsSocketLike;
		this.#handlers = handlers;
		this.data = data;
		this.listener = listener;
		sock.on("data", (chunk: Buffer) => {
			const h = this.#handlers;
			if (!h.data) return;
			let value: Buffer | ArrayBuffer | Uint8Array = chunk;
			if (h.binaryType === "arraybuffer")
				value = chunk.buffer.slice(chunk.byteOffset, chunk.byteOffset + chunk.byteLength) as ArrayBuffer;
			else if (h.binaryType === "uint8array") value = new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
			this._call("data", value);
		});
		sock.on("drain", () => this._call("drain"));
		sock.on("end", () => {
			this._call("end");
			if (!allowHalfOpen && !sock.writableEnded) sock.end();
		});
		sock.on("timeout", () => this._call("timeout"));
		sock.on("error", (err: Error) => {
			if (!this.#opened) return;
			this._call("error", err);
		});
		sock.on("close", (hadError: boolean) => {
			if (!this.#opened) return;
			const err = hadError ? ((sock as unknown as { errored?: Error | null }).errored ?? undefined) : undefined;
			this._call("close", err);
		});
	}

	/** mark open and run the open (and handshake) handlers */
	_open(): void {
		this.#opened = true;
		const s = this.#sock;
		if (s.encrypted && this.#handlers.handshake) {
			const authError = s.authorizationError;
			this._call(
				"handshake",
				Boolean(s.authorized),
				authError ? (authError instanceof Error ? authError : new Error(String(authError))) : null,
			);
		}
		this._call("open");
	}

	_call(name: keyof SocketHandler, ...args: unknown[]): void {
		const fn = this.#handlers[name];
		if (typeof fn !== "function") return;
		try {
			const r = (fn as (...a: unknown[]) => unknown).call(this.#handlers, this, ...args);
			if (r instanceof Promise) r.catch((e: unknown) => this.#handlerError(name, toError(e)));
		} catch (e) {
			this.#handlerError(name, toError(e));
		}
	}

	#handlerError(name: keyof SocketHandler, err: Error): void {
		if (name !== "error" && typeof this.#handlers.error === "function") {
			this._call("error", err);
			return;
		}
		process.nextTick(() => {
			throw err;
		});
	}

	get _socket(): net.Socket {
		return this.#sock;
	}

	write(data: string | ArrayBuffer | ArrayBufferView, byteOffset?: number, byteLength?: number): number {
		const s = this.#sock;
		if (s.destroyed || s.writableEnded) return -1;
		let chunk: string | Buffer;
		if (typeof data === "string") {
			chunk =
				byteOffset !== undefined || byteLength !== undefined
					? Buffer.from(data, "utf8").subarray(
							byteOffset ?? 0,
							byteLength !== undefined ? (byteOffset ?? 0) + byteLength : undefined,
						)
					: data;
		} else if (data instanceof ArrayBuffer) {
			const start = byteOffset ?? 0;
			chunk = Buffer.from(data, start, byteLength ?? data.byteLength - start);
		} else if (ArrayBuffer.isView(data)) {
			const start = byteOffset ?? 0;
			chunk = Buffer.from(data.buffer, data.byteOffset + start, byteLength ?? data.byteLength - start);
		} else {
			throw invalidArgType("data", "of type string or an instance of ArrayBuffer or ArrayBufferView", data);
		}
		const len = typeof chunk === "string" ? Buffer.byteLength(chunk) : chunk.length;
		s.write(chunk);
		return len;
	}

	end(data?: string | ArrayBuffer | ArrayBufferView, byteOffset?: number, byteLength?: number): number {
		let n = 0;
		if (data !== undefined) n = this.write(data, byteOffset, byteLength);
		if (!this.#sock.writableEnded) this.#sock.end();
		return n;
	}

	shutdown(_halfClose?: boolean): void {
		if (!this.#sock.writableEnded) this.#sock.end();
	}

	terminate(): void {
		this.#sock.destroy();
	}

	close(): void {
		this.end();
	}

	flush(): void {}

	ref(): void {
		this.#sock.ref();
	}

	unref(): void {
		this.#sock.unref();
	}

	timeout(seconds: number): void {
		this.#sock.setTimeout(Math.max(0, seconds) * 1000);
	}

	setNoDelay(noDelay = true): boolean {
		this.#sock.setNoDelay(noDelay);
		return true;
	}

	setKeepAlive(enable = false, initialDelay = 0): boolean {
		this.#sock.setKeepAlive(enable, initialDelay);
		return true;
	}

	reload(options: { socket: SocketHandler }): void {
		if (!options || typeof options.socket !== "object")
			throw invalidArgType("options.socket", "of type object", options?.socket);
		this.#handlers = options.socket;
	}

	pause(): void {
		this.#sock.pause();
	}

	resume(): void {
		this.#sock.resume();
	}

	get readyState(): -2 | -1 | 0 | 1 | 2 {
		return readyStateOf(this.#sock, this.#detached);
	}

	get remoteAddress(): string {
		return this.#sock.remoteAddress ?? "";
	}

	get remotePort(): number {
		return this.#sock.remotePort ?? 0;
	}

	get remoteFamily(): "IPv4" | "IPv6" {
		return (this.#sock.remoteFamily ?? "IPv4") as "IPv4" | "IPv6";
	}

	get localAddress(): string {
		return this.#sock.localAddress ?? "";
	}

	get localPort(): number {
		return this.#sock.localPort ?? 0;
	}

	get localFamily(): "IPv4" | "IPv6" {
		return (this.#sock.localFamily ?? "IPv4") as "IPv4" | "IPv6";
	}

	get bytesWritten(): number {
		return this.#sock.bytesWritten ?? 0;
	}

	get authorized(): boolean {
		return Boolean(this.#sock.authorized);
	}

	get alpnProtocol(): string | false | null {
		return this.#sock.alpnProtocol ?? null;
	}

	#tlsCall<T>(name: string, ...args: unknown[]): T | undefined {
		const fn = this.#sock[name];
		return typeof fn === "function" ? (fn as (...a: unknown[]) => T).apply(this.#sock, args) : undefined;
	}

	getAuthorizationError(): Error | null {
		const e = this.#sock.authorizationError;
		if (!e) return null;
		return e instanceof Error ? e : new Error(String(e));
	}

	getPeerCertificate(detailed?: boolean): unknown {
		return this.#tlsCall("getPeerCertificate", detailed) ?? null;
	}

	getCertificate(): unknown {
		return this.#tlsCall("getCertificate") ?? null;
	}

	getX509Certificate(): unknown {
		return this.#tlsCall("getX509Certificate");
	}

	getPeerX509Certificate(): unknown {
		return this.#tlsCall("getPeerX509Certificate");
	}

	getCipher(): unknown {
		return this.#tlsCall("getCipher");
	}

	getEphemeralKeyInfo(): unknown {
		return this.#tlsCall("getEphemeralKeyInfo") ?? null;
	}

	getSharedSigalgs(): string[] {
		return this.#tlsCall<string[]>("getSharedSigalgs") ?? [];
	}

	getTLSFinishedMessage(): Buffer | undefined {
		return this.#tlsCall("getFinished");
	}

	getTLSPeerFinishedMessage(): Buffer | undefined {
		return this.#tlsCall("getPeerFinished");
	}

	getTLSTicket(): Buffer | undefined {
		return this.#tlsCall("getTLSTicket");
	}

	getTLSVersion(): string {
		return this.#tlsCall<string>("getProtocol") ?? "";
	}

	getSession(): unknown {
		return this.#tlsCall("getSession");
	}

	setSession(session: unknown): void {
		this.#tlsCall("setSession", session);
	}

	isSessionReused(): boolean {
		return Boolean(this.#tlsCall("isSessionReused"));
	}

	exportKeyingMaterial(length: number, label: string, context?: unknown): unknown {
		return this.#tlsCall("exportKeyingMaterial", length, label, context);
	}

	setMaxSendFragment(size: number): boolean {
		return Boolean(this.#tlsCall("setMaxSendFragment", size));
	}

	setVerifyMode(requestCert: boolean, rejectUnauthorized: boolean): void {
		this.#tlsCall("setVerifyMode", requestCert, rejectUnauthorized);
	}

	disableRenegotiation(): void {
		this.#tlsCall("disableRenegotiation");
	}

	renegotiate(): void {
		this.#tlsCall("renegotiate", {}, () => {});
	}

	getServername(): string {
		return String(this.#sock.servername ?? "");
	}

	setServername(name: string): void {
		this.#tlsCall("setServername", name);
	}

	/** start TLS on this connection: [raw, tls] sockets */
	upgradeTLS(options: {
		data?: unknown;
		tls: boolean | Record<string, unknown>;
		socket: SocketHandler;
	}): [BunSocket, BunSocket] {
		validateObject(options, "options");
		const raw = this.#sock;
		this.#detached = true;
		const secure = tls.connect({ ...bunTlsOptions(options.tls), socket: raw });
		const wrapped = new BunSocket(secure, options.socket, options.data, this.listener, false);
		secure.once("secureConnect", () => wrapped._open());
		secure.once("error", (err: Error) => wrapped._call("connectError", err));
		return [this, wrapped];
	}

	[Symbol.dispose](): void {
		this.end();
	}
}

// ---------------------------------------------------------------------------
// Bun.connect

function validateHandlers(options: SocketOptions): SocketHandler {
	validateObject(options, "options");
	const handlers = options.socket;
	if (!handlers || typeof handlers !== "object")
		throw codedError("ERR_INVALID_ARG_TYPE", 'Expected "socket" to be an object', TypeError);
	return handlers;
}

export function connect(options: SocketOptions): Promise<BunSocket> {
	const handlers = validateHandlers(options);
	const { promise, resolve, reject } = Promise.withResolvers<BunSocket>();
	// a failed connect is reported to the handlers; the rejection counts as handled
	promise.catch(() => {});
	let sock: net.Socket;
	const allowHalfOpen = Boolean(options.allowHalfOpen);
	try {
		if (options.tls) {
			const tlsOpts = bunTlsOptions(options.tls);
			if (options.unix !== undefined) tlsOpts.path = options.unix;
			else {
				tlsOpts.host = options.hostname ?? "localhost";
				tlsOpts.port = options.port;
				if (tlsOpts.servername === undefined && options.hostname && net.isIP(options.hostname) === 0)
					tlsOpts.servername = options.hostname;
			}
			sock = tls.connect(tlsOpts);
		} else if (options.unix !== undefined) {
			sock = net.createConnection({ path: options.unix, allowHalfOpen: true });
		} else if (options.fd !== undefined) {
			sock = new net.Socket({ fd: options.fd, readable: true, writable: true, allowHalfOpen: true });
		} else {
			if (options.port === undefined)
				throw codedError("ERR_INVALID_ARG_TYPE", 'Expected "port" to be a number between 0 and 65535', TypeError);
			sock = net.createConnection({ host: options.hostname ?? "localhost", port: options.port, allowHalfOpen: true });
		}
	} catch (e) {
		reject(e);
		return promise;
	}
	const wrapper = new BunSocket(sock, handlers, options.data, undefined, allowHalfOpen);
	let settled = false;
	const onOpen = () => {
		if (settled) return;
		settled = true;
		sock.removeListener("error", onConnectError);
		wrapper._open();
		resolve(wrapper);
	};
	const onConnectError = (err: Error) => {
		if (settled) return;
		settled = true;
		if (typeof handlers.connectError === "function") wrapper._call("connectError", err);
		else if (typeof handlers.error === "function") wrapper._call("error", err);
		reject(err);
	};
	if (options.tls) sock.once("secureConnect", onOpen);
	else if (options.fd !== undefined) process.nextTick(onOpen);
	else sock.once("connect", onOpen);
	sock.once("error", onConnectError);
	return promise;
}

// ---------------------------------------------------------------------------
// Bun.listen

export class BunListener {
	data: unknown;
	#server: net.Server;
	#handlers: SocketHandler;
	#sockets = new Set<BunSocket>();
	#unix: string | undefined;
	#hostname: string | undefined;

	constructor(options: SocketOptions) {
		const handlers = validateHandlers(options);
		this.#handlers = handlers;
		this.data = options.data;
		const allowHalfOpen = Boolean(options.allowHalfOpen);
		const onSocket = (sock: net.Socket) => {
			const wrapper = new BunSocket(sock, this.#handlers, this.data, this, allowHalfOpen);
			this.#sockets.add(wrapper);
			sock.once("close", () => this.#sockets.delete(wrapper));
			wrapper._open();
		};
		if (options.tls) {
			this.#server = new tls.Server({ ...bunTlsOptions(options.tls), allowHalfOpen: true }, onSocket);
		} else {
			this.#server = net.createServer({ allowHalfOpen: true }, onSocket);
		}
		this.#server.on("error", (err: Error) => {
			if (typeof this.#handlers.error === "function")
				(this.#handlers.error as (s: unknown, e: Error) => unknown).call(this.#handlers, this, err);
		});
		const listen = (this.#server as unknown as Record<symbol, (opts: Record<string, unknown>) => void>)[kListenSync];
		try {
			if (options.unix !== undefined) {
				this.#unix = options.unix;
				listen.call(this.#server, { path: options.unix });
			} else {
				if (options.port === undefined)
					throw codedError("ERR_INVALID_ARG_TYPE", 'Expected "port" to be a number between 0 and 65535', TypeError);
				this.#hostname = options.hostname ?? "0.0.0.0";
				listen.call(this.#server, {
					port: options.port,
					host: this.#hostname,
					exclusive: options.exclusive,
					ipv6Only: options.ipv6Only,
				});
			}
		} catch (e) {
			const err = e as Error & { code?: string };
			if (err.code === "EADDRINUSE") {
				const ex = new Error(`Failed to listen at ${this.#hostname ?? this.#unix}`) as Error & {
					code?: string;
					syscall?: string;
					errno?: number;
				};
				ex.code = "EADDRINUSE";
				ex.syscall = "listen";
				ex.errno = (err as Error & { errno?: number }).errno;
				throw ex;
			}
			throw err;
		}
	}

	get port(): number {
		const addr = this.#server.address();
		return addr && typeof addr === "object" ? addr.port : 0;
	}

	get hostname(): string {
		return this.#hostname ?? "";
	}

	get unix(): string {
		return this.#unix ?? "";
	}

	stop(closeActiveConnections?: boolean): void {
		this.#server.close();
		if (closeActiveConnections) for (const s of this.#sockets) s.terminate();
	}

	ref(): void {
		this.#server.ref();
	}

	unref(): void {
		this.#server.unref();
	}

	reload(options: { socket: SocketHandler }): void {
		if (!options || typeof options.socket !== "object")
			throw invalidArgType("options.socket", "of type object", options?.socket);
		this.#handlers = options.socket;
		for (const s of this.#sockets) s.reload(options);
	}

	[Symbol.dispose](): void {
		this.stop(true);
	}
}

export function listen(options: SocketOptions): BunListener {
	return new BunListener(options);
}
