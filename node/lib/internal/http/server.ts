// http.Server and http.ServerResponse. Adapted from Node.js
// lib/_http_server.js (MIT).
//
// The connection handling is written as functions over a server object so
// that https.Server (a tls.Server) shares it: setupHttpServer() installs the
// options and the 'connection' listener, httpServerMethods holds the methods.

import { Buffer } from "node:buffer";
import process from "node:process";
import * as net from "node:net";
import { captureRejectionSymbol } from "node:events";
import { setInterval as setTimerInterval, clearInterval as clearTimerInterval, Timeout } from "node:timers";
import { codedError, invalidArgType, validateObject } from "internal:errors";
import { OutgoingMessage, kOutHeaders, kNeedDrain, kUniqueHeaders, type HeadersInput } from "internal:http/outgoing";
import { IncomingMessage } from "internal:http/incoming";
import { HTTPParser, REQUEST, NORMAL, UPGRADE, type MessageHead, type ParseError } from "internal:http/parser";
import {
	STATUS_CODES,
	checkInvalidHeaderChar,
	connResetException,
	headersSentError,
	parseUniqueHeadersOption,
	type HeaderValue,
} from "internal:http/common";

const continueExpression = /(?:^|\W)100-continue(?:$|\W)/i;
const chunkExpression = /(?:^|\W)chunked(?:$|\W)/i;

type ServerSocket = net.Socket & {
	_httpMessage: ServerResponse | null;
	parser: HTTPParser | null;
	_paused?: boolean;
	server: HttpServer | null;
	[kConnState]?: ConnectionState;
};

export interface ServerOptions {
	IncomingMessage?: typeof IncomingMessage;
	ServerResponse?: typeof ServerResponse;
	requestTimeout?: number;
	headersTimeout?: number;
	keepAliveTimeout?: number;
	connectionsCheckingInterval?: number;
	maxHeaderSize?: number;
	insecureHTTPParser?: boolean;
	joinDuplicateHeaders?: boolean;
	highWaterMark?: number;
	noDelay?: boolean;
	keepAlive?: boolean;
	keepAliveInitialDelay?: number;
	requireHostHeader?: boolean;
	rejectNonStandardBodyWrites?: boolean;
	uniqueHeaders?: (string | string[])[];
	shouldUpgradeCallback?: (req: IncomingMessage) => boolean;
	[key: string]: unknown;
}

interface ConnectionState {
	socket: ServerSocket;
	server: HttpServer;
	parser: HTTPParser;
	incoming: IncomingMessage[];
	outgoing: ServerResponse[];
	outgoingData: number;
	requestsCount: number;
	keepAliveTimeoutSet: boolean;
	/** start of the current request's head (headersTimeout), 0 when idle */
	headersStart: number;
	/** start of the current request (requestTimeout), 0 when complete */
	requestStart: number;
	current: IncomingMessage | null;
	onData: (d: Buffer) => void;
	onEnd: () => void;
	onClose: () => void;
	onDrain: () => void;
	onPendingData: (delta: number) => void;
}

const kConnState = Symbol("http.connectionState");
const kConnections = Symbol("http.server.connections");
const kCheckInterval = Symbol("http.server.connectionsCheckingInterval");
export const kIncomingMessageClass = Symbol("IncomingMessage");
export const kServerResponseClass = Symbol("ServerResponse");

/** the http-specific state of an http or https server */
export interface HttpServer extends net.Server {
	[kIncomingMessageClass]: typeof IncomingMessage;
	[kServerResponseClass]: typeof ServerResponse;
	[kConnections]: Set<ConnectionState>;
	[kCheckInterval]: Timeout | null;
	[kUniqueHeaders]: Set<string> | null;
	requestTimeout: number;
	headersTimeout: number;
	keepAliveTimeout: number;
	keepAliveTimeoutBuffer: number;
	connectionsCheckingInterval: number;
	maxHeaderSize: number | undefined;
	insecureHTTPParser: boolean | undefined;
	joinDuplicateHeaders: boolean | undefined;
	requireHostHeader: boolean;
	rejectNonStandardBodyWrites: boolean;
	shouldUpgradeCallback: (req: IncomingMessage) => boolean;
	httpAllowHalfOpen: boolean;
	timeout: number;
	maxHeadersCount: number | null;
	maxRequestsPerSocket: number;
}

// ---------------------------------------------------------------------------
// ServerResponse

function onServerResponseClose(this: ServerSocket): void {
	const res = this._httpMessage;
	if (res) emitCloseNT(res);
}

function emitCloseNT(res: OutgoingMessage): void {
	if (!res._closed) {
		res.destroyed = true;
		res._closed = true;
		res.emit("close");
	}
}

export class ServerResponse extends OutgoingMessage {
	req: IncomingMessage;
	statusCode = 200;
	statusMessage: string | undefined = undefined;
	_sent100 = false;
	_expect_continue = false;
	[kConnState]?: ConnectionState;

	constructor(req: IncomingMessage, options?: { highWaterMark?: number; rejectNonStandardBodyWrites?: boolean }) {
		super(options);
		if (req.method === "HEAD") this._hasBody = false;
		this.req = req;
		this.sendDate = true;
		this.rejectNonStandardBodyWrites = Boolean(options?.rejectNonStandardBodyWrites);
		if ((req.httpVersionMajor ?? 1) < 1 || (req.httpVersionMinor ?? 1) < 1) {
			const te = req.headers.te;
			this.useChunkedEncodingByDefault = chunkExpression.test(typeof te === "string" ? te : "");
			this.shouldKeepAlive = false;
		}
	}

	assignSocket(socket: net.Socket): void {
		const s = socket as ServerSocket;
		if (s._httpMessage) throw codedError("ERR_HTTP_SOCKET_ASSIGNED", "ServerResponse has an already assigned socket");
		s._httpMessage = this;
		s.on("close", onServerResponseClose);
		this.socket = s;
		this.emit("socket", s);
		this._flush();
	}

	detachSocket(socket: net.Socket): void {
		const s = socket as ServerSocket;
		s.removeListener("close", onServerResponseClose);
		s._httpMessage = null;
		this.socket = null;
	}

	writeInformation(
		statusCode: number,
		statusMessage: string,
		headers: Record<string, HeaderValue> | null,
		cb?: () => void,
	): void {
		let head = `HTTP/1.1 ${statusCode} ${statusMessage}\r\n`;
		if (headers) {
			for (const name in headers) {
				const value = headers[name];
				const list = Array.isArray(value) ? value : [value];
				for (const v of list) {
					if (checkInvalidHeaderChar(v))
						throw codedError("ERR_INVALID_CHAR", `Invalid character in header content ["${name}"]`, TypeError);
					head += `${name}: ${v}\r\n`;
				}
			}
		}
		this._writeRaw(`${head}\r\n`, "ascii", cb ?? null);
	}

	writeContinue(cb?: () => void): void {
		this._writeRaw("HTTP/1.1 100 Continue\r\n\r\n", "ascii", cb ?? null);
		this._sent100 = true;
	}

	writeProcessing(cb?: () => void): void {
		this._writeRaw("HTTP/1.1 102 Processing\r\n\r\n", "ascii", cb ?? null);
	}

	writeEarlyHints(hints: Record<string, HeaderValue>, cb?: () => void): void {
		validateObject(hints, "hints");
		const link = hints.link;
		if (link === undefined || link === null)
			throw codedError("ERR_INVALID_ARG_VALUE", "The argument 'hints.link' is invalid.", TypeError);
		let head = "HTTP/1.1 103 Early Hints\r\n";
		const links = Array.isArray(link) ? link : [String(link)];
		for (const l of links) {
			if (checkInvalidHeaderChar(l))
				throw codedError("ERR_INVALID_CHAR", 'Invalid character in header content ["Link"]', TypeError);
		}
		head += `Link: ${links.join(", ")}\r\n`;
		for (const key in hints) {
			if (key === "link") continue;
			head += `${key}: ${String(hints[key])}\r\n`;
		}
		this._writeRaw(`${head}\r\n`, "ascii", cb ?? null);
	}

	_implicitHeader(): void {
		this.writeHead(this.statusCode);
	}

	writeHead(statusCode: number, reason?: string | HeadersInput, obj?: HeadersInput): this {
		if (this._header) throw headersSentError("write");
		const original = statusCode;
		statusCode |= 0;
		if (statusCode < 100 || statusCode > 999) {
			throw codedError("ERR_HTTP_INVALID_STATUS_CODE", `Invalid status code: ${String(original)}`, RangeError);
		}
		if (typeof reason === "string") {
			this.statusMessage = reason;
		} else {
			this.statusMessage ||= STATUS_CODES[statusCode] || "unknown";
			obj ??= reason;
		}
		this.statusCode = statusCode;

		let headers: HeadersInput | null | undefined;
		if (this[kOutHeaders]) {
			if (Array.isArray(obj)) {
				if (obj.length % 2 !== 0)
					throw codedError("ERR_INVALID_ARG_VALUE", "The argument 'headers' is invalid.", TypeError);
				for (let n = 0; n < obj.length; n += 2) this.removeHeader(obj[n] as string);
				for (let n = 0; n < obj.length; n += 2) {
					const k = obj[n] as string;
					if (k) this.appendHeader(k, obj[n + 1] as HeaderValue);
				}
			} else if (obj) {
				const o = obj as Record<string, HeaderValue>;
				for (const k in o) if (k) this.setHeader(k, o[k]);
			}
			headers = this[kOutHeaders];
		} else {
			headers = obj;
		}
		if (this.statusMessage !== STATUS_CODES[statusCode] && checkInvalidHeaderChar(this.statusMessage)) {
			throw codedError("ERR_INVALID_CHAR", "Invalid character in statusMessage", TypeError);
		}
		const statusLine = `HTTP/1.1 ${statusCode} ${this.statusMessage}\r\n`;
		if (statusCode === 204 || statusCode === 304 || (statusCode >= 100 && statusCode <= 199)) this._hasBody = false;
		if (this._expect_continue && !this._sent100) this.shouldKeepAlive = false;
		this._storeHeader(statusLine, headers ?? null);
		return this;
	}

	writeHeader(statusCode: number, reason?: string | HeadersInput, obj?: HeadersInput): this {
		return this.writeHead(statusCode, reason, obj);
	}
}

// ---------------------------------------------------------------------------
// server setup

/** install the http options/state on a net.Server, or a tls.Server with connectionEvent "secureConnection" */
export function setupHttpServer(
	server: HttpServer,
	options: ServerOptions,
	requestListener?: (req: IncomingMessage, res: ServerResponse) => void,
	connectionEvent = "connection",
): void {
	server[kIncomingMessageClass] = options.IncomingMessage || IncomingMessage;
	server[kServerResponseClass] = options.ServerResponse || ServerResponse;
	const num = (v: unknown, name: string, def: number): number => {
		if (v === undefined) return def;
		if (typeof v !== "number" || !Number.isInteger(v) || v < 0) {
			throw codedError(
				"ERR_OUT_OF_RANGE",
				`The value of "${name}" is out of range. It must be >= 0. Received ${String(v)}`,
				RangeError,
			);
		}
		return v;
	};
	server.maxHeaderSize =
		options.maxHeaderSize === undefined ? undefined : num(options.maxHeaderSize, "maxHeaderSize", 0);
	if (options.insecureHTTPParser !== undefined && typeof options.insecureHTTPParser !== "boolean") {
		throw invalidArgType("options.insecureHTTPParser", "of type boolean", options.insecureHTTPParser);
	}
	server.insecureHTTPParser = options.insecureHTTPParser;
	server.requestTimeout = num(options.requestTimeout, "requestTimeout", 300000);
	server.headersTimeout = num(options.headersTimeout, "headersTimeout", 60000);
	if (server.requestTimeout > 0 && server.headersTimeout > 0 && server.headersTimeout > server.requestTimeout) {
		throw codedError(
			"ERR_OUT_OF_RANGE",
			`The value of "headersTimeout" is out of range. It must be <= requestTimeout. Received ${server.headersTimeout}`,
			RangeError,
		);
	}
	server.keepAliveTimeout = num(options.keepAliveTimeout, "keepAliveTimeout", 5000);
	server.keepAliveTimeoutBuffer = 1000;
	server.connectionsCheckingInterval = num(options.connectionsCheckingInterval, "connectionsCheckingInterval", 30000);
	server.joinDuplicateHeaders = options.joinDuplicateHeaders;
	server.requireHostHeader = options.requireHostHeader === undefined ? true : Boolean(options.requireHostHeader);
	server.rejectNonStandardBodyWrites = Boolean(options.rejectNonStandardBodyWrites);
	server.shouldUpgradeCallback =
		typeof options.shouldUpgradeCallback === "function"
			? options.shouldUpgradeCallback
			: () => server.listenerCount("upgrade") > 0;
	server.httpAllowHalfOpen = false;
	server.timeout = 0;
	server.maxHeadersCount = null;
	server.maxRequestsPerSocket = 0;
	server[kUniqueHeaders] = parseUniqueHeadersOption(options.uniqueHeaders);
	server[kConnections] = new Set();
	server[kCheckInterval] = null;
	if (requestListener) server.on("request", requestListener as (...args: unknown[]) => void);
	server.on(connectionEvent, (socket: unknown) => connectionListener(server, socket as ServerSocket));
	server.on("listening", () => setupConnectionsTracking(server));
	server.on("close", () => {
		if (server[kCheckInterval]) {
			clearTimerInterval(server[kCheckInterval]);
			server[kCheckInterval] = null;
		}
	});
}

function setupConnectionsTracking(server: HttpServer): void {
	if (server[kCheckInterval]) clearTimerInterval(server[kCheckInterval]);
	server[kCheckInterval] = setTimerInterval(
		() => checkConnections(server),
		server.connectionsCheckingInterval || 30000,
	).unref();
}

function checkConnections(server: HttpServer): void {
	const headersTimeout =
		Number.isFinite(server.headersTimeout) && server.headersTimeout >= 0 ? server.headersTimeout : 0;
	const requestTimeout =
		Number.isFinite(server.requestTimeout) && server.requestTimeout >= 0 ? server.requestTimeout : 0;
	if (headersTimeout === 0 && requestTimeout === 0) return;
	const now = Date.now();
	for (const state of server[kConnections]) {
		const headersExpired = headersTimeout > 0 && state.headersStart > 0 && now - state.headersStart > headersTimeout;
		const requestExpired = requestTimeout > 0 && state.requestStart > 0 && now - state.requestStart > requestTimeout;
		if (headersExpired || requestExpired) {
			state.headersStart = 0;
			state.requestStart = 0;
			socketOnError.call(state.socket, codedError("ERR_HTTP_REQUEST_TIMEOUT", "Request timeout"));
		}
	}
}

/** the methods of http.Server, shared with https.Server */
export const httpServerMethods = {
	closeAllConnections(this: HttpServer): void {
		const set = this[kConnections];
		if (!set) return;
		for (const state of [...set]) state.socket.destroy();
	},

	closeIdleConnections(this: HttpServer): void {
		const set = this[kConnections];
		if (!set) return;
		for (const state of [...set]) {
			if (state.incoming.length > 0 || state.requestStart > 0 || state.headersStart > 0) continue;
			const msg = state.socket._httpMessage;
			if (msg && !msg.finished) continue;
			state.socket.destroy();
		}
	},

	setTimeout(this: HttpServer, msecs: number, callback?: () => void): HttpServer {
		this.timeout = msecs;
		if (callback) this.on("timeout", callback);
		return this;
	},

	[Symbol.asyncDispose](this: HttpServer): Promise<void> {
		const { promise, resolve, reject } = Promise.withResolvers<void>();
		this.close((err) => (err ? reject(err) : resolve()));
		return promise;
	},
};

/** the 'request' handler rejected: answer 500 like Node's captureRejections */
export function httpCaptureRejection(event: string, args: unknown[]): boolean {
	if (event !== "request") return false;
	const res = args[1] as ServerResponse;
	if (!res.headersSent && !res.writableEnded) {
		for (const name of res.getHeaderNames()) res.removeHeader(name);
		res.statusCode = 500;
		res.end(STATUS_CODES[500]);
	} else {
		res.destroy();
	}
	return true;
}

export class Server extends net.Server {
	constructor(
		options?: ServerOptions | ((req: IncomingMessage, res: ServerResponse) => void),
		requestListener?: (req: IncomingMessage, res: ServerResponse) => void,
	) {
		let opts: ServerOptions;
		if (typeof options === "function") {
			requestListener = options;
			opts = {};
		} else if (options == null) {
			opts = {};
		} else {
			validateObject(options, "options");
			opts = options;
		}
		super({
			allowHalfOpen: true,
			noDelay: opts.noDelay ?? true,
			keepAlive: opts.keepAlive,
			keepAliveInitialDelay: opts.keepAliveInitialDelay,
			highWaterMark: opts.highWaterMark,
		});
		setupHttpServer(this as unknown as HttpServer, opts, requestListener);
	}

	close(cb?: (err?: Error) => void): this {
		httpServerMethods.closeIdleConnections.call(this as unknown as HttpServer);
		return super.close(cb);
	}

	closeAllConnections(): void {
		httpServerMethods.closeAllConnections.call(this as unknown as HttpServer);
	}

	closeIdleConnections(): void {
		httpServerMethods.closeIdleConnections.call(this as unknown as HttpServer);
	}

	setTimeout(msecs: number, callback?: () => void): this {
		httpServerMethods.setTimeout.call(this as unknown as HttpServer, msecs, callback);
		return this;
	}

	[Symbol.asyncDispose](): Promise<void> {
		return httpServerMethods[Symbol.asyncDispose].call(this as unknown as HttpServer);
	}

	[captureRejectionSymbol](err: Error, event: string, ...args: unknown[]): void {
		if (!httpCaptureRejection(event, args)) {
			(net.Server.prototype as unknown as Record<symbol, (...a: unknown[]) => void>)[captureRejectionSymbol].call(
				this,
				err,
				event,
				...args,
			);
		}
	}
}

// ---------------------------------------------------------------------------
// connections

function noop(): void {}

function socketOnError(this: ServerSocket, e: Error & { code?: string }): void {
	this.removeListener("error", socketOnError);
	if (this.listenerCount("error") === 0) this.on("error", noop);
	const server = this.server;
	if (!server || !server.emit("clientError", e, this)) {
		if (this.writable && (!this._httpMessage || !this._httpMessage._headerSent)) {
			let response: string;
			switch (e.code) {
				case "HPE_HEADER_OVERFLOW":
					response = "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n";
					break;
				case "ERR_HTTP_REQUEST_TIMEOUT":
					response = "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\n\r\n";
					break;
				default:
					response = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
			}
			this.write(response);
		}
		this.destroy(e);
	}
}

function socketOnTimeout(this: ServerSocket): void {
	const state = this[kConnState];
	const req = state?.current;
	const reqTimeout = req && !req.complete && req.emit("timeout", this);
	const res = this._httpMessage;
	const resTimeout = res && res.emit("timeout", this);
	const serverTimeout = this.server?.emit("timeout", this);
	if (!reqTimeout && !resTimeout && !serverTimeout) this.destroy();
}

function connectionListener(server: HttpServer, socket: ServerSocket): void {
	socket.server = server;
	socket._httpMessage = null;
	if (server.timeout && typeof socket.setTimeout === "function") socket.setTimeout(server.timeout);
	socket.on("timeout", socketOnTimeout);

	const parser = new HTTPParser(REQUEST, {
		maxHeaderSize: server.maxHeaderSize || undefined,
		lenient: Boolean(server.insecureHTTPParser),
	});
	if (typeof server.maxHeadersCount === "number") parser.maxHeaderPairs = server.maxHeadersCount;
	socket.parser = parser;

	const state: ConnectionState = {
		socket,
		server,
		parser,
		incoming: [],
		outgoing: [],
		outgoingData: 0,
		requestsCount: 0,
		keepAliveTimeoutSet: false,
		headersStart: 0,
		requestStart: 0,
		current: null,
		onData: noop,
		onEnd: noop,
		onClose: noop,
		onDrain: noop,
		onPendingData: noop,
	};
	socket[kConnState] = state;
	server[kConnections].add(state);

	state.onPendingData = (delta: number) => {
		state.outgoingData += delta;
		socketOnDrain(socket, state);
	};
	state.onData = (d: Buffer) => {
		if (state.headersStart === 0 && state.requestStart === 0) state.headersStart = Date.now();
		const ret = parser.execute(d);
		onParserExecute(server, socket, parser, state, ret, d);
	};
	state.onEnd = () => {
		const ret = parser.finish();
		if (ret) {
			socketOnError.call(socket, ret);
		} else if (!server.httpAllowHalfOpen) {
			socket.end();
		} else if (state.outgoing.length) {
			state.outgoing[state.outgoing.length - 1]._last = true;
		} else if (socket._httpMessage) {
			socket._httpMessage._last = true;
		} else {
			socket.end();
		}
	};
	state.onClose = () => {
		server[kConnections].delete(state);
		parser.close();
		socket.parser = null;
		while (state.incoming.length) (state.incoming.shift() as IncomingMessage).destroy(connResetException("aborted"));
		while (state.outgoing.length) (state.outgoing.shift() as ServerResponse).destroy(connResetException("aborted"));
	};
	state.onDrain = () => socketOnDrain(socket, state);

	parser.onHeadersComplete = (info: MessageHead): number => {
		const IncomingCtor = server[kIncomingMessageClass];
		const req = new IncomingCtor(socket);
		req._addHead(info.rawHeaders, info.versionMajor, info.versionMinor);
		req.joinDuplicateHeaders = Boolean(server.joinDuplicateHeaders);
		req.method = info.method;
		req.url = info.url ?? "";
		req.upgrade = info.upgrade;
		req._onRead = () => {
			if (!socket._paused) socket.resume();
		};
		state.current = req;
		state.headersStart = 0;
		state.requestStart = Date.now();
		return parserOnIncoming(server, socket, state, req, info.shouldKeepAlive);
	};
	parser.onBody = (chunk: Buffer) => {
		const req = state.current;
		if (!req || req._dumped) return;
		if (!req.push(chunk)) socket.pause();
	};
	parser.onMessageComplete = (rawTrailers: string[]) => {
		const req = state.current;
		state.requestStart = 0;
		if (req) {
			req.complete = true;
			if (rawTrailers.length) req._addTrailers(rawTrailers);
			req.push(null);
		}
		if (!socket._paused && socket.isPaused()) socket.resume();
	};

	socket.on("data", state.onData);
	socket.on("error", socketOnError);
	socket.on("end", state.onEnd);
	socket.on("close", state.onClose);
	socket.on("drain", state.onDrain);
	socket.setEncoding = () => {
		throw codedError("ERR_HTTP_SOCKET_ENCODING", "Changing the socket encoding is not allowed per RFC7230 Section 3.");
	};
	socket._paused = false;
}

function socketOnDrain(socket: ServerSocket, state: ConnectionState): void {
	const needPause = state.outgoingData > socket.writableHighWaterMark;
	if (socket._paused && !needPause) {
		socket._paused = false;
		socket.resume();
	}
	const msg = socket._httpMessage;
	if (msg && !msg.finished && msg[kNeedDrain] && msg.writableLength === 0) {
		msg[kNeedDrain] = false;
		msg.emit("drain");
	}
}

function onParserExecute(
	server: HttpServer,
	socket: ServerSocket,
	parser: HTTPParser,
	state: ConnectionState,
	ret: number | ParseError,
	d: Buffer,
): void {
	if (typeof ret !== "number") {
		ret.rawPacket = d;
		socketOnError.call(socket, ret);
		return;
	}
	const req = state.current;
	if (parser.upgraded && req?.upgrade) {
		const eventName = req.method === "CONNECT" ? "connect" : "upgrade";
		socket.removeListener("data", state.onData);
		socket.removeListener("end", state.onEnd);
		socket.removeListener("close", state.onClose);
		socket.removeListener("drain", state.onDrain);
		socket.removeListener("error", socketOnError);
		socket.removeListener("timeout", socketOnTimeout);
		server[kConnections].delete(state);
		parser.close();
		socket.parser = null;
		delete (socket as Partial<ServerSocket>).setEncoding;
		if (server.listenerCount(eventName) > 0) {
			const bodyHead = d.subarray(ret);
			(socket as unknown as { readableFlowing: boolean | null }).readableFlowing = null;
			server.emit(eventName, req, socket, bodyHead);
		} else {
			socket.destroy();
		}
	} else if (req && req.method === "PRI") {
		socket.destroy();
	}
}

function parserOnIncoming(
	server: HttpServer,
	socket: ServerSocket,
	state: ConnectionState,
	req: IncomingMessage,
	keepAlive: boolean,
): number {
	if (state.keepAliveTimeoutSet) {
		socket.setTimeout(server.timeout || 0);
		state.keepAliveTimeoutSet = false;
	}
	if (req.upgrade) {
		req.upgrade = req.method === "CONNECT" || Boolean(server.shouldUpgradeCallback(req));
		if (req.upgrade) return UPGRADE;
	}
	state.incoming.push(req);

	if (!socket._paused) {
		if (socket.writableNeedDrain || state.outgoingData >= socket.writableHighWaterMark) {
			socket._paused = true;
			socket.pause();
		}
	}

	const ResponseCtor = server[kServerResponseClass];
	const res = new ResponseCtor(req, {
		highWaterMark: socket.writableHighWaterMark,
		rejectNonStandardBodyWrites: server.rejectNonStandardBodyWrites,
	});
	res._keepAliveTimeout = server.keepAliveTimeout;
	res._maxRequestsPerSocket = server.maxRequestsPerSocket;
	res._onPendingData = state.onPendingData;
	res[kConnState] = state;
	res.shouldKeepAlive = keepAlive;
	res[kUniqueHeaders] = server[kUniqueHeaders];

	if (socket._httpMessage) state.outgoing.push(res);
	else res.assignSocket(socket);

	res.on("finish", () => resOnFinish(req, res, socket, state, server));

	let handled = false;
	if (req.httpVersionMajor === 1 && req.httpVersionMinor === 1) {
		if (server.requireHostHeader && req.headers.host === undefined) {
			res.writeHead(400, ["Connection", "close"]);
			res.end();
			return NORMAL;
		}
		const limit = typeof server.maxRequestsPerSocket === "number" && server.maxRequestsPerSocket > 0;
		if (limit) {
			state.requestsCount++;
			res.maxRequestsOnConnectionReached = server.maxRequestsPerSocket <= state.requestsCount;
		}
		if (limit && server.maxRequestsPerSocket < state.requestsCount) {
			handled = true;
			server.emit("dropRequest", req, socket);
			res.writeHead(503);
			res.end();
		} else {
			const expect = req.headers.expect;
			if (typeof expect === "string") {
				handled = true;
				if (continueExpression.test(expect)) {
					res._expect_continue = true;
					if (server.listenerCount("checkContinue") > 0) {
						server.emit("checkContinue", req, res);
					} else {
						res.writeContinue();
						server.emit("request", req, res);
					}
				} else if (server.listenerCount("checkExpectation") > 0) {
					server.emit("checkExpectation", req, res);
				} else {
					res.writeHead(417);
					res.end();
				}
			}
		}
	}
	if (!handled) server.emit("request", req, res);
	return NORMAL;
}

function resOnFinish(
	req: IncomingMessage,
	res: ServerResponse,
	socket: ServerSocket,
	state: ConnectionState,
	server: HttpServer,
): void {
	const index = state.incoming.indexOf(req);
	if (index !== -1) state.incoming.splice(index, 1);
	const rs = (req as unknown as { _readableState?: { resumeScheduled?: boolean } })._readableState;
	if (!req._consuming && !rs?.resumeScheduled) req._dump();
	res.detachSocket(socket);
	if (state.current === req && req.readableEnded) state.current = null;
	process.nextTick(() => emitCloseNT(res));
	if (res._last) {
		if (typeof socket.destroySoon === "function") socket.destroySoon();
		else socket.end();
	} else if (state.outgoing.length === 0) {
		const keepAliveTimeout =
			Number.isFinite(server.keepAliveTimeout) && server.keepAliveTimeout >= 0 ? server.keepAliveTimeout : 0;
		if (keepAliveTimeout && typeof socket.setTimeout === "function") {
			socket.setTimeout(keepAliveTimeout + server.keepAliveTimeoutBuffer);
			state.keepAliveTimeoutSet = true;
		}
	} else {
		const m = state.outgoing.shift();
		if (m) m.assignSocket(socket);
	}
}

export function createServer(
	options?: ServerOptions | ((req: IncomingMessage, res: ServerResponse) => void),
	requestListener?: (req: IncomingMessage, res: ServerResponse) => void,
): Server {
	return new Server(options, requestListener);
}

export default Server;
