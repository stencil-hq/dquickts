// The network side of fetch(): a qbun:http request (libcurl) with a
// pull-based body mapped to pause()/resume(), the idle timeout, proxies
// (Bun's HTTP(S)_PROXY / NO_PROXY rules), TLS options and Bun-shaped errors.

import * as core from "qbun:core";
import * as native from "qbun:http";
import * as net from "qbun:net";
import type { Timeout } from "node:timers";
import { chunkBytes, concatBytes } from "internal:fetch/body";
import type { NativeBody } from "internal:fetch/body";
import { fillHeaders, wireEntries } from "internal:fetch/headers";
import type { HeadersInit } from "internal:fetch/headers";

/** The error description made by qbun:http. */
export interface NativeErrorInfo {
	code: string;
	message: string;
	curlCode: number;
	errno?: number;
	syscall?: string;
	status?: number;
	tlsMessage?: string;
}

interface NativeRequest {
	write(data: Uint8Array): void;
	end(): void;
	pause(): void;
	resume(): void;
	abort(): void;
	ref(): void;
	unref(): void;
	hasRef(): boolean;
}

interface NativeHandlers {
	onHeaders(status: number, statusText: string, headers: string[], url: string, version: string): void;
	onData(chunk: Uint8Array): void;
	onPull(): void;
	onEnd(): void;
	onError(info: NativeErrorInfo): void;
}

const request = native.request as (options: Record<string, unknown>, handlers: NativeHandlers) => NativeRequest;

/** Connection settings of a request (Bun's fetch extensions). */
export interface Transport {
	proxy: string;
	proxyHeaders: string[] | null;
	rejectUnauthorized: boolean;
	ca: string | null;
	extraCa: string | null;
	cert: string | null;
	key: string | null;
	passphrase: string | null;
	ciphers: string | null;
	unix: string | null;
	verbose: boolean;
	keepalive: boolean;
	decompress: boolean;
	httpVersion: 1 | 2;
	/** idle timeout in ms, 0 for none */
	timeout: number;
}

export interface HttpHead {
	status: number;
	statusText: string;
	/** flat [name, value, ...] */
	headers: string[];
	url: string;
	version: string;
}

// ------------------------------------------------------------------ errors

const CLOSED_MESSAGE =
	"The socket connection was closed unexpectedly. For more information, pass `verbose: true` in the second argument to fetch()";

interface FetchSystemError extends Error {
	code: string;
	errno?: number;
	syscall?: string;
	hostname?: string;
	path?: string;
}

/** A Bun-shaped fetch error (`code`, `path`, `errno`) with a Node-style `cause`. */
export function fetchError(info: NativeErrorInfo, url: string): Error {
	let host = "";
	let port = "";
	try {
		const u = new URL(url);
		host = u.hostname;
		port = u.port || (u.protocol === "https:" || u.protocol === "wss:" ? "443" : "80");
	} catch {
		// keep the empty host
	}
	const cause = new Error(
		info.syscall === "getaddrinfo"
			? `getaddrinfo ${info.code} ${host}`
			: info.syscall === "connect"
				? `connect ${info.code} ${host}:${port}`
				: `${info.code}: ${info.message}`,
	) as FetchSystemError;
	cause.code = info.code;
	if (info.errno !== undefined) cause.errno = info.errno;
	if (info.syscall) cause.syscall = info.syscall;
	if (host) cause.hostname = host;

	let code = info.code;
	let message: string;
	switch (info.code) {
		case "ECONNREFUSED":
			code = "ConnectionRefused";
			message = "Unable to connect. Is the computer able to access the url?";
			break;
		case "ENOTFOUND":
			code = "FailedToOpenSocket";
			message = "Was there a typo in the url or port?";
			break;
		case "ETIMEDOUT":
			message = "The operation timed out.";
			break;
		case "ECONNRESET":
		case "EPIPE":
		case "ECONNABORTED":
			code = "ECONNRESET";
			message = CLOSED_MESSAGE;
			break;
		case "ERR_TLS_CERT_ALTNAME_INVALID":
			message = `Hostname/IP does not match certificate's altnames: Host: ${host}. is not in the cert's altnames`;
			break;
		case "ZlibError":
			message = `Decompression error: ${info.message}`;
			break;
		default:
			message = info.tlsMessage || info.message;
			break;
	}
	const err = new Error(message, { cause }) as FetchSystemError;
	err.code = code;
	err.path = url;
	err.errno = info.errno ?? 0;
	if (info.syscall) err.syscall = info.syscall;
	return err;
}

export function timeoutError(url: string): Error {
	return fetchError({ code: "ETIMEDOUT", message: "The operation timed out.", curlCode: 28 }, url);
}

// ------------------------------------------------------------------ proxies

function env(name: string): string | undefined {
	const vars = process.env;
	const value = vars[name.toLowerCase()] ?? vars[name.toUpperCase()];
	return value === undefined || value === "" ? undefined : value;
}

function defaultPort(protocol: string): string {
	return protocol === "https:" || protocol === "wss:" ? "443" : "80";
}

/** Whether NO_PROXY (`list`) exempts `url`. */
export function noProxyMatch(url: URL, list: string): boolean {
	const host = url.hostname.toLowerCase().replace(/^\[|\]$/g, "");
	const port = url.port || defaultPort(url.protocol);
	for (const raw of list.split(/[\s,]+/)) {
		let entry = raw.trim().toLowerCase();
		if (!entry) continue;
		if (entry === "*") return true;
		let entryPort = "";
		const v6 = /^\[([^\]]+)\](?::(\d+))?$/.exec(entry);
		if (v6) {
			entry = v6[1];
			entryPort = v6[2] ?? "";
		} else {
			const m = /^([^:]+):(\d+)$/.exec(entry);
			if (m) {
				entry = m[1];
				entryPort = m[2];
			}
		}
		if (entryPort && entryPort !== port) continue;
		if (entry.startsWith("*.")) entry = entry.slice(2);
		else if (entry.startsWith(".")) entry = entry.slice(1);
		if (host === entry || host.endsWith(`.${entry}`)) return true;
	}
	return false;
}

export type ProxyOption =
	| string
	| URL
	| false
	| null
	| undefined
	| { url: string | URL; headers?: HeadersInit; respectNoProxy?: boolean };

/** The proxy of a request ("" = direct) and its headers. */
export function resolveProxy(url: URL, option: ProxyOption): { proxy: string; headers: string[] | null } {
	if (option === false || option === null) return { proxy: "", headers: null };
	let proxy: string | undefined;
	let headers: string[] | null = null;
	let respectNoProxy = true;
	if (typeof option === "string" || option instanceof URL) {
		proxy = String(option);
	} else if (typeof option === "object" && option !== undefined) {
		proxy = String(option.url);
		if (option.headers !== undefined) {
			const h = new Headers();
			fillHeaders(h, option.headers);
			headers = [];
			for (const [name, value] of wireEntries(h)) headers.push(name, value);
		}
		if (option.respectNoProxy === false) respectNoProxy = false;
	} else {
		const secure = url.protocol === "https:" || url.protocol === "wss:";
		proxy = (secure ? env("https_proxy") : undefined) ?? env("http_proxy") ?? env("all_proxy");
	}
	if (!proxy) return { proxy: "", headers: null };
	if (respectNoProxy) {
		const list = env("no_proxy");
		if (list && noProxyMatch(url, list)) return { proxy: "", headers: null };
	}
	return { proxy, headers };
}

// ---------------------------------------------------------------------- TLS

export interface TLSOptions {
	rejectUnauthorized?: boolean;
	ca?: unknown;
	cert?: unknown;
	key?: unknown;
	passphrase?: string;
	ciphers?: string;
	serverName?: string;
	servername?: string;
	checkServerIdentity?: unknown;
}

let extraCaCache: string | null | undefined;

/** NODE_EXTRA_CA_CERTS, read once. */
function extraCa(): string | null {
	if (extraCaCache !== undefined) return extraCaCache;
	const path = process.env.NODE_EXTRA_CA_CERTS;
	extraCaCache = null;
	if (path) {
		try {
			extraCaCache = core.readText(path) as string;
		} catch (err) {
			process.emitWarning(`Ignoring extra certs from \`${path}\`, load failed: ${(err as Error).message}`);
		}
	}
	return extraCaCache;
}

/** PEM text of a TLS option: string, bytes, Blob/BunFile or an array of them. */
async function pemOf(value: unknown): Promise<string | null> {
	if (value === undefined || value === null || value === false) return null;
	if (Array.isArray(value)) {
		const parts: string[] = [];
		for (const item of value) {
			const pem = await pemOf(item);
			if (pem) parts.push(pem);
		}
		return parts.length ? parts.join("\n") : null;
	}
	if (typeof value === "string") return value;
	if (value instanceof ArrayBuffer || ArrayBuffer.isView(value)) {
		const bytes =
			value instanceof ArrayBuffer
				? new Uint8Array(value)
				: new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
		return new TextDecoder().decode(bytes);
	}
	if (typeof value === "object" && "text" in value && typeof value.text === "function") {
		const text: unknown = await value.text();
		return String(text);
	}
	throw new TypeError("TLS option must be a string, Buffer, BunFile or an array of them");
}

export interface TransportInit {
	tls?: TLSOptions | null;
	proxy?: ProxyOption;
	unix?: string;
	verbose?: boolean;
	keepalive?: boolean;
	decompress?: boolean;
	protocol?: string;
	timeout?: number | boolean;
}

function defaultIdleTimeout(): number {
	const raw = process.env.BUN_CONFIG_HTTP_IDLE_TIMEOUT;
	const seconds = raw === undefined ? NaN : Number(raw);
	return Number.isFinite(seconds) && seconds >= 0 ? seconds * 1000 : 300_000;
}

/** The connection settings of a fetch (proxy filled per URL by the caller). */
export async function resolveTransport(init: TransportInit): Promise<Transport> {
	const tls = init.tls ?? {};
	const ca = await pemOf(tls.ca);
	let timeout: number;
	if (init.timeout === undefined || init.timeout === true) timeout = defaultIdleTimeout();
	else if (init.timeout === false) timeout = 0;
	else timeout = Number.isFinite(Number(init.timeout)) && Number(init.timeout) > 0 ? Number(init.timeout) : 0;
	const protocol = init.protocol === undefined ? "" : String(init.protocol);
	return {
		proxy: "",
		proxyHeaders: null,
		rejectUnauthorized: tls.rejectUnauthorized ?? process.env.NODE_TLS_REJECT_UNAUTHORIZED !== "0",
		ca,
		extraCa: ca === null ? extraCa() : null,
		cert: await pemOf(tls.cert),
		key: await pemOf(tls.key),
		passphrase: tls.passphrase === undefined ? null : String(tls.passphrase),
		ciphers: tls.ciphers === undefined ? null : String(tls.ciphers),
		unix: init.unix === undefined || init.unix === null ? null : String(init.unix),
		verbose: !!init.verbose,
		keepalive: init.keepalive !== false,
		decompress: init.decompress !== false,
		httpVersion: protocol === "http1.1" || protocol === "h1" ? 1 : 2,
		timeout,
	};
}

/** The qbun:http options of a transport. */
export function transportOptions(t: Transport): Record<string, unknown> {
	const o: Record<string, unknown> = {
		proxy: t.proxy,
		rejectUnauthorized: t.rejectUnauthorized,
		verbose: t.verbose,
		keepalive: t.keepalive,
	};
	if (t.proxyHeaders) o.proxyHeaders = t.proxyHeaders;
	if (t.ca !== null) o.ca = t.ca;
	if (t.extraCa !== null) o.extraCa = t.extraCa;
	if (t.cert !== null) o.cert = t.cert;
	if (t.key !== null) o.key = t.key;
	if (t.passphrase !== null) o.passphrase = t.passphrase;
	if (t.ciphers !== null) o.ciphers = t.ciphers;
	if (t.unix !== null) o.unix = t.unix;
	return o;
}

// --------------------------------------------------------------------- body

const HIGH_WATER = 64 * 1024;

/** A network body: chunks from qbun:http, pulled by a ReadableStream or collected. */
export class NetBody implements NativeBody {
	#native: NativeRequest | null;
	#chunks: Uint8Array[] = [];
	#queued = 0;
	#state: "open" | "ended" | "errored" | "canceled" = "open";
	#error: unknown = undefined;
	#paused = false;
	#controller: ReadableStreamDefaultController<Uint8Array> | null = null;
	#pull: PromiseWithResolvers<void> | null = null;
	#collect: PromiseWithResolvers<Uint8Array> | null = null;
	#streamed = false;
	#onPause: (paused: boolean) => void;
	#onSettled: (() => void)[] = [];

	constructor(req: NativeRequest, onPause: (paused: boolean) => void) {
		this.#native = req;
		this.#onPause = onPause;
	}

	/** Run `fn` once the body ends, fails or is canceled. */
	whenSettled(fn: () => void): void {
		if (this.#state !== "open") fn();
		else this.#onSettled.push(fn);
	}

	#settle(): void {
		this.#native = null;
		const list = this.#onSettled;
		this.#onSettled = [];
		for (const fn of list) fn();
	}

	#setPaused(paused: boolean): void {
		if (this.#paused === paused || !this.#native) return;
		this.#paused = paused;
		if (paused) this.#native.pause();
		else this.#native.resume();
		this.#onPause(paused);
	}

	push(chunk: Uint8Array): void {
		if (this.#state !== "open") return;
		if (this.#pull && this.#controller) {
			const pull = this.#pull;
			this.#pull = null;
			this.#controller.enqueue(chunk);
			pull.resolve();
			return;
		}
		this.#chunks.push(chunk);
		this.#queued += chunk.length;
		if (!this.#collect && this.#queued >= HIGH_WATER) this.#setPaused(true);
	}

	end(): void {
		if (this.#state !== "open") return;
		this.#state = "ended";
		if (this.#collect) {
			const collect = this.#collect;
			this.#collect = null;
			collect.resolve(concatBytes(this.#chunks, this.#queued));
			this.#chunks = [];
			this.#queued = 0;
		}
		if (this.#pull && this.#controller) {
			const pull = this.#pull;
			this.#pull = null;
			if (!this.#chunks.length) this.#controller.close();
			pull.resolve();
		}
		this.#settle();
	}

	fail(error: unknown): void {
		if (this.#state !== "open") return;
		this.#state = "errored";
		this.#error = error;
		this.#native?.abort();
		this.#chunks = [];
		this.#queued = 0;
		if (this.#collect) {
			const collect = this.#collect;
			this.#collect = null;
			collect.reject(error);
		}
		if (this.#controller) {
			try {
				this.#controller.error(error);
			} catch {
				// the stream is already closed
			}
			const pull = this.#pull;
			this.#pull = null;
			pull?.resolve();
		}
		this.#settle();
	}

	stream(): ReadableStream<Uint8Array> {
		this.#streamed = true;
		return new ReadableStream<Uint8Array>(
			{
				start: (controller) => {
					this.#controller = controller;
					if (this.#state === "errored") controller.error(this.#error);
					else if (this.#state === "ended" && !this.#chunks.length) controller.close();
				},
				pull: (controller) => this.#doPull(controller),
				cancel: (reason) => this.cancel(reason),
			},
			{ highWaterMark: 0 },
		);
	}

	#doPull(controller: ReadableStreamDefaultController<Uint8Array>): Promise<void> | undefined {
		if (this.#chunks.length) {
			const chunk = this.#chunks.shift()!;
			this.#queued -= chunk.length;
			controller.enqueue(chunk);
			if (this.#queued < HIGH_WATER / 2) this.#setPaused(false);
			if (!this.#chunks.length && this.#state === "ended") controller.close();
			return undefined;
		}
		if (this.#state === "ended") {
			controller.close();
			return undefined;
		}
		if (this.#state !== "open") return undefined;
		this.#pull = Promise.withResolvers<void>();
		this.#setPaused(false);
		return this.#pull.promise;
	}

	collect(): Promise<Uint8Array> {
		if (this.#state === "ended") {
			const bytes = concatBytes(this.#chunks, this.#queued);
			this.#chunks = [];
			this.#queued = 0;
			return Promise.resolve(bytes);
		}
		if (this.#state === "errored") return Promise.reject(this.#error);
		if (this.#state === "canceled") return Promise.resolve(new Uint8Array(0));
		this.#collect = Promise.withResolvers<Uint8Array>();
		this.#setPaused(false);
		return this.#collect.promise;
	}

	cancel(reason?: unknown): void {
		if (this.#state !== "open") return;
		this.#state = "canceled";
		this.#error = reason;
		this.#native?.abort();
		this.#chunks = [];
		this.#queued = 0;
		this.#settle();
	}

	/** Cancel when the Response was collected without its body being touched. */
	dropIfUnused(): void {
		if (!this.#streamed && !this.#collect) this.cancel();
	}

	get settled(): boolean {
		return this.#state !== "open";
	}
}

// ------------------------------------------------------------------ request

export interface HttpRequestOptions {
	url: string;
	method: string;
	headers: (string | null)[];
	/** whole body, or a stream uploaded as it is read */
	body: Uint8Array | ReadableStream<Uint8Array> | null;
	bodyLength: number | null;
	transport: Transport;
}

export interface HttpExchange {
	head: Promise<HttpHead>;
	body: NetBody;
	/** Stop the request: `reason` rejects the head or errors the body. */
	abort(reason: unknown): void;
}

/** Start an HTTP request on qbun:http. */
export function startHttp(o: HttpRequestOptions): HttpExchange {
	const head = Promise.withResolvers<HttpHead>();
	const t = o.transport;
	let headDone = false;
	let timer: Timeout | null = null;
	let reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
	let reading = false;
	let finished = false;

	const stopTimer = (): void => {
		clearTimeout(timer ?? undefined);
		timer = null;
	};
	const armTimer = (): void => {
		if (t.timeout <= 0 || finished || timer) return;
		timer = setTimeout(() => {
			timer = null;
			fail(timeoutError(o.url));
		}, t.timeout);
		timer.unref();
	};
	const touch = (): void => {
		timer?.refresh();
	};
	const fail = (err: unknown): void => {
		if (finished) return;
		finished = true;
		stopTimer();
		req.abort();
		if (!headDone) {
			headDone = true;
			head.reject(err);
		}
		body.fail(err);
		reader?.cancel(err).catch(() => {});
		reader = null;
	};

	const options: Record<string, unknown> = transportOptions(t);
	options.url = o.url;
	options.method = o.method;
	options.headers = o.headers;
	options.decompress = t.decompress;
	options.httpVersion = t.httpVersion;
	if (o.body instanceof Uint8Array) {
		options.body = o.body;
	} else if (o.body) {
		options.streaming = true;
		if (o.bodyLength !== null) options.bodyLength = o.bodyLength;
		reader = o.body.getReader();
	}

	const pull = (): void => {
		if (reading || !reader) return;
		reading = true;
		const r = reader;
		const step = (): void => {
			r.read().then(
				({ done, value }) => {
					if (finished) return;
					if (done) {
						reading = false;
						reader = null;
						req.end();
						return;
					}
					let bytes: Uint8Array;
					try {
						bytes = chunkBytes(value);
					} catch (err) {
						fail(err);
						return;
					}
					if (bytes.length === 0) {
						step();
						return;
					}
					reading = false;
					req.write(bytes);
				},
				(err) => fail(err),
			);
		};
		step();
	};

	const handlers: NativeHandlers = {
		onHeaders(status, statusText, headers, url, version) {
			touch();
			if (headDone) return;
			headDone = true;
			head.resolve({ status, statusText, headers, url, version });
		},
		onData(chunk) {
			touch();
			body.push(chunk);
		},
		onPull() {
			touch();
			pull();
		},
		onEnd() {
			if (finished) return;
			finished = true;
			stopTimer();
			if (!headDone) {
				headDone = true;
				head.reject(fetchError({ code: "ECONNRESET", message: CLOSED_MESSAGE, curlCode: 52 }, o.url));
			}
			body.end();
		},
		onError(info) {
			fail(fetchError(info, o.url));
		},
	};

	const req = request(options, handlers);
	const body = new NetBody(req, (paused) => {
		// a consumer applying backpressure is not an idle connection
		if (paused) stopTimer();
		else armTimer();
	});
	body.whenSettled(() => {
		finished = true;
		stopTimer();
		reader?.cancel().catch(() => {});
		reader = null;
	});
	armTimer();
	return {
		head: head.promise,
		body,
		abort: fail,
	};
}

/** Warm the DNS cache for a host (fetch.preconnect). */
export function preresolve(host: string): void {
	net.getaddrinfo?.(host, 0, 0, () => {});
}
