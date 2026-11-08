// WHATWG WebSocket client (Bun flavor) on qbun:http (libcurl's WebSocket
// API on the runtime loop). Bun extensions: the options object
// `{ protocols, protocol, headers, proxy, tls, perMessageDeflate }`,
// binaryType "nodebuffer" (the default), ping()/pong()/terminate(),
// pause()/resume()/isPaused and the "ping"/"pong" events.

import * as native from "qbun:http";
import { Blob } from "node:buffer";
import { URL } from "node:url";
import { setTimeout, clearTimeout } from "node:timers";
import type { Timeout } from "node:timers";
import { TextEncoder } from "internal:encoding";
import { CloseEvent, ErrorEvent, Event, EventTarget, MessageEvent } from "internal:event-target";
import { Headers, fillHeaders, isToken, wireEntries } from "internal:fetch/headers";
import type { HeadersInit } from "internal:fetch/headers";
import { isBlobLike } from "internal:fetch/formdata";
import { resolveProxy, resolveTransport, transportOptions } from "internal:fetch/http";
import type { NativeErrorInfo, ProxyOption, TLSOptions } from "internal:fetch/http";

interface NativeSocket {
	send(data: string | Uint8Array, binary: boolean): void;
	ping(data?: Uint8Array | string): void;
	pong(data?: Uint8Array | string): void;
	close(code: number, reason: string): void;
	terminate(): void;
	pause(): void;
	resume(): void;
	ref(): void;
	unref(): void;
	hasRef(): boolean;
	readonly bufferedAmount: number;
}

interface NativeSocketHandlers {
	onOpen(status: number, headers: string[], url: string): void;
	onMessage(data: string | Uint8Array, binary: boolean): void;
	onPing(data: Uint8Array): void;
	onPong(data: Uint8Array): void;
	onError(info: NativeErrorInfo): void;
	onClose(code: number, reason: string, wasClean: boolean): void;
}

const connect = native.websocket as (options: Record<string, unknown>, handlers: NativeSocketHandlers) => NativeSocket;

export type BinaryType = "blob" | "arraybuffer" | "nodebuffer";

export interface WebSocketOptions {
	protocols?: string | string[];
	protocol?: string;
	headers?: HeadersInit;
	proxy?: ProxyOption;
	tls?: TLSOptions;
	perMessageDeflate?: boolean;
}

const CONNECTING = 0;
const OPEN = 1;
const CLOSING = 2;
const CLOSED = 3;
const CLOSE_TIMEOUT = 30_000;
const encoder = new TextEncoder();

type Handler = ((this: WebSocket, event: Event) => unknown) | null;

interface SocketError extends Error {
	code: string;
	errno?: number;
	syscall?: string;
}

function socketError(info: NativeErrorInfo, url: string): SocketError {
	let message: string;
	if (info.code === "ERR_UNEXPECTED_RESPONSE") {
		message = info.status ? `Expected 101 status code, received ${info.status}` : "Expected 101 status code";
	} else if (info.syscall === "connect" || info.syscall === "getaddrinfo") {
		let host = "";
		try {
			host = new URL(url).host;
		} catch {
			// keep the empty host
		}
		message = `${info.syscall} ${info.code} ${host}`;
	} else {
		message = info.tlsMessage || info.message;
	}
	const err = new Error(message) as SocketError;
	err.code = info.code;
	if (info.errno !== undefined) err.errno = info.errno;
	if (info.syscall) err.syscall = info.syscall;
	return err;
}

function parseURL(input: string | URL): URL {
	let url: URL;
	try {
		url = new URL(String(input));
	} catch {
		throw new DOMException(`Invalid URL: ${String(input)}`, "SyntaxError");
	}
	if (url.protocol === "http:") url.protocol = "ws:";
	else if (url.protocol === "https:") url.protocol = "wss:";
	if (url.protocol !== "ws:" && url.protocol !== "wss:") {
		throw new DOMException(
			`Invalid URL scheme '${url.protocol.slice(0, -1)}': only 'ws' and 'wss' are allowed`,
			"SyntaxError",
		);
	}
	if (url.hash) throw new DOMException("A WebSocket URL must not contain a fragment", "SyntaxError");
	return url;
}

function checkProtocols(protocols: string[]): void {
	const seen = new Set<string>();
	for (const p of protocols) {
		if (!isToken(p)) throw new DOMException(`Invalid protocol: '${p}'`, "SyntaxError");
		const lower = p.toLowerCase();
		if (seen.has(lower)) throw new DOMException(`Duplicate protocol: '${p}'`, "SyntaxError");
		seen.add(lower);
	}
}

/** The bytes of a binary send/ping payload (Blobs are read by the caller). */
function viewOf(data: ArrayBuffer | ArrayBufferView | SharedArrayBuffer): Uint8Array {
	if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
	return new Uint8Array(data);
}

export class WebSocket extends EventTarget {
	static readonly CONNECTING = CONNECTING;
	static readonly OPEN = OPEN;
	static readonly CLOSING = CLOSING;
	static readonly CLOSED = CLOSED;

	#url: string;
	#readyState = CONNECTING;
	#protocol = "";
	#extensions = "";
	#binaryType: BinaryType = "nodebuffer";
	#native: NativeSocket | null = null;
	#handlers: Record<string, Handler> = { open: null, message: null, error: null, close: null };
	#closeTimer: Timeout | null = null;
	#paused = false;
	/** sends waiting for a Blob to be read, in order */
	#sendChain: Promise<void> | null = null;
	#pendingBytes = 0;
	#closedDispatched = false;
	#unref = false;

	constructor(url: string | URL, protocolsOrOptions?: string | string[] | WebSocketOptions) {
		super();
		if (arguments.length === 0) throw new TypeError("WebSocket constructor: 1 argument required, but only 0 present.");
		const parsed = parseURL(url);
		this.#url = parsed.href;
		let options: WebSocketOptions = {};
		let protocols: string[] = [];
		if (typeof protocolsOrOptions === "string") protocols = [protocolsOrOptions];
		else if (Array.isArray(protocolsOrOptions)) protocols = protocolsOrOptions.map(String);
		else if (
			protocolsOrOptions !== undefined &&
			protocolsOrOptions !== null &&
			typeof protocolsOrOptions === "object"
		) {
			options = protocolsOrOptions;
			if (options.protocols !== undefined)
				protocols = typeof options.protocols === "string" ? [options.protocols] : [...options.protocols].map(String);
			else if (options.protocol !== undefined) protocols = [String(options.protocol)];
		}
		checkProtocols(protocols);
		const headers = new Headers();
		if (options.headers !== undefined && options.headers !== null) fillHeaders(headers, options.headers);
		if (protocols.length && !headers.has("sec-websocket-protocol"))
			headers.set("Sec-WebSocket-Protocol", protocols.join(", "));
		const list: string[] = [];
		for (const [name, value] of wireEntries(headers)) list.push(name, value);
		void this.#start(parsed, list, options);
	}

	async #start(url: URL, headers: string[], options: WebSocketOptions): Promise<void> {
		let opts: Record<string, unknown>;
		try {
			const transport = await resolveTransport({ tls: options.tls ?? null, timeout: false });
			const proxy = resolveProxy(url, options.proxy);
			transport.proxy = proxy.proxy;
			transport.proxyHeaders = proxy.headers;
			opts = transportOptions(transport);
		} catch (err) {
			this.#fail(err instanceof Error ? err : new Error(String(err)));
			return;
		}
		if (this.#readyState !== CONNECTING) return;
		opts.url = url.href;
		opts.headers = headers;
		const url_ = this.#url;
		const origin = url.origin;
		const handlers: NativeSocketHandlers = {
			onOpen: (status, list) => {
				if (this.#readyState !== CONNECTING) return;
				for (let i = 0; i + 1 < list.length; i += 2) {
					const name = list[i].toLowerCase();
					if (name === "sec-websocket-protocol") this.#protocol = list[i + 1];
					else if (name === "sec-websocket-extensions") this.#extensions = list[i + 1];
				}
				this.#readyState = OPEN;
				if (this.#paused) this.#native?.pause();
				this.dispatchEvent(new Event("open"));
			},
			onMessage: (data, binary) => {
				if (this.#readyState !== OPEN && this.#readyState !== CLOSING) return;
				this.dispatchEvent(
					new MessageEvent("message", { data: binary ? this.#binary(data as Uint8Array) : data, origin }),
				);
			},
			onPing: (data) => {
				this.dispatchEvent(new MessageEvent("ping", { data: this.#binary(data) }));
			},
			onPong: (data) => {
				this.dispatchEvent(new MessageEvent("pong", { data: this.#binary(data) }));
			},
			onError: (info) => {
				const err = socketError(info, url_);
				this.dispatchEvent(new ErrorEvent("error", { message: err.message, error: err }));
			},
			onClose: (code, reason, wasClean) => {
				this.#native = null;
				this.#closed(code, reason, wasClean);
			},
		};
		try {
			this.#native = connect(opts, handlers);
			if (this.#unref) this.#native.unref();
		} catch (err) {
			this.#fail(err instanceof Error ? err : new Error(String(err)));
		}
	}

	/** The connection failed before it could open: error then close 1006. */
	#fail(err: Error): void {
		if (this.#readyState === CLOSED) return;
		this.#readyState = CLOSED;
		queueMicrotask(() => {
			this.dispatchEvent(new ErrorEvent("error", { message: err.message, error: err }));
			this.#closed(1006, "", false);
		});
	}

	#closed(code: number, reason: string, wasClean: boolean): void {
		clearTimeout(this.#closeTimer ?? undefined);
		this.#closeTimer = null;
		this.#readyState = CLOSED;
		if (this.#closedDispatched) return;
		this.#closedDispatched = true;
		this.dispatchEvent(new CloseEvent("close", { code, reason, wasClean }));
	}

	#binary(data: Uint8Array): unknown {
		switch (this.#binaryType) {
			case "arraybuffer":
				return data.byteOffset === 0 && data.byteLength === data.buffer.byteLength ? data.buffer : data.slice().buffer;
			case "blob":
				return new Blob([data]);
			default:
				return data;
		}
	}

	get url(): string {
		return this.#url;
	}

	get URL(): string {
		return this.#url;
	}

	get readyState(): number {
		return this.#readyState;
	}

	get protocol(): string {
		return this.#protocol;
	}

	get extensions(): string {
		return this.#extensions;
	}

	get bufferedAmount(): number {
		return (this.#native?.bufferedAmount ?? 0) + this.#pendingBytes;
	}

	get binaryType(): BinaryType {
		return this.#binaryType;
	}

	set binaryType(value: BinaryType) {
		if (value === "blob" || value === "arraybuffer" || value === "nodebuffer") this.#binaryType = value;
	}

	get isPaused(): boolean {
		return this.#paused;
	}

	#handler(type: string): Handler {
		return this.#handlers[type] ?? null;
	}

	#setHandler(type: string, fn: Handler): void {
		const had = type in this.#handlers && this.#handlers[type] !== null;
		this.#handlers[type] = typeof fn === "function" ? fn : null;
		if (!had && this.#handlers[type]) this.addEventListener(type, this.#dispatchHandler);
		else if (had && !this.#handlers[type]) this.removeEventListener(type, this.#dispatchHandler);
	}

	#dispatchHandler = (event: Event): void => {
		const fn = this.#handlers[event.type];
		if (fn) fn.call(this, event);
	};

	get onopen(): Handler {
		return this.#handler("open");
	}
	set onopen(fn: Handler) {
		this.#setHandler("open", fn);
	}
	get onmessage(): Handler {
		return this.#handler("message");
	}
	set onmessage(fn: Handler) {
		this.#setHandler("message", fn);
	}
	get onerror(): Handler {
		return this.#handler("error");
	}
	set onerror(fn: Handler) {
		this.#setHandler("error", fn);
	}
	get onclose(): Handler {
		return this.#handler("close");
	}
	set onclose(fn: Handler) {
		this.#setHandler("close", fn);
	}

	#sendNow(data: unknown, kind: "send" | "ping" | "pong"): void {
		const ws = this.#native;
		if (!ws || this.#readyState !== OPEN) return;
		if (kind === "send") {
			if (typeof data === "string") ws.send(data, false);
			else ws.send(viewOf(data as ArrayBufferView), true);
		} else {
			const payload =
				data === undefined ? undefined : typeof data === "string" ? data : viewOf(data as ArrayBufferView);
			if (kind === "ping") ws.ping(payload);
			else ws.pong(payload);
		}
	}

	#queue(data: unknown, kind: "send" | "ping" | "pong"): void {
		if (isBlobLike(data)) {
			const size = data.size;
			this.#pendingBytes += size;
			const read = data.arrayBuffer();
			const prev = this.#sendChain ?? Promise.resolve();
			const next = prev.then(async () => {
				let bytes: ArrayBuffer | null = null;
				try {
					bytes = await read;
				} finally {
					this.#pendingBytes -= size;
				}
				this.#sendNow(bytes, kind);
			});
			this.#sendChain = next.catch((err) => {
				const error = err instanceof Error ? err : new Error(String(err));
				this.dispatchEvent(new ErrorEvent("error", { message: error.message, error }));
			});
			const chain = this.#sendChain;
			void chain.then(() => {
				if (this.#sendChain === chain) this.#sendChain = null;
			});
			return;
		}
		if (
			data !== undefined &&
			typeof data !== "string" &&
			!ArrayBuffer.isView(data) &&
			!(data instanceof ArrayBuffer) &&
			!(typeof SharedArrayBuffer !== "undefined" && data instanceof SharedArrayBuffer)
		) {
			data = String(data);
		}
		if (this.#sendChain) {
			const payload = data;
			this.#sendChain = this.#sendChain.then(() => this.#sendNow(payload, kind));
			return;
		}
		this.#sendNow(data, kind);
	}

	send(data: string | ArrayBufferLike | ArrayBufferView | Blob): void {
		if (arguments.length === 0) throw new TypeError("WebSocket.send: 1 argument required, but only 0 present.");
		if (this.#readyState === CONNECTING)
			throw new DOMException("WebSocket is not open: readyState 0 (CONNECTING)", "InvalidStateError");
		if (this.#readyState !== OPEN) return;
		this.#queue(data, "send");
	}

	ping(data?: string | ArrayBufferLike | ArrayBufferView | Blob): void {
		if (this.#readyState === CONNECTING)
			throw new DOMException("WebSocket is not open: readyState 0 (CONNECTING)", "InvalidStateError");
		if (this.#readyState !== OPEN) return;
		this.#queue(data, "ping");
	}

	pong(data?: string | ArrayBufferLike | ArrayBufferView | Blob): void {
		if (this.#readyState === CONNECTING)
			throw new DOMException("WebSocket is not open: readyState 0 (CONNECTING)", "InvalidStateError");
		if (this.#readyState !== OPEN) return;
		this.#queue(data, "pong");
	}

	close(code?: number, reason?: string): void {
		if (code !== undefined) {
			code = Number(code);
			const allowed =
				(code >= 1000 && code <= 1014 && code !== 1004 && code !== 1005 && code !== 1006) ||
				(code >= 3000 && code <= 4999);
			if (!Number.isInteger(code) || !allowed) {
				throw new DOMException(
					`The close code must be either 1000, or between 3000 and 4999. ${code} is neither.`,
					"InvalidAccessError",
				);
			}
		}
		const text = reason === undefined ? "" : String(reason);
		if (encoder.encode(text).length > 123)
			throw new DOMException("The close reason must not be longer than 123 bytes", "SyntaxError");
		if (this.#readyState === CLOSING || this.#readyState === CLOSED) return;
		if (this.#readyState === CONNECTING) {
			this.#native?.terminate();
			this.#native = null;
			this.#fail(new Error("WebSocket was closed before the connection was established"));
			return;
		}
		this.#readyState = CLOSING;
		const ws = this.#native;
		const finish = (): void => {
			ws?.close(code ?? -1, text);
			this.#closeTimer = setTimeout(() => {
				this.#closeTimer = null;
				this.#native?.terminate();
				this.#native = null;
				this.#closed(1006, "", false);
			}, CLOSE_TIMEOUT);
			this.#closeTimer.unref();
		};
		if (this.#sendChain) void this.#sendChain.then(finish);
		else finish();
	}

	terminate(): void {
		if (this.#readyState === CLOSED) return;
		const wasConnecting = this.#readyState === CONNECTING;
		this.#native?.terminate();
		this.#native = null;
		this.#readyState = CLOSED;
		queueMicrotask(() => {
			if (wasConnecting) {
				const err = new Error("WebSocket was terminated before the connection was established");
				this.dispatchEvent(new ErrorEvent("error", { message: err.message, error: err }));
			}
			this.#closed(1006, "", false);
		});
	}

	pause(): boolean {
		if (this.#readyState === CLOSED || this.#readyState === CLOSING) return false;
		this.#paused = true;
		this.#native?.pause();
		return true;
	}

	resume(): boolean {
		if (this.#readyState === CLOSED || this.#readyState === CLOSING) return false;
		this.#paused = false;
		this.#native?.resume();
		return true;
	}

	/** qbun: keep (or not) the process alive while the socket is open. */
	ref(): void {
		this.#unref = false;
		this.#native?.ref();
	}

	unref(): void {
		this.#unref = true;
		this.#native?.unref();
	}

	get [Symbol.toStringTag](): string {
		return "WebSocket";
	}
}

for (const [name, value] of [
	["CONNECTING", CONNECTING],
	["OPEN", OPEN],
	["CLOSING", CLOSING],
	["CLOSED", CLOSED],
] as const) {
	Object.defineProperty(WebSocket.prototype, name, { value, enumerable: true });
}

export default { WebSocket };
