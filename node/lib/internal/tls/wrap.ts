// node:tls sockets and servers: TLSSocket (a net.Socket whose handle is a TLS
// engine over a transport), Server and connect().
// Adapted from Node.js lib/internal/tls/wrap.js (MIT).
//
// The handle of a TLSSocket is a TLSWrap: it implements the qbun:net
// StreamHandle interface that net.Socket drives, and moves the bytes
// between the TLS engine of qbun:tls and the transport:
//   - tls.connect() without `socket`: a qbun:net TCP (or pipe) handle,
//     connected by net.Socket.prototype.connect through TLSWrap.connect()
//   - any other socket or duplex stream (tls.connect({ socket }), new
//     TLSSocket(socket), server connections): its public stream API.

import { createHash, X509Certificate } from "node:crypto";
import * as netBinding from "qbun:net";
import * as net from "node:net";
import { captureRejectionSymbol } from "node:events";
import {
	codedError,
	invalidArgType,
	invalidArgValue,
	notSupported,
	validateFunction,
	validateNumber,
	validateObject,
	validateString,
} from "internal:errors";
import { isIP } from "internal:http/ip";
import {
	CLIENT_RENEG_LIMIT,
	CLIENT_RENEG_WINDOW,
	SecureContext,
	checkServerIdentity as defaultCheckServerIdentity,
	connResetException,
	convertALPNProtocols,
	createSecureContext,
	defaults,
	native,
	nativeContextOf,
	translatePeerCertificate,
	validateOpenSSLEngineOptions,
	type CipherNameAndProtocol,
	type EphemeralKeyInfo,
	type NativeConnection,
	type PeerCertificate,
	type SecureContextOptions,
} from "internal:tls/common";

type Callback = (...args: unknown[]) => void;
type WriteCb = (err: Error | null) => void;

/** the stream API used on a transport socket (net.Socket or any Duplex) */
interface DuplexLike {
	readonly destroyed: boolean;
	readonly writableEnded: boolean;
	readonly closed?: boolean;
	readonly allowHalfOpen?: boolean;
	readonly connecting?: boolean;
	readonly readableLength?: number;
	_handle?: net.StreamHandle | null;
	_host?: string | null;
	remoteAddress?: string;
	remoteFamily?: string;
	remotePort?: number;
	localAddress?: string;
	localFamily?: string;
	localPort?: number;
	writableLength?: number;
	on(event: string, listener: Callback): unknown;
	once(event: string, listener: Callback): unknown;
	off(event: string, listener: Callback): unknown;
	write(chunk: Uint8Array, cb?: (err?: Error | null) => void): boolean;
	end(cb?: () => void): unknown;
	destroy(err?: Error): unknown;
	pause(): unknown;
	resume(): unknown;
	ref?(): unknown;
	unref?(): unknown;
	setNoDelay?(on?: boolean): unknown;
	setKeepAlive?(on?: boolean, delay?: number): unknown;
}

export interface TLSSocketOptions extends SecureContextOptions {
	isServer?: boolean;
	server?: Server;
	secureContext?: SecureContext;
	credentials?: SecureContext;
	requestCert?: boolean;
	rejectUnauthorized?: boolean;
	ALPNProtocols?: string[] | Uint8Array | Buffer;
	ALPNCallback?: (arg: { servername: string | false; protocols: string[] }) => string | undefined;
	SNICallback?: (servername: string, cb: (err: Error | null, ctx?: SecureContext) => void) => void;
	session?: Buffer;
	requestOCSP?: boolean;
	enableTrace?: boolean;
	pskCallback?: Callback;
	pskIdentityHint?: string;
	handshakeTimeout?: number;
	allowHalfOpen?: boolean;
	pauseOnConnect?: boolean;
	highWaterMark?: number;
	onread?: unknown;
	signal?: AbortSignal;
	noDelay?: boolean;
	keepAlive?: boolean;
	keepAliveInitialDelay?: number;
	pipe?: boolean;
}

export interface ConnectionOptions extends TLSSocketOptions {
	host?: string;
	port?: number;
	path?: string;
	socket?: DuplexLike;
	servername?: string;
	checkServerIdentity?: (hostname: string, cert: PeerCertificate) => Error | undefined;
	minDHSize?: number;
	timeout?: number;
	lookup?: unknown;
	localAddress?: string;
	family?: number;
}

// ---------------------------------------------------------------------------
// transports

interface SockAddr {
	address: string;
	family: "IPv4" | "IPv6";
	port: number;
}

interface TransportEvents {
	data(chunk: Buffer): void;
	end(): void;
	error(err: Error): void;
	close(): void;
}

interface Transport {
	readonly fd: number;
	readonly writeQueueSize: number;
	/** start delivering the incoming data */
	start(ev: TransportEvents): void;
	pause(): void;
	resume(): void;
	write(data: Buffer, cb: WriteCb): void;
	end(cb: () => void): void;
	destroy(cb: () => void): void;
	ref(): void;
	unref(): void;
	hasRef(): boolean;
	getsockname(): SockAddr | null;
	getpeername(): SockAddr | null;
	setNoDelay(on: boolean): void;
	setKeepAlive(on: boolean, delaySecs: number): void;
	setBlocking(blocking: boolean): void;
	connect?(host: string, port: number | undefined, cb: WriteCb): void;
	bind?(host: string, port?: number, ipv6Only?: boolean): void;
}

const noop = (): void => {};

interface NetNative {
	createTCP(): net.StreamHandle;
	createPipe(ipc?: boolean): net.StreamHandle;
}

// qbun:net exports these factories (node/net.c)
const netNative = netBinding as unknown as NetNative;

function socketClosed(): Error {
	return codedError("ERR_SOCKET_CLOSED", "Socket is closed");
}

/** a qbun:net stream handle */
class HandleTransport implements Transport {
	readonly #h: net.StreamHandle;
	#ev: TransportEvents | null = null;
	#reading = false;
	#closed = false;

	constructor(handle: net.StreamHandle) {
		this.#h = handle;
	}

	get fd(): number {
		return this.#h.fd;
	}

	get writeQueueSize(): number {
		return this.#closed ? 0 : this.#h.writeQueueSize;
	}

	start(ev: TransportEvents): void {
		this.#ev = ev;
		this.#h.onread = (chunk, err) => {
			if (err) ev.error(err);
			else if (chunk === null) ev.end();
			else ev.data(chunk);
		};
		this.resume();
	}

	pause(): void {
		if (!this.#reading || this.#closed) return;
		this.#reading = false;
		this.#h.readStop();
	}

	resume(): void {
		if (this.#reading || this.#closed || !this.#ev) return;
		this.#reading = true;
		try {
			this.#h.readStart();
		} catch (err) {
			this.#reading = false;
			this.#ev.error(err as Error);
		}
	}

	write(data: Buffer, cb: WriteCb): void {
		if (this.#closed) {
			process.nextTick(cb, socketClosed());
			return;
		}
		try {
			this.#h.write(data, cb);
		} catch (err) {
			process.nextTick(cb, err as Error);
		}
	}

	end(cb: () => void): void {
		if (this.#closed) {
			process.nextTick(cb);
			return;
		}
		try {
			this.#h.shutdown(() => cb());
		} catch {
			process.nextTick(cb);
		}
	}

	destroy(cb: () => void): void {
		if (this.#closed) {
			process.nextTick(cb);
			return;
		}
		this.#closed = true;
		this.#h.onread = null;
		this.#h.close(cb);
	}

	ref(): void {
		if (!this.#closed) this.#h.ref();
	}

	unref(): void {
		if (!this.#closed) this.#h.unref();
	}

	hasRef(): boolean {
		return !this.#closed && this.#h.hasRef();
	}

	getsockname(): SockAddr | null {
		return this.#closed ? null : (this.#h.getsockname?.() ?? null);
	}

	getpeername(): SockAddr | null {
		return this.#closed ? null : (this.#h.getpeername?.() ?? null);
	}

	setNoDelay(on: boolean): void {
		if (!this.#closed) this.#h.setNoDelay?.(on);
	}

	setKeepAlive(on: boolean, delaySecs: number): void {
		if (!this.#closed) this.#h.setKeepAlive?.(on, delaySecs);
	}

	setBlocking(blocking: boolean): void {
		if (!this.#closed) this.#h.setBlocking?.(blocking);
	}

	connect(host: string, port: number | undefined, cb: WriteCb): void {
		const connect = this.#h.connect;
		if (typeof connect !== "function")
			throw codedError("ERR_INVALID_HANDLE_TYPE", "This handle type cannot be connected", TypeError);
		connect.call(this.#h, host, port, cb);
	}

	bind(host: string, port?: number, ipv6Only?: boolean): void {
		const bind = this.#h.bind;
		if (typeof bind !== "function")
			throw codedError("ERR_INVALID_HANDLE_TYPE", "This handle type cannot be bound", TypeError);
		bind.call(this.#h, host, port, ipv6Only);
	}
}

/** a socket or duplex stream, through its public API */
class StreamTransport implements Transport {
	readonly stream: DuplexLike;
	#ev: TransportEvents | null = null;
	#closed = false;
	readonly #onData = (chunk: unknown): void => {
		const buf = Buffer.isBuffer(chunk)
			? chunk
			: typeof chunk === "string"
				? Buffer.from(chunk, "latin1")
				: Buffer.from(chunk as Uint8Array);
		this.#ev?.data(buf);
	};
	readonly #onEnd = (): void => this.#ev?.end();
	readonly #onError = (err: unknown): void => this.#ev?.error(err as Error);
	readonly #onClose = (): void => this.#ev?.close();

	constructor(stream: DuplexLike) {
		this.stream = stream;
	}

	get fd(): number {
		return this.stream._handle?.fd ?? -1;
	}

	get writeQueueSize(): number {
		return this.stream.writableLength ?? 0;
	}

	start(ev: TransportEvents): void {
		this.#ev = ev;
		const s = this.stream;
		s.on("data", this.#onData);
		s.on("end", this.#onEnd);
		s.on("error", this.#onError);
		s.on("close", this.#onClose);
		s.resume();
	}

	#detach(): void {
		const s = this.stream;
		s.off("data", this.#onData);
		s.off("end", this.#onEnd);
		s.off("error", this.#onError);
		s.off("close", this.#onClose);
		this.#ev = null;
	}

	pause(): void {
		if (!this.#closed) this.stream.pause();
	}

	resume(): void {
		if (!this.#closed) this.stream.resume();
	}

	write(data: Buffer, cb: WriteCb): void {
		const s = this.stream;
		if (this.#closed || s.destroyed || s.writableEnded) {
			process.nextTick(cb, socketClosed());
			return;
		}
		s.write(data, (err) => cb(err ?? null));
	}

	end(cb: () => void): void {
		const s = this.stream;
		if (this.#closed || s.destroyed || s.writableEnded) {
			process.nextTick(cb);
			return;
		}
		s.end(() => cb());
	}

	destroy(cb: () => void): void {
		if (this.#closed) {
			process.nextTick(cb);
			return;
		}
		this.#closed = true;
		this.#detach();
		const s = this.stream;
		let called = false;
		const done = (): void => {
			if (called) return;
			called = true;
			cb();
		};
		if (s.closed === true) {
			process.nextTick(done);
			return;
		}
		s.once("close", done);
		if (!s.destroyed) s.destroy();
		// streams that never emit 'close' (emitClose: false)
		if (s.closed === true) process.nextTick(done);
	}

	ref(): void {
		this.stream.ref?.();
	}

	unref(): void {
		this.stream.unref?.();
	}

	hasRef(): boolean {
		const h = this.stream._handle;
		return h ? h.hasRef() : !this.#closed;
	}

	getsockname(): SockAddr | null {
		const s = this.stream;
		const h = s._handle;
		if (h && typeof h.getsockname === "function") return h.getsockname();
		if (s.localAddress === undefined) return null;
		return {
			address: s.localAddress,
			family: (s.localFamily as SockAddr["family"]) ?? (isIP(s.localAddress) === 6 ? "IPv6" : "IPv4"),
			port: s.localPort ?? 0,
		};
	}

	getpeername(): SockAddr | null {
		const s = this.stream;
		const h = s._handle;
		if (h && typeof h.getpeername === "function") return h.getpeername();
		if (s.remoteAddress === undefined) return null;
		return {
			address: s.remoteAddress,
			family: (s.remoteFamily as SockAddr["family"]) ?? (isIP(s.remoteAddress) === 6 ? "IPv6" : "IPv4"),
			port: s.remotePort ?? 0,
		};
	}

	setNoDelay(on: boolean): void {
		this.stream.setNoDelay?.(on);
	}

	setKeepAlive(on: boolean, delaySecs: number): void {
		this.stream.setKeepAlive?.(on, delaySecs * 1000);
	}

	setBlocking(blocking: boolean): void {
		this.stream._handle?.setBlocking?.(blocking);
	}
}

// ---------------------------------------------------------------------------
// TLSWrap: the StreamHandle of a TLSSocket

const kOnHandshakeDone = Symbol("onHandshakeDone");
const kOnError = Symbol("onError");
const kOnCertCb = Symbol("onCertCb");
const kOnNewSession = Symbol("onNewSession");
const kOnEarlyEnd = Symbol("onEarlyEnd");
const kOnTransportError = Symbol("onTransportError");
const kOnTransportClose = Symbol("onTransportClose");

interface PendingWrite {
	data: Buffer;
	cb: WriteCb;
}

class TLSWrap implements net.StreamHandle {
	onread: ((chunk: Buffer | null, err?: Error) => void) | null = null;
	reading = false;
	owner: TLSSocket | null = null;
	readonly conn: NativeConnection;
	transport: Transport | null;
	readonly #fromHandle: boolean;
	#userReading = false;
	#secure = false;
	#closed = false;
	#started = false;
	#queue: PendingWrite[] = [];
	#queuedBytes = 0;
	#shutdownCb: WriteCb | null = null;
	#held: Buffer[] = [];
	#eofPending = false;
	#eofDelivered = false;
	#certCbDispatched = false;

	constructor(conn: NativeConnection, transport: Transport, fromHandle: boolean) {
		this.conn = conn;
		this.transport = transport;
		this.#fromHandle = fromHandle;
	}

	get fd(): number {
		return this.transport?.fd ?? -1;
	}

	get writeQueueSize(): number {
		return this.#queuedBytes + (this.transport?.writeQueueSize ?? 0);
	}

	get closed(): boolean {
		return this.#closed;
	}

	/** start reading the transport (once it is connected) */
	attach(): void {
		if (this.#started || this.#closed || !this.transport) return;
		this.#started = true;
		this.transport.start({
			data: (chunk) => this.#onData(chunk),
			end: () => this.#onTransportEnd(),
			error: (err) => {
				if (this.#closed) return;
				if (this.#fromHandle) this.onread?.(null, err);
				else this.owner?.[kOnTransportError](err);
			},
			close: () => {
				if (!this.#closed) this.owner?.[kOnTransportClose]();
			},
		});
	}

	/** send the ClientHello */
	startClient(): void {
		if (this.#closed) return;
		let flags: number;
		try {
			flags = this.conn.start();
		} catch (err) {
			this.#fail(err as Error);
			return;
		}
		this.#process(flags);
	}

	/** resume the handshake paused for the SNI callback */
	certCbDone(): void {
		if (this.#closed) return;
		let flags: number;
		try {
			flags = this.conn.certCbDone();
		} catch (err) {
			this.#fail(err as Error);
			return;
		}
		this.#process(flags);
	}

	/** send what the engine produced (handshake records, alerts) */
	flushEncrypted(): void {
		this.#flush();
	}

	#onData(chunk: Buffer): void {
		if (this.#closed) return;
		let flags: number;
		try {
			flags = this.conn.receive(chunk);
		} catch (err) {
			this.#fail(err as Error);
			return;
		}
		this.#process(flags);
	}

	#fail(err: Error): void {
		if (this.#closed) return;
		// the alert describing the failure
		this.#flush();
		this.owner?.[kOnError](err);
	}

	#process(flags: number): void {
		if (this.#closed) return;
		if (flags & native.EVENTS) {
			this.#dispatchEvents();
			if (this.#closed) return;
		}
		this.#flush();
		if (flags & native.CERTCB && !this.#certCbDispatched) {
			this.#certCbDispatched = true;
			this.owner?.[kOnCertCb]();
			if (this.#closed) return;
		}
		if (flags & native.HANDSHAKE_DONE) {
			this.#secure = true;
			this.owner?.[kOnHandshakeDone]();
			if (this.#closed) return;
			this.#drainQueue();
			if (this.#closed) return;
		}
		if (flags & native.EOF) this.#eofPending = true;
		this.#deliver();
	}

	#dispatchEvents(): void {
		const events = this.conn.takeEvents();
		if (!events) return;
		for (const ev of events) {
			if (this.#closed || !this.owner) return;
			if (ev[0] === "keylog") this.owner.emit("keylog", ev[1]);
			else if (ev[0] === "session") this.owner[kOnNewSession](ev[1] as Buffer, ev[2] as Buffer);
		}
	}

	#deliver(): void {
		if (this.#closed) return;
		const clear = this.conn.readCleartext();
		if (clear) this.#held.push(clear);
		while (this.#userReading && this.#held.length > 0 && !this.#closed) {
			const buf = this.#held.shift() as Buffer;
			this.onread?.(buf);
		}
		if (this.#closed) return;
		if (this.#held.length > 0) {
			// backpressure: stop reading the transport until readStart()
			if (this.#secure) this.transport?.pause();
			return;
		}
		if (this.#eofPending && !this.#eofDelivered) {
			this.#eofDelivered = true;
			this.onread?.(null);
		}
	}

	/** write the encrypted output of the engine, `cb` after the transport flushed it */
	#flush(cb?: WriteCb): boolean {
		const t = this.transport;
		if (!t || this.#closed) return false;
		const enc = this.conn.takeEncrypted();
		if (!enc) return false;
		t.write(enc, cb ?? noop);
		return true;
	}

	#writeNow(data: Buffer, cb: WriteCb): boolean {
		let n = 0;
		try {
			if (data.byteLength > 0) n = this.conn.writeCleartext(data);
		} catch (err) {
			process.nextTick(cb, err as Error);
			this.#fail(err as Error);
			return false;
		}
		if (n < data.byteLength) {
			// a renegotiation waits for the peer
			const rest = data.subarray(n);
			this.#queue.unshift({ data: rest, cb });
			this.#queuedBytes += rest.byteLength;
			this.#flush();
			return false;
		}
		if (!this.#flush(cb)) process.nextTick(cb, null);
		return true;
	}

	#drainQueue(): void {
		while (this.#queue.length > 0 && !this.#closed) {
			const w = this.#queue.shift() as PendingWrite;
			this.#queuedBytes -= w.data.byteLength;
			if (!this.#writeNow(w.data, w.cb)) return;
		}
		if (this.#queue.length === 0 && this.#shutdownCb && !this.#closed) {
			const cb = this.#shutdownCb;
			this.#shutdownCb = null;
			this.#doShutdown(cb);
		}
	}

	#doShutdown(cb: WriteCb): void {
		try {
			this.conn.shutdown();
		} catch {}
		this.#flush();
		const t = this.transport;
		if (!t) {
			process.nextTick(cb, null);
			return;
		}
		t.end(() => cb(null));
	}

	#onTransportEnd(): void {
		if (this.#closed) return;
		if (!this.#secure) {
			this.owner?.[kOnEarlyEnd]();
			if (this.#closed) return;
		}
		this.#eofPending = true;
		this.#deliver();
	}

	// -- StreamHandle -----------------------------------------------------------

	readStart(): void {
		this.#userReading = true;
		this.reading = true;
		if (this.#closed) return;
		if (this.#secure) this.transport?.resume();
		if (this.#held.length > 0 || (this.#eofPending && !this.#eofDelivered)) {
			process.nextTick(() => {
				if (this.#userReading) this.#deliver();
			});
		}
	}

	readStop(): void {
		this.#userReading = false;
		this.reading = false;
		// the handshake needs the transport: pause it only once secure
		if (this.#secure && !this.#closed) this.transport?.pause();
	}

	write(data: Uint8Array | string, cb: WriteCb): void {
		const buf =
			typeof data === "string"
				? Buffer.from(data, "utf8")
				: Buffer.isBuffer(data)
					? data
					: Buffer.from(data.buffer, data.byteOffset, data.byteLength);
		if (this.#closed) {
			process.nextTick(cb, socketClosed());
			return;
		}
		if (!this.#secure || this.#queue.length > 0) {
			this.#queue.push({ data: buf, cb });
			this.#queuedBytes += buf.byteLength;
			return;
		}
		this.#writeNow(buf, cb);
	}

	writev(chunks: (Uint8Array | string)[], cb: WriteCb): void {
		const bufs = chunks.map((c) => (typeof c === "string" ? Buffer.from(c, "utf8") : c));
		this.write(Buffer.concat(bufs), cb);
	}

	shutdown(cb: WriteCb): void {
		if (this.#closed) {
			process.nextTick(cb, null);
			return;
		}
		if (!this.#secure || this.#queue.length > 0) {
			this.#shutdownCb = cb;
			return;
		}
		this.#doShutdown(cb);
	}

	close(cb?: () => void): void {
		if (this.#closed) {
			if (cb) process.nextTick(cb);
			return;
		}
		this.#closed = true;
		this.reading = false;
		this.#userReading = false;
		const queue = this.#queue;
		this.#queue = [];
		this.#queuedBytes = 0;
		this.#held = [];
		for (const w of queue) process.nextTick(w.cb, socketClosed());
		const shutdownCb = this.#shutdownCb;
		this.#shutdownCb = null;
		if (shutdownCb) process.nextTick(shutdownCb, null);
		this.conn.destroy();
		const t = this.transport;
		this.transport = null;
		if (t) t.destroy(() => cb?.());
		else if (cb) process.nextTick(cb);
	}

	ref(): void {
		this.transport?.ref();
	}

	unref(): void {
		this.transport?.unref();
	}

	hasRef(): boolean {
		return this.transport?.hasRef() ?? false;
	}

	setBlocking(blocking: boolean): void {
		this.transport?.setBlocking(blocking);
	}

	getsockname(): SockAddr | null {
		return this.transport?.getsockname() ?? null;
	}

	getpeername(): SockAddr | null {
		return this.transport?.getpeername() ?? null;
	}

	setNoDelay(enable: boolean): void {
		this.transport?.setNoDelay(enable);
	}

	setKeepAlive(enable: boolean, delaySecs: number): void {
		this.transport?.setKeepAlive(enable, delaySecs);
	}

	connect(host: string, port: number | undefined, cb: WriteCb): void {
		const t = this.transport;
		if (!t || typeof t.connect !== "function") {
			throw codedError("ERR_INVALID_HANDLE_TYPE", "This handle type cannot be connected", TypeError);
		}
		t.connect(host, port, (err) => {
			if (!err) this.attach();
			cb(err);
		});
	}

	bind(host: string, port?: number, ipv6Only?: boolean): void {
		const t = this.transport;
		if (!t || typeof t.bind !== "function")
			throw codedError("ERR_INVALID_HANDLE_TYPE", "This handle type cannot be bound", TypeError);
		t.bind(host, port, ipv6Only);
	}
}

// ---------------------------------------------------------------------------
// sessions (Node's wrapped session format binds a session to its server name)

const kTLSSessionStatePrefix = Buffer.from("\0nodejs:tls:session:1\0");

interface SessionOptions {
	servername?: string;
	host?: string;
	socket?: DuplexLike | null;
	isServer?: boolean;
	rejectUnauthorized?: boolean;
}

function getSessionServerIdentity(options: SessionOptions | null | undefined): string {
	return options?.servername || options?.host || options?.socket?._host || "localhost";
}

function wrapSessionState(session: Buffer | undefined, options: SessionOptions | null | undefined): Buffer | undefined {
	if (!Buffer.isBuffer(session) || options?.isServer) return session;
	const servername = Buffer.from(getSessionServerIdentity(options), "utf8");
	const len = Buffer.allocUnsafe(2);
	len.writeUInt16BE(servername.length, 0);
	return Buffer.concat([kTLSSessionStatePrefix, len, servername, session]);
}

function unwrapSessionState(session: unknown): { servername: string; session: Buffer } | undefined {
	if (!Buffer.isBuffer(session) || session.length < kTLSSessionStatePrefix.length + 2) return undefined;
	if (Buffer.compare(session.subarray(0, kTLSSessionStatePrefix.length), kTLSSessionStatePrefix) !== 0)
		return undefined;
	const start = kTLSSessionStatePrefix.length;
	const nameLen = session.readUInt16BE(start);
	const nameStart = start + 2;
	const nameEnd = nameStart + nameLen;
	if (session.length < nameEnd) return undefined;
	return { servername: session.toString("utf8", nameStart, nameEnd), session: session.subarray(nameEnd) };
}

function getSessionForReuse(session: unknown, options: SessionOptions | null | undefined): Buffer | undefined {
	let s = session;
	if (typeof s === "string") s = Buffer.from(s, "latin1");
	if (options?.isServer) return s as Buffer | undefined;
	const wrapped = unwrapSessionState(s);
	if (wrapped !== undefined) {
		if (wrapped.servername !== getSessionServerIdentity(options)) return undefined;
		return wrapped.session;
	}
	// a raw session is not bound to a server name: not reused by verified clients
	if (Buffer.isBuffer(s) && options?.rejectUnauthorized !== false) return undefined;
	return Buffer.isBuffer(s) ? s : undefined;
}

// ---------------------------------------------------------------------------
// TLSSocket

const kWrap = Symbol("wrap");
const kConnectOptions = Symbol("connect-options");
const kDisableRenegotiation = Symbol("disable-renegotiation");
const kErrorEmitted = Symbol("error-emitted");
const kHandshakeTimeout = Symbol("handshake-timeout");
const kSecureContext = Symbol("secureContext");
const kSNICallback = Symbol("snicallback");
const kALPNCallback = Symbol("alpncallback");
const kEnableTrace = Symbol("enableTrace");
const kPendingSession = Symbol("pendingSession");
const kIsVerified = Symbol("verified");
const kHandshakes = Symbol("handshakes");

let warnOnAllowUnauthorized = true;
function getAllowUnauthorized(): boolean {
	const allow = process.env.NODE_TLS_REJECT_UNAUTHORIZED === "0";
	if (allow && warnOnAllowUnauthorized) {
		warnOnAllowUnauthorized = false;
		process.emitWarning?.(
			"Setting the NODE_TLS_REJECT_UNAUTHORIZED environment variable to '0' makes TLS connections and HTTPS requests insecure by disabling certificate verification.",
		);
	}
	return allow;
}

function keylogNewListener(this: TLSSocket, event: string | symbol): void {
	if (event !== "keylog") return;
	const wrap = this[kWrap];
	if (!wrap || wrap.closed) return;
	wrap.conn.enableKeylogCallback();
	this.removeListener("newListener", keylogNewListener);
}

function sessionNewListener(this: TLSSocket, event: string | symbol): void {
	if (event !== "session") return;
	const wrap = this[kWrap];
	if (!wrap || wrap.closed) return;
	wrap.conn.enableSessionCallbacks();
	this.removeListener("newListener", sessionNewListener);
}

function initRead(tlsSocket: TLSSocket): void {
	if (!tlsSocket._handle) return;
	tlsSocket.read(0);
}

function onSocketCloseDestroySSL(this: TLSSocket): void {
	setImmediate(() => this._destroySSL());
}

/** Node's callALPNCallback: the offset of the selected protocol in the client's list */
function callALPNCallback(socket: TLSSocket, protocolsBuffer: Buffer): number | undefined {
	try {
		const wrap = socket[kWrap];
		const servername = wrap.conn.getServername();
		const protocols: string[] = [];
		let offset = 0;
		while (offset < protocolsBuffer.length) {
			const len = protocolsBuffer[offset];
			offset += 1;
			protocols.push(protocolsBuffer.subarray(offset, offset + len).toString("ascii"));
			offset += len;
		}
		const cb = socket[kALPNCallback] as NonNullable<TLSSocketOptions["ALPNCallback"]>;
		const selected = cb({ servername, protocols });
		if (selected === undefined) return undefined;
		const index = protocols.indexOf(selected);
		if (index === -1) {
			throw codedError(
				"ERR_TLS_ALPN_CALLBACK_INVALID_RESULT",
				`ALPN callback returned a value (${selected}) that did not match any of the client's offered protocols (${protocols.join(", ")})`,
				TypeError,
			);
		}
		let protocolOffset = 0;
		for (let i = 0; i < index; i++) protocolOffset += 1 + Buffer.byteLength(protocols[i], "ascii");
		return protocolOffset;
	} catch (err) {
		socket.destroy(err as Error);
		return undefined;
	}
}

export class TLSSocket extends net.Socket {
	encrypted = true;
	_tlsOptions: TLSSocketOptions = {};
	_secureEstablished = false;
	_securePending = false;
	_newSessionPending = false;
	_controlReleased = false;
	secureConnecting = true;
	_SNICallback: TLSSocketOptions["SNICallback"] | null = null;
	servername: string | false | null = null;
	alpnProtocol: string | false | null = null;
	authorized = false;
	authorizationError: string | null = null;
	_requestCert = false;
	_rejectUnauthorized = false;
	ssl: TLSWrap | null = null;
	socket: DuplexLike | null = null;
	[kWrap]: TLSWrap;
	[kSecureContext]: SecureContext | null = null;
	[kALPNCallback]: TLSSocketOptions["ALPNCallback"] | null = null;
	[kConnectOptions]: ConnectionOptions | null = null;
	[kDisableRenegotiation] = false;
	[kErrorEmitted] = false;
	[kPendingSession]: Buffer | undefined = undefined;
	[kIsVerified] = false;
	[kHandshakes] = 0;
	lastHandshakeTime = 0;

	constructor(socket?: DuplexLike | null, opts?: TLSSocketOptions) {
		const tlsOptions: TLSSocketOptions = { ...opts };
		validateOpenSSLEngineOptions(tlsOptions);
		if (tlsOptions.enableTrace !== undefined && typeof tlsOptions.enableTrace !== "boolean") {
			throw invalidArgType("options.enableTrace", "of type boolean", tlsOptions.enableTrace);
		}
		if (tlsOptions.ALPNProtocols) convertALPNProtocols(tlsOptions.ALPNProtocols, tlsOptions);
		if (tlsOptions.pskCallback) throw notSupported("TLS-PSK (options.pskCallback)");

		const context = tlsOptions.secureContext || tlsOptions.credentials || createSecureContext(tlsOptions);
		const nativeCtx = nativeContextOf(context, "context");
		const conn = native.createConnection(nativeCtx, Boolean(tlsOptions.isServer));
		const fromHandle = !socket;
		const transport: Transport = socket
			? new StreamTransport(socket)
			: new HandleTransport(tlsOptions.pipe ? netNative.createPipe(false) : netNative.createTCP());
		const wrap = new TLSWrap(conn, transport, fromHandle);

		super({
			handle: wrap,
			allowHalfOpen: socket ? Boolean(socket.allowHalfOpen) : tlsOptions.allowHalfOpen,
			pauseOnCreate: tlsOptions.pauseOnConnect,
			manualStart: true,
			highWaterMark: tlsOptions.highWaterMark,
			onread: !socket ? tlsOptions.onread : null,
			signal: tlsOptions.signal,
			noDelay: tlsOptions.noDelay,
			keepAlive: tlsOptions.keepAlive,
			keepAliveInitialDelay: tlsOptions.keepAliveInitialDelay,
		});

		wrap.owner = this;
		this[kWrap] = wrap;
		this[kSecureContext] = context instanceof SecureContext ? context : null;
		this._tlsOptions = tlsOptions;
		this.ssl = wrap;
		this.socket = socket ?? null;

		this.on("close", onSocketCloseDestroySSL);

		this.on("error", this._tlsError);
		this._init(socket ?? undefined);

		if (tlsOptions.enableTrace) this.enableTrace();

		// read on the next tick: the caller sets up its listeners first
		process.nextTick(initRead, this);
	}

	_init(socket?: DuplexLike): void {
		const options = this._tlsOptions;
		const wrap = this[kWrap];
		const conn = wrap.conn;
		this.server = (options.server ?? null) as net.Server | null;

		// clients always request a certificate, servers on explicit configuration
		const requestCert = Boolean(options.requestCert) || !options.isServer;
		const rejectUnauthorized = Boolean(options.rejectUnauthorized);
		this._requestCert = requestCert;
		this._rejectUnauthorized = rejectUnauthorized;
		if (requestCert || rejectUnauthorized) conn.setVerifyMode(requestCert, rejectUnauthorized);

		if (!this.listeners("newListener").includes(keylogNewListener)) this.on("newListener", keylogNewListener);

		if (options.isServer) {
			if (options.ALPNCallback) {
				validateFunction(options.ALPNCallback, "options.ALPNCallback");
				this[kALPNCallback] = options.ALPNCallback;
				conn.setALPNCallback((buf) => callALPNCallback(this, buf));
			}
			const server = options.server;
			if (server && (server.listenerCount("resumeSession") > 0 || server.listenerCount("newSession") > 0)) {
				conn.enableSessionCallbacks();
			}
		} else {
			if (options.session) {
				try {
					conn.setSession(options.session);
				} catch {}
			}
			if (!this.listeners("newListener").includes(sessionNewListener)) this.on("newListener", sessionNewListener);
		}

		if (
			options.isServer &&
			options.SNICallback &&
			(options.SNICallback !== defaultSNICallback || (options.server && options.server._contexts.length > 0))
		) {
			validateFunction(options.SNICallback, "options.SNICallback");
			this._SNICallback = options.SNICallback;
			conn.enableCertCb();
		}

		if (options.ALPNProtocols) conn.setALPNProtocols(options.ALPNProtocols as Buffer);

		if (options.handshakeTimeout !== undefined && options.handshakeTimeout > 0) {
			this.setTimeout(options.handshakeTimeout, this._handleTimeout);
		}

		if (socket instanceof net.Socket) {
			this._parent = socket;
			// the connect event of the wrapped socket starts the handshake
			this.connecting = socket.connecting || !socket._handle;
			socket.once("connect", () => {
				this.connecting = false;
				this.emit("connect");
			});
		}

		if (socket) wrap.attach();
		else this.connecting = true;
	}

	// -- engine events --------------------------------------------------------

	[kOnHandshakeDone](): void {
		if (this._tlsOptions.isServer) {
			const now = Date.now();
			const last = this.lastHandshakeTime;
			this.lastHandshakeTime = now;
			if (last !== 0) {
				if (now - last >= CLIENT_RENEG_WINDOW * 1000) this[kHandshakes] = 1;
				else this[kHandshakes]++;
				if (this[kHandshakes] > CLIENT_RENEG_LIMIT) {
					this._emitTLSError(codedError("ERR_TLS_SESSION_ATTACK", "TLS session renegotiation attack detected"));
					return;
				}
				if (this[kDisableRenegotiation]) {
					this._emitTLSError(
						codedError("ERR_TLS_RENEGOTIATION_DISABLED", "TLS session renegotiation disabled for this socket"),
					);
					return;
				}
			}
		}
		this._finishInit();
	}

	[kOnError](err: Error): void {
		if (this._hadError) return;
		this._hadError = true;
		if (!this._secureEstablished) {
			// destroy after the 'tlsClientError' listeners saw the socket
			this._closeAfterHandlingError = true;
			this.destroy(err);
		} else if (
			this._tlsOptions.isServer &&
			this._rejectUnauthorized &&
			/peer did not return a certificate/.test(err.message)
		) {
			this.destroy();
		} else {
			this._emitTLSError(err);
		}
	}

	[kOnCertCb](): void {
		const wrap = this[kWrap];
		const servername = wrap.conn.getServername();
		if (!servername || !this._SNICallback) {
			wrap.certCbDone();
			return;
		}
		let once = false;
		try {
			this._SNICallback.call(this, servername, (err, context) => {
				if (once) {
					this.destroy(codedError("ERR_MULTIPLE_CALLBACK", "Callback called multiple times"));
					return;
				}
				once = true;
				if (err) {
					this.destroy(err);
					return;
				}
				if (!this._handle || wrap.closed) {
					this.destroy(socketClosed());
					return;
				}
				if (context) {
					try {
						wrap.conn.setSNIContext(nativeContextOf(context, "context"));
					} catch (e) {
						this.destroy(e as Error);
						return;
					}
				}
				wrap.certCbDone();
			});
		} catch (err) {
			this.destroy(err as Error);
		}
	}

	[kOnNewSession](sessionId: Buffer, session: Buffer): void {
		if (this._tlsOptions.isServer) {
			const server = this._tlsOptions.server;
			if (!server) return;
			try {
				server.emit("newSession", sessionId, session, noop);
			} catch (err) {
				server.emit("error", err);
			}
			return;
		}
		const wrapped = wrapSessionState(session, this[kConnectOptions]);
		if (this[kIsVerified]) this.emit("session", wrapped);
		else this[kPendingSession] = wrapped;
	}

	[kOnEarlyEnd](): void {
		if (this._tlsOptions.isServer) {
			// the client went away during the handshake
			this.destroy();
			return;
		}
		if (this._hadError) return;
		const options: ConnectionOptions = this[kConnectOptions] ?? {};
		this._hadError = true;
		const error = connResetException(
			"Client network socket disconnected before secure TLS connection was established",
		) as Error & {
			path?: string;
			host?: string;
			port?: number;
			localAddress?: string;
		};
		error.path = options.path;
		error.host = options.host;
		error.port = options.port;
		error.localAddress = options.localAddress;
		this.destroy(error);
	}

	[kOnTransportError](err: Error): void {
		this._emitTLSError(err);
	}

	[kOnTransportClose](): void {
		this.destroy();
	}

	// -- Node's TLSSocket internals ------------------------------------------

	_handleTimeout(): void {
		this._emitTLSError(codedError("ERR_TLS_HANDSHAKE_TIMEOUT", "TLS handshake timeout"));
	}

	_emitTLSError(err: Error): void {
		const e = this._tlsError(err);
		if (e) this.emit("error", e);
	}

	_tlsError(err: Error): Error | null {
		this.emit("_tlsError", err);
		if (this._controlReleased) return err;
		return null;
	}

	_releaseControl(): boolean {
		if (this._controlReleased) return false;
		this._controlReleased = true;
		this.removeListener("error", this._tlsError);
		return true;
	}

	_finishInit(): void {
		const wrap = this[kWrap];
		if (!this._handle || wrap.closed) return;
		this.alpnProtocol = wrap.conn.getALPNNegotiatedProtocol();
		if (this.servername === null) this.servername = wrap.conn.getServername();
		this._secureEstablished = true;
		if (this._tlsOptions.handshakeTimeout !== undefined && this._tlsOptions.handshakeTimeout > 0) {
			this.setTimeout(0, this._handleTimeout);
		}
		this.emit("secure");
	}

	_start(): void {
		if (this.connecting) {
			this.once("connect", this._start);
			return;
		}
		if (!this._handle) return;
		this[kWrap].startClient();
	}

	_destroySSL(): void {
		const wrap = this[kWrap];
		if (wrap && !wrap.closed) wrap.close();
		const ctx = this[kSecureContext];
		if (ctx && ctx.singleUse && ctx.context) {
			ctx.context.close();
			ctx.context = null;
		}
		this[kSecureContext] = null;
		this[kPendingSession] = undefined;
		this[kIsVerified] = false;
	}

	// -- public API -----------------------------------------------------------

	disableRenegotiation(): void {
		this[kDisableRenegotiation] = true;
	}

	renegotiate(
		options: { rejectUnauthorized?: boolean; requestCert?: boolean },
		callback?: (err: Error | null) => void,
	): boolean | undefined {
		validateObject(options, "options");
		if (callback !== undefined) validateFunction(callback, "callback");
		if (this.destroyed) return undefined;
		const wrap = this[kWrap];
		let requestCert = this._requestCert;
		let rejectUnauthorized = this._rejectUnauthorized;
		if (options.requestCert !== undefined) requestCert = Boolean(options.requestCert);
		if (options.rejectUnauthorized !== undefined) rejectUnauthorized = Boolean(options.rejectUnauthorized);
		if (requestCert !== this._requestCert || rejectUnauthorized !== this._rejectUnauthorized) {
			wrap.conn.setVerifyMode(requestCert, rejectUnauthorized);
			this._requestCert = requestCert;
			this._rejectUnauthorized = rejectUnauthorized;
		}
		try {
			wrap.conn.renegotiate();
		} catch (err) {
			const e = err as Error & { code?: string };
			const error =
				e.code === "ERR_SSL_WRONG_SSL_VERSION"
					? codedError("ERR_TLS_RENEGOTIATION_UNSUPPORTED", "Attempt to renegotiate TLS session not supported")
					: e;
			if (callback) process.nextTick(callback, error);
			return false;
		}
		wrap.flushEncrypted();
		if (callback) this.once("secure", () => callback(null));
		return true;
	}

	exportKeyingMaterial(length: number, label: string, context?: Buffer): Buffer {
		validateNumber(length, "length", 0, 2 ** 32 - 1);
		if (!Number.isInteger(length)) throw invalidArgType("length", "an integer", length);
		validateString(label, "label");
		if (context !== undefined && !ArrayBuffer.isView(context))
			throw invalidArgType("context", "an instance of Buffer, TypedArray, or DataView", context);
		if (!this._secureEstablished)
			throw codedError("ERR_TLS_INVALID_STATE", "TLS socket connection must be securely established");
		return this[kWrap].conn.exportKeyingMaterial(length, label, context);
	}

	setMaxSendFragment(size: number): boolean {
		validateNumber(size, "size", -(2 ** 31), 2 ** 31 - 1);
		return this[kWrap].conn.setMaxSendFragment(size);
	}

	setServername(name: string): void {
		validateString(name, "name");
		if (this._tlsOptions.isServer)
			throw codedError("ERR_TLS_SNI_FROM_SERVER", "Cannot issue SNI from a TLS server-side socket");
		this[kWrap].conn.setServername(name);
	}

	setSession(session: Buffer | string): void {
		const s = getSessionForReuse(session, this[kConnectOptions] || this._tlsOptions);
		if (s !== undefined) this[kWrap].conn.setSession(s);
	}

	getSession(): Buffer | undefined | null {
		if (!this._handle) return null;
		return wrapSessionState(this[kWrap].conn.getSession(), this[kConnectOptions] || this._tlsOptions);
	}

	getPeerCertificate(detailed?: boolean): PeerCertificate | Record<string, never> | null {
		if (!this._handle) return null;
		return translatePeerCertificate(this[kWrap].conn.getPeerCertificate(Boolean(detailed))) || {};
	}

	getCertificate(): PeerCertificate | Record<string, never> | null {
		if (!this._handle) return null;
		return translatePeerCertificate(this[kWrap].conn.getCertificate()) || {};
	}

	getPeerX509Certificate(_detailed?: boolean): unknown {
		if (!this._handle) return undefined;
		const raw = this[kWrap].conn.getPeerCertificate(false)?.raw;
		return raw ? new X509Certificate(raw) : undefined;
	}

	getX509Certificate(): unknown {
		if (!this._handle) return undefined;
		const raw = this[kWrap].conn.getCertificate()?.raw;
		return raw ? new X509Certificate(raw) : undefined;
	}

	setKeyCert(context: SecureContext | SecureContextOptions): void {
		if (!this._handle) return;
		const ctx = context instanceof SecureContext ? context : createSecureContext(context);
		this[kWrap].conn.setSNIContext(nativeContextOf(ctx, "context"));
	}

	getCipher(): CipherNameAndProtocol | null | undefined {
		return this._handle ? this[kWrap].conn.getCipher() : null;
	}

	getSharedSigalgs(): string[] | null | undefined {
		return this._handle ? this[kWrap].conn.getSharedSigalgs() : null;
	}

	getEphemeralKeyInfo(): EphemeralKeyInfo | null {
		return this._handle ? this[kWrap].conn.getEphemeralKeyInfo() : null;
	}

	getFinished(): Buffer | null | undefined {
		return this._handle ? this[kWrap].conn.getFinished() : null;
	}

	getPeerFinished(): Buffer | null | undefined {
		return this._handle ? this[kWrap].conn.getPeerFinished() : null;
	}

	getProtocol(): string | null {
		return this._handle ? this[kWrap].conn.getProtocol() : null;
	}

	getTLSTicket(): Buffer | null | undefined {
		return this._handle ? this[kWrap].conn.getTLSTicket() : null;
	}

	isSessionReused(): boolean | null {
		return this._handle ? this[kWrap].conn.isSessionReused() : null;
	}

	enableTrace(): void {
		if (this._handle) this[kWrap].conn.enableTrace();
	}
}

// ---------------------------------------------------------------------------
// Server

function onServerSocketSecure(this: TLSSocket): void {
	if (this._requestCert) {
		const wrap = this[kWrap];
		const verifyError = wrap.conn.verifyError();
		if (verifyError) {
			this.authorizationError = verifyError.code;
			if (this._rejectUnauthorized) this.destroy();
		} else if (!wrap.conn.hasPeerCertificate()) {
			// TLSv1.3 resumption without a certificate
			this.authorizationError = "UNABLE_TO_GET_ISSUER_CERT";
			if (this._rejectUnauthorized) this.destroy();
		} else {
			this.authorized = true;
		}
	}
	if (!this.destroyed && this._releaseControl()) {
		this.secureConnecting = false;
		this._tlsOptions.server?.emit("secureConnection", this);
	}
}

function onSocketTLSError(this: TLSSocket, err: Error): void {
	if (!this._controlReleased && !this[kErrorEmitted]) {
		this[kErrorEmitted] = true;
		this._tlsOptions.server?.emit("tlsClientError", err, this);
	}
}

function onSocketKeylog(this: TLSSocket, line: Buffer): void {
	this._tlsOptions.server?.emit("keylog", line, this);
}

function onSocketClose(this: TLSSocket, hadError: boolean): void {
	if (hadError) return;
	if (!this._controlReleased && !this[kErrorEmitted]) {
		this[kErrorEmitted] = true;
		this._tlsOptions.server?.emit("tlsClientError", connResetException("socket hang up"), this);
	}
}

function tlsConnectionListener(this: Server, rawSocket: net.Socket): void {
	// net.Socket implements DuplexLike (its declared types are narrower)
	const socket = new TLSSocket(rawSocket as unknown as DuplexLike, {
		secureContext: this._sharedCreds,
		isServer: true,
		server: this,
		requestCert: this.requestCert,
		rejectUnauthorized: this.rejectUnauthorized,
		handshakeTimeout: this[kHandshakeTimeout],
		ALPNProtocols: this.ALPNProtocols,
		ALPNCallback: this.ALPNCallback,
		SNICallback: this[kSNICallback] || defaultSNICallback,
		enableTrace: this[kEnableTrace],
		pauseOnConnect: this.pauseOnConnect,
	});
	socket.on("secure", onServerSocketSecure);
	if (this.listenerCount("keylog") > 0) socket.on("keylog", onSocketKeylog);
	socket[kErrorEmitted] = false;
	socket.on("close", onSocketClose);
	socket.on("_tlsError", onSocketTLSError);
	socket.on("error", onSocketTLSError);
}

/** the default SNICallback: the contexts added with server.addContext() */
function defaultSNICallback(
	this: TLSSocket,
	servername: string,
	callback: (err: Error | null, ctx?: SecureContext) => void,
): void {
	const server = this.server as Server | null;
	const contexts = server?._contexts ?? [];
	for (let i = contexts.length - 1; i >= 0; --i) {
		const [re, ctx] = contexts[i];
		if (re.test(servername)) {
			callback(null, ctx);
			return;
		}
	}
	callback(null, undefined);
}

export interface TlsServerOptions extends TLSSocketOptions {
	sessionTimeout?: number;
	ticketKeys?: Buffer;
	[key: string]: unknown;
}

/** a 32 character session id context derived from the command line (Node hashes argv with SHA-1) */
function defaultSessionIdContext(): string {
	return createHash("sha1").update(process.argv.join(" ")).digest("hex").slice(0, 32);
}

export class Server extends net.Server {
	_contexts: [RegExp, SecureContext][] = [];
	requestCert = false;
	rejectUnauthorized = true;
	ALPNCallback: TLSSocketOptions["ALPNCallback"] = undefined;
	ALPNProtocols: Buffer | undefined = undefined;
	sessionTimeout: number | undefined = undefined;
	ticketKeys: Buffer | undefined = undefined;
	pfx: SecureContextOptions["pfx"] = undefined;
	key: SecureContextOptions["key"] = undefined;
	passphrase: string | undefined = undefined;
	cert: SecureContextOptions["cert"] = undefined;
	ca: SecureContextOptions["ca"] = undefined;
	minVersion: string | undefined = undefined;
	maxVersion: string | undefined = undefined;
	secureProtocol: string | undefined = undefined;
	crl: SecureContextOptions["crl"] = undefined;
	sigalgs: string | undefined = undefined;
	ciphers: string | undefined = undefined;
	ecdhCurve: string | undefined = undefined;
	dhparam: SecureContextOptions["dhparam"] = undefined;
	honorCipherOrder = true;
	secureOptions: number | undefined = undefined;
	sessionIdContext: string | undefined = undefined;
	certificateCompression: string[] | undefined = undefined;
	_sharedCreds: SecureContext | undefined = undefined;
	[kHandshakeTimeout] = 120 * 1000;
	[kSNICallback]: TLSSocketOptions["SNICallback"] = undefined;
	[kEnableTrace]: boolean | undefined = undefined;

	constructor(options?: TlsServerOptions | ((socket: TLSSocket) => void), listener?: (socket: TLSSocket) => void) {
		let opts: TlsServerOptions;
		let secureListener = listener;
		if (typeof options === "function") {
			secureListener = options;
			opts = {};
		} else if (options == null || typeof options === "object") {
			opts = options ?? {};
		} else {
			throw invalidArgType("options", "of type object", options);
		}
		if (opts.ALPNCallback && opts.ALPNProtocols) {
			throw codedError(
				"ERR_TLS_ALPN_CALLBACK_WITH_PROTOCOLS",
				"The ALPNCallback and ALPNProtocols TLS options are mutually exclusive",
				TypeError,
			);
		}
		super(opts, tlsConnectionListener as unknown as (socket: net.Socket) => void);

		this.requestCert = opts.requestCert === true;
		this.rejectUnauthorized = opts.rejectUnauthorized !== false;
		this.ALPNCallback = opts.ALPNCallback;
		if (opts.sessionTimeout) this.sessionTimeout = opts.sessionTimeout;
		if (opts.ticketKeys) this.ticketKeys = opts.ticketKeys;
		if (opts.ALPNProtocols) convertALPNProtocols(opts.ALPNProtocols, this);
		this.setSecureContext(opts);

		this[kHandshakeTimeout] = opts.handshakeTimeout || 120 * 1000;
		this[kSNICallback] = opts.SNICallback;
		validateNumber(this[kHandshakeTimeout], "options.handshakeTimeout");
		if (this[kSNICallback]) validateFunction(this[kSNICallback], "options.SNICallback");
		if (opts.pskCallback) throw notSupported("TLS-PSK (options.pskCallback)");
		if (secureListener) this.on("secureConnection", secureListener);
		this[kEnableTrace] = opts.enableTrace;
	}

	setSecureContext(options: TlsServerOptions): void {
		validateObject(options, "options");
		validateOpenSSLEngineOptions(options);
		this.pfx = options.pfx || undefined;
		this.key = options.key || undefined;
		this.passphrase = options.passphrase || undefined;
		this.cert = options.cert || undefined;
		this.ca = options.ca || undefined;
		this.minVersion = options.minVersion || undefined;
		this.maxVersion = options.maxVersion || undefined;
		this.secureProtocol = options.secureProtocol || undefined;
		this.crl = options.crl || undefined;
		this.sigalgs = options.sigalgs;
		this.ciphers = options.ciphers || undefined;
		this.ecdhCurve = options.ecdhCurve;
		this.dhparam = options.dhparam || undefined;
		this.honorCipherOrder = options.honorCipherOrder !== undefined ? Boolean(options.honorCipherOrder) : true;
		this.secureOptions = options.secureOptions || undefined;
		this.sessionIdContext = options.sessionIdContext ? options.sessionIdContext : defaultSessionIdContext();
		if (options.sessionTimeout) this.sessionTimeout = options.sessionTimeout;
		if (options.ticketKeys) this.ticketKeys = options.ticketKeys;
		this.certificateCompression = options.certificateCompression;
		this._sharedCreds = createSecureContext({
			pfx: this.pfx,
			key: this.key,
			passphrase: this.passphrase,
			cert: this.cert,
			ca: this.ca,
			ciphers: this.ciphers,
			sigalgs: this.sigalgs,
			ecdhCurve: this.ecdhCurve,
			dhparam: this.dhparam,
			minVersion: this.minVersion,
			maxVersion: this.maxVersion,
			secureProtocol: this.secureProtocol,
			secureOptions: this.secureOptions,
			honorCipherOrder: this.honorCipherOrder,
			crl: this.crl,
			sessionIdContext: this.sessionIdContext,
			ticketKeys: this.ticketKeys,
			sessionTimeout: this.sessionTimeout,
			certificateCompression: this.certificateCompression,
		});
	}

	_getServerData(): { ticketKeys: string } {
		return { ticketKeys: this.getTicketKeys().toString("hex") };
	}

	_setServerData(data: { ticketKeys: string }): void {
		this.setTicketKeys(Buffer.from(data.ticketKeys, "hex"));
	}

	getTicketKeys(): Buffer {
		return nativeContextOf(this._sharedCreds, "context").getTicketKeys();
	}

	setTicketKeys(keys: Buffer): void {
		if (!ArrayBuffer.isView(keys))
			throw invalidArgType("buffer", "an instance of Buffer, TypedArray, or DataView", keys);
		if (keys.byteLength !== 48) throw invalidArgValue("keys", keys.byteLength, "must be exactly 48 bytes");
		nativeContextOf(this._sharedCreds, "context").setTicketKeys(keys);
	}

	addContext(servername: string, context: SecureContext | SecureContextOptions): void {
		if (!servername)
			throw codedError("ERR_TLS_REQUIRED_SERVER_NAME", '"servername" is required parameter for Server.addContext');
		const re = new RegExp(`^${servername.replace(/([.^$+?\-\\[\]{}])/g, "\\$1").replaceAll("*", "[^.]*")}$`, "i");
		const secureContext = context instanceof SecureContext ? context : createSecureContext(context);
		this._contexts.push([re, secureContext]);
	}

	[captureRejectionSymbol](err: Error, event: string | symbol, sock?: net.Socket): void {
		// a rejected 'secureConnection'/'connection' listener destroys the socket (Node)
		if ((event === "secureConnection" || event === "connection") && sock) sock.destroy(err);
		else this.emit("error", err);
	}
}

export function createServer(
	options?: TlsServerOptions | ((socket: TLSSocket) => void),
	listener?: (socket: TLSSocket) => void,
): Server {
	return new Server(options, listener);
}

// ---------------------------------------------------------------------------
// connect

function normalizeConnectArgs(listArgs: unknown[]): [ConnectionOptions, Callback | undefined] {
	const args = net._normalizeArgs(listArgs) as unknown as [ConnectionOptions, Callback | null];
	const options: ConnectionOptions = { ...args[0] };
	const extra = listArgs[1] !== null && typeof listArgs[1] === "object" ? listArgs[1] : listArgs[2];
	if (extra !== null && typeof extra === "object") Object.assign(options, extra);
	return [options, args[1] ?? undefined];
}

function onConnectSecure(this: TLSSocket): void {
	const options = this[kConnectOptions] as ConnectionOptions;
	const wrap = this[kWrap];

	const ekeyinfo = this.getEphemeralKeyInfo();
	if (ekeyinfo && ekeyinfo.type === "DH" && (ekeyinfo.size ?? 0) < (options.minDHSize ?? 1024)) {
		const err = codedError("ERR_TLS_DH_PARAM_SIZE", `DH parameter size ${ekeyinfo.size} is less than 2048`);
		this.emit("error", err);
		this.destroy();
		return;
	}

	let verifyError: (Error & { code?: string }) | null | undefined = wrap.conn.verifyError();
	// the server's identity, unless it resumed our session
	if (!verifyError && !this.isSessionReused()) {
		const cert = this.getPeerCertificate(true) as PeerCertificate;
		const check = options.checkServerIdentity ?? defaultCheckServerIdentity;
		verifyError = check(getSessionServerIdentity(options), cert) as (Error & { code?: string }) | undefined;
	}

	if (verifyError) {
		this.authorized = false;
		this.authorizationError = verifyError.code || verifyError.message;
		if (options.rejectUnauthorized !== false) {
			this.destroy(verifyError);
			return;
		}
	} else {
		this.authorized = true;
	}
	this.secureConnecting = false;
	this.emit("secureConnect");

	this[kIsVerified] = true;
	const session = this[kPendingSession];
	this[kPendingSession] = undefined;
	if (session) this.emit("session", session);
}

/** tls.connect([port[, host]][, options][, callback]) */
export function connect(...args: unknown[]): TLSSocket {
	const [userOptions, cb] = normalizeConnectArgs(args);
	const allowUnauthorized = getAllowUnauthorized();
	const options: ConnectionOptions = {
		rejectUnauthorized: !allowUnauthorized,
		ciphers: defaults.ciphers,
		checkServerIdentity: defaultCheckServerIdentity,
		minDHSize: 1024,
		...userOptions,
	};
	validateOpenSSLEngineOptions(options);
	if (!options.keepAlive) options.singleUse = true;
	validateFunction(options.checkServerIdentity, "options.checkServerIdentity");
	validateNumber(options.minDHSize, "options.minDHSize", 1);

	// SNI defaults to the host name (not to IP addresses, RFC 6066)
	if (options.servername === undefined) {
		const host = typeof options.host === "string" ? options.host : options.socket?._host;
		if (typeof host === "string" && host && !isIP(host)) options.servername = host;
	}
	if (options.servername && isIP(options.servername)) {
		throw invalidArgValue(
			"options.servername",
			options.servername,
			"Setting the TLS ServerName to an IP address is not permitted.",
		);
	}
	// SNI is a C string: it ends at a NUL
	const sni = options.servername ? options.servername.split("\0")[0] : options.servername;

	const context = options.secureContext || createSecureContext(options);

	options.session = getSessionForReuse(options.session, options);

	const tlssock = new TLSSocket(options.socket, {
		allowHalfOpen: options.allowHalfOpen,
		pipe: Boolean(options.path),
		secureContext: context,
		isServer: false,
		requestCert: true,
		rejectUnauthorized: options.rejectUnauthorized !== false,
		session: options.session,
		ALPNProtocols: options.ALPNProtocols,
		requestOCSP: options.requestOCSP,
		enableTrace: options.enableTrace,
		highWaterMark: options.highWaterMark,
		onread: options.onread,
		signal: options.signal,
		noDelay: options.noDelay,
		keepAlive: options.keepAlive,
		keepAliveInitialDelay: options.keepAliveInitialDelay,
	});

	options.rejectUnauthorized = options.rejectUnauthorized !== false;
	tlssock[kConnectOptions] = options;

	if (cb) tlssock.once("secureConnect", cb);
	if (options.session) tlssock.setSession(options.session);
	if (sni) tlssock.setServername(sni);

	if (!options.socket) {
		if (options.timeout) tlssock.setTimeout(options.timeout);
		const connectOptions: Record<string, unknown> = {};
		for (const k of [
			"host",
			"port",
			"path",
			"localAddress",
			"localPort",
			"family",
			"hints",
			"lookup",
			"noDelay",
			"keepAlive",
			"keepAliveInitialDelay",
			"autoSelectFamily",
			"autoSelectFamilyAttemptTimeout",
			"blockList",
		]) {
			const v = (options as Record<string, unknown>)[k];
			if (v !== undefined) connectOptions[k] = v;
		}
		tlssock.connect(connectOptions, tlssock._start);
	}

	tlssock._releaseControl();
	if (options.socket) tlssock._start();

	tlssock.on("secure", onConnectSecure);
	return tlssock;
}
