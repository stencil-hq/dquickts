// Bun.serve(): an HTTP/1.1 (or HTTPS) server calling a fetch(Request)
// handler and writing the returned web Response (streamed), with Bun's
// `routes`, WebSocket upgrades (internal:http/websocket-server), idle
// timeouts and the Server object API.

import { Server as HttpsServer } from "node:https";
import { CookieMap } from "internal:bun/misc";
import { Readable } from "node:stream";
import * as net from "node:net";
import { kListenSync } from "node:net";
import { Server as HttpServer, type ServerOptions } from "internal:http/server";
import type { ServerResponse } from "internal:http/server";
import type { IncomingMessage } from "internal:http/incoming";
import { STATUS_CODES } from "internal:http/common";
import {
	ServerWebSocket,
	TopicRegistry,
	acceptKey,
	type WebSocketHandler,
	type WsData,
} from "internal:http/websocket-server";
import { bunTlsOptions } from "internal:http/bun-socket";
import { codedError, invalidArgType, validateObject } from "internal:errors";

type Handler = (this: BunServer, req: Request, server: BunServer) => unknown;
type ErrorHandler = (this: BunServer, error: Error) => unknown;
type MethodMap = Partial<Record<string, Response | Handler>>;
type RouteValue = Response | Handler | MethodMap | false | null | undefined;

export interface ServeOptions {
	port?: number | string;
	hostname?: string;
	unix?: string;
	fetch?: Handler;
	error?: ErrorHandler;
	routes?: Record<string, RouteValue>;
	static?: Record<string, RouteValue>;
	websocket?: WebSocketHandler;
	tls?: Record<string, unknown> | Record<string, unknown>[] | boolean;
	idleTimeout?: number;
	maxRequestBodySize?: number;
	development?: boolean | { hmr?: boolean; console?: boolean };
	id?: string | null;
	reusePort?: boolean;
	ipv6Only?: boolean;
	exclusive?: boolean;
	[key: string]: unknown;
}

interface RequestContext {
	server: BunServer;
	socket: net.Socket;
	req: IncomingMessage;
	res: ServerResponse | null;
	upgrade: { data: unknown; headers: unknown } | null;
	responded: boolean;
}

interface CompiledRoute {
	segments: string[];
	paramCount: number;
	staticCount: number;
	wildcard: boolean;
	value: RouteValue;
}

interface HttpServerInternals extends HttpServer {
	keepAliveTimeout: number;
	keepAliveTimeoutBuffer: number;
	timeout: number;
	requestTimeout: number;
	headersTimeout: number;
}

const METHODS_WITHOUT_BODY = new Set(["GET", "HEAD", "OPTIONS", "TRACE", "CONNECT"]);

/** the connection a Request built by Bun.serve came from */
const kContext = Symbol("bun.serve.context");
type ServedRequest = Request & { [kContext]?: RequestContext };

function toError(e: unknown): Error {
	return e instanceof Error ? e : new Error(String(e));
}

function compileRoutes(routes: Record<string, RouteValue> | undefined): {
	exact: Map<string, RouteValue>;
	dynamic: CompiledRoute[];
} {
	const exact = new Map<string, RouteValue>();
	const dynamic: CompiledRoute[] = [];
	if (!routes) return { exact, dynamic };
	validateObject(routes, "routes");
	for (const path in routes) {
		const value = routes[path];
		if (!path.startsWith("/"))
			throw codedError("ERR_INVALID_ARG_VALUE", `Route path must start with '/': ${path}`, TypeError);
		if (
			value !== false &&
			value !== null &&
			value !== undefined &&
			typeof value !== "function" &&
			!(value instanceof Response) &&
			typeof value !== "object"
		) {
			throw invalidArgType(`routes["${path}"]`, "a Response, a function or an object of methods", value);
		}
		const segments = path.split("/").slice(1);
		const wildcard = segments[segments.length - 1] === "*";
		const paramCount = segments.filter((s) => s.startsWith(":")).length;
		if (!wildcard && paramCount === 0) {
			exact.set(path, value);
			continue;
		}
		dynamic.push({
			segments,
			paramCount,
			staticCount: segments.length - paramCount - (wildcard ? 1 : 0),
			wildcard,
			value,
		});
	}
	// most specific first: no wildcard, more static segments, more segments
	dynamic.sort(
		(a, b) =>
			Number(a.wildcard) - Number(b.wildcard) || b.staticCount - a.staticCount || b.segments.length - a.segments.length,
	);
	return { exact, dynamic };
}

function matchRoute(route: CompiledRoute, parts: string[]): Record<string, string> | null {
	const segs = route.segments;
	if (route.wildcard) {
		if (parts.length < segs.length - 1) return null;
	} else if (parts.length !== segs.length) return null;
	const params: Record<string, string> = {};
	const n = route.wildcard ? segs.length - 1 : segs.length;
	for (let i = 0; i < n; i++) {
		const s = segs[i];
		if (s.startsWith(":")) {
			if (!parts[i]) return null;
			try {
				params[s.slice(1)] = decodeURIComponent(parts[i]);
			} catch {
				params[s.slice(1)] = parts[i];
			}
		} else if (s !== parts[i]) return null;
	}
	return params;
}

function waitDrainOrClose(res: ServerResponse): Promise<void> {
	const { promise, resolve } = Promise.withResolvers<void>();
	const done = () => {
		res.removeListener("drain", done);
		res.removeListener("close", done);
		resolve();
	};
	res.on("drain", done);
	res.on("close", done);
	return promise;
}

/** the read result if it settles before the next macrotask, else undefined */
function settledSoon<T>(p: Promise<T>): Promise<{ value: T } | undefined> {
	const { promise, resolve, reject } = Promise.withResolvers<{ value: T } | undefined>();
	p.then((value) => resolve({ value }), reject);
	setImmediate(() => resolve(undefined));
	return promise;
}

function asBytes(chunk: unknown): Uint8Array {
	if (chunk instanceof Uint8Array) return chunk;
	if (typeof chunk === "string") return Buffer.from(chunk, "utf8");
	if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk);
	if (ArrayBuffer.isView(chunk)) return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
	throw new TypeError("Response body stream must produce Uint8Array chunks");
}

function defaultPort(): number {
	const env = process.env;
	const p = env.BUN_PORT || env.PORT || env.NODE_PORT;
	const n = p ? parseInt(p, 10) : NaN;
	return Number.isInteger(n) && n >= 0 && n < 65536 ? n : 3000;
}

export class BunServer {
	#http: HttpServerInternals;
	#fetch: Handler | undefined;
	#error: ErrorHandler | undefined;
	#routes: { exact: Map<string, RouteValue>; dynamic: CompiledRoute[] };
	#websocket: WebSocketHandler | undefined;
	#topics = new TopicRegistry();
	#websockets = new Set<ServerWebSocket>();
	#pending = 0;
	#stopping = false;
	#closed: Promise<void> | null = null;
	#protocol: "http" | "https";
	#hostname: string | undefined;
	#port: number | undefined;
	#unix: string | undefined;
	#idleMs: number;
	#maxBody: number;
	readonly development: boolean;
	readonly id: string;

	constructor(options: ServeOptions) {
		validateObject(options, "options");
		this.#fetch = options.fetch;
		this.#error = options.error;
		this.#websocket = options.websocket;
		this.#routes = compileRoutes(options.routes ?? options.static);
		if (this.#fetch !== undefined && typeof this.#fetch !== "function")
			throw invalidArgType("fetch", "a function", this.#fetch);
		if (!this.#fetch && this.#routes.exact.size === 0 && this.#routes.dynamic.length === 0) {
			throw codedError(
				"ERR_INVALID_ARG_TYPE",
				'Bun.serve() needs either:\n\n  - A routes object:\n     routes: {\n       "/path": {\n         GET: (req) => new Response("Hello")\n       }\n     }\n\n  - Or a fetch handler:\n     fetch: (req) => {\n       return new Response("Hello")\n     }\n\nLearn more at https://bun.com/docs/api/http',
				TypeError,
			);
		}
		const idle = options.idleTimeout ?? 10;
		if (typeof idle !== "number" || idle < 0 || idle > 255) {
			throw codedError(
				"ERR_INVALID_ARG_VALUE",
				"Bun.serve expects idleTimeout to be an integer between 0 and 255 seconds",
				TypeError,
			);
		}
		this.#idleMs = idle * 1000;
		this.#maxBody = typeof options.maxRequestBodySize === "number" ? options.maxRequestBodySize : 128 * 1024 * 1024;
		const dev = options.development;
		this.development = dev === undefined ? process.env.NODE_ENV !== "production" : Boolean(dev);
		this.id = typeof options.id === "string" ? options.id : "";

		const tlsConfig = Array.isArray(options.tls) ? options.tls[0] : options.tls;
		this.#protocol = tlsConfig ? "https" : "http";
		const httpOpts: ServerOptions = {
			requestTimeout: 0,
			headersTimeout: 0,
			keepAliveTimeout: this.#idleMs,
			requireHostHeader: false,
			shouldUpgradeCallback: (req: IncomingMessage) => {
				const up = req.headers.upgrade;
				return Boolean(this.#websocket) && typeof up === "string" && up.toLowerCase() === "websocket";
			},
		};
		let srv: HttpServerInternals;
		if (tlsConfig) {
			srv = new HttpsServer({ ...bunTlsOptions(tlsConfig), ...httpOpts });
		} else {
			srv = new HttpServer(httpOpts) as HttpServerInternals;
		}
		srv.keepAliveTimeoutBuffer = 0;
		srv.timeout = this.#idleMs;
		srv.on("request", (req: unknown, res: unknown) => {
			void this.#onRequest(req as IncomingMessage, res as ServerResponse);
		});
		srv.on("upgrade", (req: unknown, socket: unknown, head: unknown) => {
			void this.#onUpgrade(req as IncomingMessage, socket as net.Socket, head as Buffer);
		});
		srv.on("clientError", (_err: unknown, socket: unknown) => {
			const s = socket as net.Socket;
			if (s.writable) s.end("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
			else s.destroy();
		});
		this.#http = srv;
		this.#listen(options);
	}

	#listen(options: ServeOptions): void {
		const srv = this.#http;
		const listen = (srv as unknown as Record<symbol, (o: Record<string, unknown>) => void>)[kListenSync];
		if (options.unix !== undefined && options.unix !== "") {
			this.#unix = String(options.unix);
			try {
				listen.call(srv, { path: this.#unix });
			} catch (e) {
				throw this.#listenError(e);
			}
			return;
		}
		const portValue = options.port ?? defaultPort();
		const port = typeof portValue === "string" ? parseInt(portValue, 10) : portValue;
		if (!Number.isInteger(port) || port < 0 || port > 65535) {
			throw codedError("ERR_INVALID_ARG_VALUE", `Invalid port: ${String(portValue)}`, RangeError);
		}
		const hostname = options.hostname || "0.0.0.0";
		this.#hostname = hostname;
		this.#port = port;
		const opts = {
			port,
			host: hostname,
			exclusive: options.reusePort === false || Boolean(options.exclusive),
			ipv6Only: Boolean(options.ipv6Only),
		};
		if (net.isIP(hostname) || hostname === "localhost") {
			try {
				listen.call(srv, opts);
			} catch (e) {
				throw this.#listenError(e);
			}
			const addr = srv.address();
			if (addr && typeof addr === "object") this.#port = addr.port;
		} else {
			// a hostname to resolve: listen asynchronously
			srv.once("listening", () => {
				const addr = srv.address();
				if (addr && typeof addr === "object") this.#port = addr.port;
			});
			srv.on("error", (e: unknown) => {
				process.nextTick(() => {
					throw this.#listenError(e);
				});
			});
			srv.listen(opts);
		}
	}

	#listenError(e: unknown): Error {
		const err = e as Error & { code?: string; errno?: number };
		if (err?.code === "EADDRINUSE") {
			const what = this.#unix !== undefined ? `unix socket ${this.#unix}` : `port ${this.#port}`;
			const ex = new Error(`Failed to start server. Is ${what} in use?`) as Error & {
				code?: string;
				syscall?: string;
				errno?: number;
			};
			ex.code = "EADDRINUSE";
			ex.syscall = "listen";
			ex.errno = err.errno ?? 0;
			return ex;
		}
		return toError(e);
	}

	// -- requests ------------------------------------------------------------

	#buildRequest(req: IncomingMessage, signal: AbortSignal, withBody: boolean): Request {
		const hostHeader = req.headers.host;
		let url = req.url || "/";
		if (!/^[a-z][a-z0-9+.-]*:\/\//i.test(url)) {
			const host =
				typeof hostHeader === "string" && hostHeader
					? hostHeader
					: this.#unix !== undefined
						? "localhost"
						: `${this.#urlHost()}:${this.#port}`;
			url = `${this.#protocol}://${host}${url.startsWith("/") ? "" : "/"}${url}`;
		}
		const headers = new Headers();
		const raw = req.rawHeaders;
		for (let i = 0; i < raw.length; i += 2) headers.append(raw[i], raw[i + 1]);
		const method = req.method ?? "GET";
		const init: RequestInit & { duplex?: string } = { method, headers, signal };
		const hasBody =
			withBody &&
			!METHODS_WITHOUT_BODY.has(method) &&
			(req.headers["transfer-encoding"] !== undefined || Number(req.headers["content-length"] ?? 0) > 0);
		if (hasBody) {
			init.body = Readable.toWeb(req) as unknown as ReadableStream<Uint8Array>;
			init.duplex = "half";
		} else {
			req.resume();
		}
		let request: Request;
		try {
			request = new Request(url, init);
		} catch {
			request = new Request(`${this.#protocol}://localhost${req.url?.startsWith("/") ? req.url : "/"}`, init);
		}
		return request;
	}

	#withExtras(request: Request, params: Record<string, string>): Request {
		Object.defineProperty(request, "params", { value: params, configurable: true, enumerable: true, writable: true });
		if (!("cookies" in request)) {
			let cookies: unknown;
			Object.defineProperty(request, "cookies", {
				configurable: true,
				enumerable: false,
				get: () => {
					if (cookies === undefined) {
						cookies = new CookieMap(request.headers.get("cookie") ?? "");
					}
					return cookies;
				},
			});
		}
		return request;
	}

	/** routes, then fetch; the handler's result */
	#dispatch(request: Request): unknown {
		const url = new URL(request.url);
		const path = url.pathname;
		let value: RouteValue;
		let params: Record<string, string> = {};
		let found = false;
		if (this.#routes.exact.has(path)) {
			value = this.#routes.exact.get(path);
			found = value !== false && value !== null && value !== undefined;
		}
		if (!found && this.#routes.dynamic.length) {
			const parts = path.split("/").slice(1);
			for (const route of this.#routes.dynamic) {
				const m = matchRoute(route, parts);
				if (m && route.value !== false && route.value !== null && route.value !== undefined) {
					value = route.value;
					params = m;
					found = true;
					break;
				}
			}
		}
		if (found) {
			let target: Response | Handler | undefined;
			if (value instanceof Response || typeof value === "function") target = value;
			else if (value && typeof value === "object") {
				const methods = value as MethodMap;
				target = methods[request.method] ?? (request.method === "HEAD" ? methods.GET : undefined);
			}
			if (target instanceof Response) return target.clone();
			if (typeof target === "function") return target.call(this, this.#withExtras(request, params), this);
		}
		if (this.#fetch) return this.#fetch.call(this, this.#withExtras(request, params), this);
		return new Response("Not Found", { status: 404 });
	}

	async #handle(request: Request): Promise<Response | undefined> {
		let result: unknown;
		try {
			result = await this.#dispatch(request);
		} catch (e) {
			return this.#handleError(toError(e));
		}
		if (result instanceof Response) return result;
		if (result === undefined && this.#context(request)?.upgrade) return undefined;
		return this.#handleError(new TypeError(`Expected a Response object, but received '${String(result)}'`));
	}

	async #handleError(err: Error): Promise<Response> {
		if (this.#error) {
			try {
				const r = await this.#error.call(this, err);
				if (r instanceof Response) return r;
			} catch (e) {
				console.error(e);
				return new Response("Internal Server Error", { status: 500 });
			}
		}
		console.error(err);
		return new Response(this.development ? `${err.stack ?? err.message}` : "Internal Server Error", {
			status: 500,
			headers: { "content-type": "text/plain;charset=utf-8" },
		});
	}

	async #onRequest(req: IncomingMessage, res: ServerResponse): Promise<void> {
		this.#pending++;
		const ac = new AbortController();
		const onClose = () => {
			if (!res.writableFinished) ac.abort(new DOMException("The connection was closed.", "AbortError"));
		};
		res.on("close", onClose);
		try {
			if (this.#stopping) res.shouldKeepAlive = false;
			const len = Number(req.headers["content-length"] ?? 0);
			if (len > this.#maxBody) {
				res.writeHead(413, { connection: "close" });
				res.end();
				return;
			}
			const socket = req.socket as net.Socket;
			const request = this.#buildRequest(req, ac.signal, true);
			const ctx: RequestContext = { server: this, socket, req, res, upgrade: null, responded: false };
			(request as ServedRequest)[kContext] = ctx;
			const response = await this.#handle(request);
			ctx.responded = true;
			await this.#writeResponse(response ?? new Response(null, { status: 500 }), res, req.method ?? "GET");
		} catch (e) {
			if (!res.headersSent) {
				res.statusCode = 500;
				res.end();
			} else {
				res.destroy(toError(e));
			}
		} finally {
			this.#pending--;
		}
	}

	async #writeResponse(response: Response, res: ServerResponse, method: string): Promise<void> {
		const status = response.status;
		res.statusCode = status;
		res.statusMessage = response.statusText || STATUS_CODES[status] || "unknown";
		const cookies = typeof response.headers.getSetCookie === "function" ? response.headers.getSetCookie() : [];
		for (const [name, value] of response.headers) {
			if (name === "set-cookie") continue;
			res.setHeader(name, value);
		}
		if (cookies.length) res.setHeader("set-cookie", cookies);
		const body = response.body;
		const noBody = method === "HEAD" || status === 204 || status === 304 || (status >= 100 && status < 200);
		if (!body || noBody) {
			if (body) void body.cancel().catch(() => {});
			if (!body && !noBody && !res.hasHeader("content-length")) res.setHeader("content-length", "0");
			res.end();
			return;
		}
		const reader = body.getReader();
		const abort = () => {
			void reader.cancel().catch(() => {});
		};
		res.once("close", abort);
		try {
			// a read that did not settle soon stays pending: the loop awaits it
			// instead of issuing another read, which would lose its chunk
			let pending: Promise<ReadableStreamReadResult<unknown>> | undefined = reader.read();
			const first = await settledSoon(pending);
			if (first !== undefined) {
				pending = undefined;
				if (first.value.done) {
					if (!res.hasHeader("content-length")) res.setHeader("content-length", "0");
					res.end();
					return;
				}
				const chunk = asBytes(first.value.value);
				pending = reader.read();
				const second = await settledSoon(pending);
				if (second !== undefined && second.value.done) {
					if (!res.hasHeader("content-length") && !res.hasHeader("transfer-encoding"))
						res.setHeader("content-length", String(chunk.byteLength));
					res.end(chunk);
					return;
				}
				if (!res.write(chunk)) await waitDrainOrClose(res);
				if (second !== undefined) {
					pending = undefined;
					if (res.destroyed) return;
					if (!res.write(asBytes(second.value.value))) await waitDrainOrClose(res);
				}
			} else {
				// a slow stream: send the head now
				res.flushHeaders();
			}
			for (;;) {
				if (res.destroyed) return;
				const next = pending ?? reader.read();
				pending = undefined;
				const { done, value } = await next;
				if (done) break;
				if (res.destroyed) return;
				if (!res.write(asBytes(value))) await waitDrainOrClose(res);
			}
			res.end();
		} catch (e) {
			res.destroy(toError(e));
		} finally {
			res.removeListener("close", abort);
		}
	}

	// -- upgrades ------------------------------------------------------------

	async #onUpgrade(req: IncomingMessage, socket: net.Socket, head: Buffer): Promise<void> {
		this.#pending++;
		const ac = new AbortController();
		socket.once("close", () => ac.abort(new DOMException("The connection was closed.", "AbortError")));
		try {
			const request = this.#buildRequest(req, ac.signal, false);
			const ctx: RequestContext = { server: this, socket, req, res: null, upgrade: null, responded: false };
			(request as ServedRequest)[kContext] = ctx;
			const response = await this.#handle(request);
			ctx.responded = true;
			if (ctx.upgrade) {
				this.#completeUpgrade(req, socket, head, ctx.upgrade);
				return;
			}
			await this.#writeRawResponse(socket, response ?? new Response(null, { status: 500 }), req.method ?? "GET");
		} catch (e) {
			socket.destroy(toError(e));
		} finally {
			this.#pending--;
		}
	}

	async #writeRawResponse(socket: net.Socket, response: Response, method: string): Promise<void> {
		const status = response.status;
		const reason = response.statusText || STATUS_CODES[status] || "unknown";
		const body = method === "HEAD" ? new Uint8Array(0) : new Uint8Array(await response.arrayBuffer());
		let head = `HTTP/1.1 ${status} ${reason}\r\n`;
		for (const [name, value] of response.headers) {
			if (name === "set-cookie" || name === "content-length" || name === "connection" || name === "transfer-encoding")
				continue;
			head += `${name}: ${value}\r\n`;
		}
		if (typeof response.headers.getSetCookie === "function")
			for (const c of response.headers.getSetCookie()) head += `set-cookie: ${c}\r\n`;
		head += `Date: ${new Date().toUTCString()}\r\nContent-Length: ${body.byteLength}\r\nConnection: close\r\n\r\n`;
		if (socket.destroyed) return;
		socket.write(head, "latin1");
		socket.end(body);
	}

	#completeUpgrade(
		req: IncomingMessage,
		socket: net.Socket,
		head: Buffer,
		upgrade: { data: unknown; headers: unknown },
	): void {
		const handler = this.#websocket as WebSocketHandler;
		if (socket.destroyed) return;
		const key = String(req.headers["sec-websocket-key"]);
		const lines = [
			"HTTP/1.1 101 Switching Protocols",
			"Upgrade: websocket",
			"Connection: Upgrade",
			`Sec-WebSocket-Accept: ${acceptKey(key)}`,
		];
		let hasProtocol = false;
		if (upgrade.headers !== undefined && upgrade.headers !== null) {
			const extra = new Headers(upgrade.headers as HeadersInit);
			for (const [name, value] of extra) {
				if (name === "upgrade" || name === "connection" || name === "sec-websocket-accept") continue;
				if (name === "sec-websocket-protocol") hasProtocol = true;
				lines.push(`${name}: ${value}`);
			}
		}
		const requested = req.headers["sec-websocket-protocol"];
		if (!hasProtocol && typeof requested === "string" && requested) {
			lines.push(`Sec-WebSocket-Protocol: ${requested.split(",")[0].trim()}`);
		}
		socket.write(`${lines.join("\r\n")}\r\n\r\n`, "latin1");
		const ws = new ServerWebSocket(socket, handler, this.#topics, upgrade.data, (w) => this.#websockets.delete(w));
		this.#websockets.add(ws);
		ws._open();
		ws._push(head);
	}

	upgrade(request: Request, options?: { headers?: HeadersInit; data?: unknown }): boolean {
		if (!this.#websocket) {
			throw new TypeError('To enable websocket support, set the "websocket" object in Bun.serve({})');
		}
		if (!(request instanceof Request)) throw invalidArgType("request", "an instance of Request", request);
		const ctx = this.#context(request);
		if (!ctx || ctx.res !== null || ctx.upgrade || ctx.responded) return false;
		const h = ctx.req.headers;
		const up = h.upgrade;
		if (typeof up !== "string" || up.toLowerCase() !== "websocket") return false;
		if (typeof h["sec-websocket-key"] !== "string" || h["sec-websocket-version"] !== "13") return false;
		ctx.upgrade = { data: options?.data, headers: options?.headers };
		return true;
	}

	// -- Server API ----------------------------------------------------------

	#urlHost(): string {
		const h = this.#hostname ?? "localhost";
		if (h === "0.0.0.0" || h === "::" || h === "") return "localhost";
		return net.isIPv6(h) ? `[${h}]` : h;
	}

	get port(): number | undefined {
		return this.#unix !== undefined ? undefined : this.#port;
	}

	get hostname(): string | undefined {
		return this.#unix !== undefined ? undefined : this.#hostname;
	}

	get url(): URL {
		if (this.#unix !== undefined) return new URL(`unix://${this.#unix}`);
		return new URL(`${this.#protocol}://${this.#urlHost()}:${this.#port}/`);
	}

	get protocol(): "http" | "https" {
		return this.#protocol;
	}

	get pendingRequests(): number {
		return this.#pending;
	}

	get pendingWebSockets(): number {
		return this.#websockets.size;
	}

	fetch(request: Request | string): Response | Promise<Response> {
		const req = typeof request === "string" ? new Request(request) : request;
		return this.#handle(req).then((r) => r ?? new Response(null, { status: 500 }));
	}

	#context(request: Request): RequestContext | undefined {
		const ctx = (request as ServedRequest)[kContext];
		return ctx?.server === this ? ctx : undefined;
	}

	requestIP(request: Request): { address: string; family: "IPv4" | "IPv6"; port: number } | null {
		const ctx = this.#context(request);
		if (!ctx || this.#unix !== undefined) return null;
		const s = ctx.socket;
		if (!s.remoteAddress) return null;
		return { address: s.remoteAddress, family: (s.remoteFamily ?? "IPv4") as "IPv4" | "IPv6", port: s.remotePort ?? 0 };
	}

	timeout(request: Request, seconds: number): void {
		const ctx = this.#context(request);
		if (!ctx) return;
		ctx.socket.setTimeout(Math.max(0, seconds) * 1000);
	}

	publish(topic: string, data: WsData, _compress?: boolean): number {
		return this.#topics.publish(String(topic), data, null);
	}

	subscriberCount(topic: string): number {
		return this.#topics.count(String(topic));
	}

	closeIdleConnections(): number {
		this.#http.closeIdleConnections();
		return 0;
	}

	stop(closeActiveConnections?: boolean): Promise<void> {
		if (!this.#closed) {
			this.#stopping = true;
			const { promise, resolve } = Promise.withResolvers<void>();
			this.#closed = promise;
			this.#http.close(() => resolve());
		}
		if (closeActiveConnections) {
			for (const ws of this.#websockets) ws.terminate();
			this.#http.closeAllConnections();
		}
		return this.#closed;
	}

	reload(options: ServeOptions): this {
		validateObject(options, "options");
		if (options.fetch !== undefined) this.#fetch = options.fetch;
		if (options.error !== undefined) this.#error = options.error;
		if (options.routes !== undefined || options.static !== undefined)
			this.#routes = compileRoutes(options.routes ?? options.static);
		if (options.websocket !== undefined) {
			this.#websocket = options.websocket;
			for (const ws of this.#websockets) ws._setHandler(options.websocket);
		}
		return this;
	}

	ref(): void {
		this.#http.ref();
	}

	unref(): void {
		this.#http.unref();
	}

	[Symbol.dispose](): void {
		void this.stop(true);
	}

	[Symbol.asyncDispose](): Promise<void> {
		return this.stop(true);
	}
}

export function serve(options: ServeOptions): BunServer {
	return new BunServer(options);
}
