// http.ClientRequest: the HTTP/1.1 client. Adapted from Node.js
// lib/_http_client.js (MIT).

import { Buffer } from "node:buffer";
import process from "node:process";
import * as net from "node:net";
import { finished } from "node:stream";
import { abortError, codedError, invalidArgType, validateString } from "internal:errors";
import { OutgoingMessage, kOutHeaders, kNeedDrain, kUniqueHeaders, type HeadersInput } from "internal:http/outgoing";
import { IncomingMessage } from "internal:http/incoming";
import { Agent, type AgentRequest, type RequestOptions } from "internal:http/agent";
import {
	HTTPParser,
	RESPONSE,
	NORMAL,
	SKIP_BODY,
	UPGRADE,
	type MessageHead,
	type ParseError,
} from "internal:http/parser";
import {
	checkIsHttpToken,
	connResetException,
	getTimerDuration,
	headersSentError,
	parseUniqueHeadersOption,
	type HeaderValue,
} from "internal:http/common";

const INVALID_PATH_REGEX = /[^\u0021-\u00ff]/;

type ClientSocket = net.Socket & {
	_httpMessage: ClientRequest | null;
	parser: HTTPParser | null;
	_hadError: boolean;
	timeout?: number;
};

type ClientResponse = IncomingMessage & { req: ClientRequest | null };

export interface ClientRequestArgs extends RequestOptions {
	protocol?: string | null;
	hostname?: string | null;
	host?: string | null;
	port?: number | string | null;
	defaultPort?: number | string;
	path?: string | null;
	method?: string;
	headers?: Record<string, HeaderValue> | (string | HeaderValue)[];
	auth?: string | null;
	agent?: Agent | boolean | null;
	_defaultAgent?: Agent;
	timeout?: number;
	setHost?: boolean;
	setDefaultHeaders?: boolean;
	createConnection?: (
		options: RequestOptions,
		cb: (err: Error | null, socket?: net.Socket) => void,
	) => net.Socket | undefined;
	maxHeaderSize?: number;
	insecureHTTPParser?: boolean;
	joinDuplicateHeaders?: boolean;
	signal?: AbortSignal;
	uniqueHeaders?: (string | string[])[];
	highWaterMark?: number;
	socketPath?: string;
	[key: string]: unknown;
}

/** url.urlToHttpOptions() */
export function urlToHttpOptions(url: URL): ClientRequestArgs {
	const { hostname, pathname, port, username, password, search } = url;
	const options: ClientRequestArgs = {
		protocol: url.protocol,
		hostname: hostname && hostname[0] === "[" ? hostname.slice(1, -1) : hostname,
		hash: url.hash,
		search,
		pathname,
		path: `${pathname || ""}${search || ""}`,
		href: url.href,
	};
	if (port !== "") options.port = Number(port);
	if (username || password) options.auth = `${decodeURIComponent(username)}:${decodeURIComponent(password)}`;
	return options;
}

function validateHost(host: unknown, name: string): string | null | undefined {
	if (host !== null && host !== undefined && typeof host !== "string") {
		throw invalidArgType(`options.${name}`, "of type string or one of undefined or null", host);
	}
	return host as string | null | undefined;
}

function emitErrorEvent(request: ClientRequest, error: Error): void {
	request.emit("error", error);
}

function statusIsInformational(status: number): boolean {
	return status < 200 && status >= 100 && status !== 101;
}

export class ClientRequest extends OutgoingMessage implements AgentRequest {
	agent: Agent | undefined;
	method: string;
	host: string;
	protocol: string;
	socketPath: string | undefined;
	timeout: number | undefined = undefined;
	maxHeaderSize: number | undefined;
	insecureHTTPParser: boolean | undefined;
	joinDuplicateHeaders: boolean | undefined;
	_ended = false;
	res: ClientResponse | null = null;
	aborted = false;
	timeoutCb: (() => void) | null = null;
	upgradeOrConnect = false;
	parser: HTTPParser | null = null;
	maxHeadersCount: number | null = null;
	reusedSocket = false;
	#path = "/";
	#error: Error | undefined = undefined;

	constructor(
		input: string | URL | ClientRequestArgs,
		options?: ClientRequestArgs | ((res: IncomingMessage) => void),
		cb?: (res: IncomingMessage) => void,
	) {
		super({ highWaterMark: (typeof options === "object" && options?.highWaterMark) || undefined });

		let base: ClientRequestArgs | null;
		let pathIsFromURL = false;
		if (typeof input === "string") {
			base = urlToHttpOptions(new URL(input));
			pathIsFromURL = true;
		} else if (input instanceof URL) {
			base = urlToHttpOptions(input);
			pathIsFromURL = true;
		} else {
			cb = options as ((res: IncomingMessage) => void) | undefined;
			options = input;
			base = null;
		}
		let opts: ClientRequestArgs;
		if (typeof options === "function") {
			cb = options;
			opts = base || {};
		} else {
			if (pathIsFromURL && options != null && Object.hasOwn(options, "path")) pathIsFromURL = false;
			opts = { ...base, ...options };
		}

		let agent = opts.agent as Agent | false | null | undefined | true;
		const defaultAgent = opts._defaultAgent || Agent.globalAgent;
		if (agent === false) {
			agent = new (defaultAgent.constructor as new () => Agent)();
		} else if (agent === null || agent === undefined) {
			if (typeof opts.createConnection !== "function") agent = defaultAgent;
		} else if (typeof (agent as Agent).addRequest !== "function") {
			throw invalidArgType("options.agent", "of type Agent-like Object, undefined, or false", agent);
		}
		this.agent = (agent || undefined) as Agent | undefined;

		const protocol = opts.protocol || defaultAgent.protocol;
		let expectedProtocol = defaultAgent.protocol;
		if (this.agent?.protocol) expectedProtocol = this.agent.protocol;

		if (opts.path) {
			const path = String(opts.path);
			if (INVALID_PATH_REGEX.test(path))
				throw codedError("ERR_UNESCAPED_CHARACTERS", "Request path contains unescaped characters", TypeError);
		}
		if (protocol !== expectedProtocol) {
			throw codedError(
				"ERR_INVALID_PROTOCOL",
				`Protocol "${protocol}" not supported. Expected "${expectedProtocol}"`,
				TypeError,
			);
		}

		const defaultPort = opts.defaultPort || this.agent?.defaultPort;
		const connectOpts: ClientRequestArgs = { ...opts };
		const port = (connectOpts.port = opts.port || defaultPort || 80);
		const host = (connectOpts.host =
			validateHost(opts.hostname, "hostname") || validateHost(opts.host, "host") || "localhost");
		const setHost = opts.setHost !== undefined ? Boolean(opts.setHost) : opts.setDefaultHeaders !== false;
		this._removedConnection = opts.setDefaultHeaders === false;
		this._removedContLen = opts.setDefaultHeaders === false;
		this._removedTE = opts.setDefaultHeaders === false;
		this.socketPath = opts.socketPath;
		if (opts.timeout !== undefined) this.timeout = getTimerDuration(opts.timeout, "timeout");

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
			delete connectOpts.signal;
		}

		let method = opts.method;
		if (method != null) validateString(method, "options.method");
		if (method) {
			if (!checkIsHttpToken(method))
				throw codedError("ERR_INVALID_HTTP_TOKEN", `Method must be a valid HTTP token ["${method}"]`, TypeError);
			method = method.toUpperCase();
		} else {
			method = "GET";
		}
		this.method = method;

		if (opts.maxHeaderSize !== undefined && (!Number.isInteger(opts.maxHeaderSize) || opts.maxHeaderSize < 0)) {
			throw codedError(
				"ERR_OUT_OF_RANGE",
				`The value of "maxHeaderSize" is out of range. It must be >= 0. Received ${opts.maxHeaderSize}`,
				RangeError,
			);
		}
		this.maxHeaderSize = opts.maxHeaderSize;
		if (opts.insecureHTTPParser !== undefined && typeof opts.insecureHTTPParser !== "boolean") {
			throw invalidArgType("options.insecureHTTPParser", "of type boolean", opts.insecureHTTPParser);
		}
		this.insecureHTTPParser = opts.insecureHTTPParser;
		if (opts.joinDuplicateHeaders !== undefined && typeof opts.joinDuplicateHeaders !== "boolean") {
			throw invalidArgType("options.joinDuplicateHeaders", "of type boolean", opts.joinDuplicateHeaders);
		}
		this.joinDuplicateHeaders = opts.joinDuplicateHeaders;

		let path = opts.path || "/";
		if (method === "CONNECT" && pathIsFromURL && path[0] === "/") path = path.slice(1) || "/";
		this.#path = path;
		if (cb) this.once("response", cb as (...args: unknown[]) => void);

		this.useChunkedEncodingByDefault = !(
			method === "GET" ||
			method === "HEAD" ||
			method === "DELETE" ||
			method === "OPTIONS" ||
			method === "TRACE" ||
			method === "CONNECT"
		);
		this.host = host;
		this.protocol = protocol;

		if (this.agent) {
			if (!this.agent.keepAlive && !Number.isFinite(this.agent.maxSockets)) {
				this._last = true;
				this.shouldKeepAlive = false;
			} else {
				this._last = false;
				this.shouldKeepAlive = true;
			}
		}

		let hostHeader = host;
		const posColon = hostHeader.indexOf(":");
		if (posColon !== -1 && hostHeader.includes(":", posColon + 1) && hostHeader.charCodeAt(0) !== 91) {
			hostHeader = `[${hostHeader}]`;
		}
		if (port && +port !== +(defaultPort ?? 0)) hostHeader += `:${port}`;

		const headers = opts.headers;
		if (!Array.isArray(headers)) {
			if (headers) {
				for (const key in headers) this.setHeader(key, headers[key]);
			}
			if (host && !this.getHeader("host") && setHost) {
				this.setHeader("Host", method === "CONNECT" && opts.path ? String(this.path) : hostHeader);
			}
			if (opts.auth && !this.getHeader("Authorization")) {
				this.setHeader("Authorization", `Basic ${Buffer.from(opts.auth).toString("base64")}`);
			}
			if (this.getHeader("expect")) {
				if (this._header) throw headersSentError("render");
				this._storeHeader(`${this.method} ${this.path} HTTP/1.1\r\n`, this[kOutHeaders]);
			}
		} else {
			this._storeHeader(`${this.method} ${this.path} HTTP/1.1\r\n`, headers as HeadersInput);
		}
		this[kUniqueHeaders] = parseUniqueHeadersOption(opts.uniqueHeaders);

		if (this.agent) {
			this.agent.addRequest(this, connectOpts as RequestOptions);
		} else {
			this._last = true;
			this.shouldKeepAlive = false;
			const copts: ClientRequestArgs = { ...connectOpts };
			if (copts.socketPath) copts.path = copts.socketPath;
			else if (copts.path) copts.path = undefined;
			const createConnection = copts.createConnection;
			if (typeof createConnection === "function") {
				let called = false;
				const oncreate = (err: Error | null, socket?: net.Socket) => {
					if (called) return;
					called = true;
					if (err) process.nextTick(() => emitErrorEvent(this, err));
					else this.onSocket(socket as ClientSocket);
				};
				try {
					const newSocket = createConnection(copts as RequestOptions, oncreate);
					if (newSocket) oncreate(null, newSocket);
				} catch (err) {
					oncreate(err as Error);
				}
			} else {
				this.onSocket(net.createConnection(copts as net.TcpSocketConnectOpts) as ClientSocket);
			}
		}
	}

	get path(): string {
		return this.#path;
	}

	set path(value: string) {
		const path = String(value);
		if (INVALID_PATH_REGEX.test(path))
			throw codedError("ERR_UNESCAPED_CHARACTERS", "Request path contains unescaped characters", TypeError);
		this.#path = path;
	}

	_implicitHeader(): void {
		if (this._header) throw headersSentError("render");
		this._storeHeader(`${this.method} ${this.path} HTTP/1.1\r\n`, this[kOutHeaders]);
	}

	abort(): void {
		if (this.aborted) return;
		this.aborted = true;
		process.nextTick(() => this.emit("abort"));
		this.destroy();
	}

	destroy(err?: Error | null): this {
		if (this.destroyed) return this;
		this.destroyed = true;
		if (this.res) this.res._dump();
		if (this.writableFinished && this.res?.complete) return this;
		this.#error = err ?? undefined;
		this.socket?.destroy(err ?? undefined);
		return this;
	}

	onSocket(socket: net.Socket | null | undefined, err?: Error): void {
		const s = socket as ClientSocket | null | undefined;
		if (s && !err) {
			s._httpMessage = this;
			s.on("error", socketErrorListener);
		}
		process.nextTick(() => this.#onSocketNT(s, err));
	}

	#onSocketNT(socket: ClientSocket | null | undefined, err?: Error): void {
		if (this.destroyed || err) {
			this.destroyed = true;
			const destroyReq = (e?: Error | null) => {
				if (!this.aborted && !e) e = connResetException("socket hang up");
				if (e && !socket?._hadError) emitErrorEvent(this, e);
				this._closed = true;
				this.emit("close");
			};
			if (socket) {
				if (!err && this.agent && !socket.destroyed) {
					socket.emit("free");
					socket.removeListener("error", socketErrorListener);
				} else {
					finished(socket.destroy(err || this.#error), (er?: Error | null) => {
						if ((er as (Error & { code?: string }) | null | undefined)?.code === "ERR_STREAM_PREMATURE_CLOSE")
							er = null;
						destroyReq(er || err);
					});
					return;
				}
			}
			destroyReq(err || this.#error);
			return;
		}
		if (!socket) return;
		this.#tickOnSocket(socket);
		this._flush();
	}

	#tickOnSocket(socket: ClientSocket): void {
		const parser = new HTTPParser(RESPONSE, {
			maxHeaderSize: this.maxHeaderSize || undefined,
			lenient: Boolean(this.insecureHTTPParser),
		});
		if (typeof this.maxHeadersCount === "number") parser.maxHeaderPairs = this.maxHeadersCount;
		this.socket = socket;
		this.parser = parser;
		socket.parser = parser;
		socket._httpMessage = this;
		let incoming: ClientResponse | null = null;

		parser.onHeadersComplete = (info: MessageHead): number => {
			const res = new IncomingMessage(socket) as ClientResponse;
			res._addHead(info.rawHeaders, info.versionMajor, info.versionMinor);
			res.joinDuplicateHeaders = Boolean(this.joinDuplicateHeaders);
			res.statusCode = info.statusCode;
			res.statusMessage = info.statusMessage;
			res.upgrade = info.upgrade;
			res._onRead = () => socket.resume();
			incoming = res;
			return this.#onIncoming(socket, res, info.shouldKeepAlive);
		};
		parser.onBody = (chunk: Buffer) => {
			const stream = incoming;
			if (!stream || stream._dumped) return;
			if (!stream.push(chunk)) socket.pause();
		};
		parser.onMessageComplete = (rawTrailers: string[]) => {
			const stream = incoming;
			if (stream) {
				stream.complete = true;
				if (rawTrailers.length) stream._addTrailers(rawTrailers);
				stream.push(null);
			}
			if (socket.isPaused() && !(stream && stream.readableLength)) socket.resume();
		};

		const onData = (d: Buffer) => {
			const req = socket._httpMessage;
			const ret = parser.execute(d);
			if (typeof ret !== "number") {
				(ret as ParseError).rawPacket = d;
				freeParser();
				socket.removeListener("data", onData);
				socket.removeListener("end", onEnd);
				socket.destroy();
				socket._hadError = true;
				if (req) emitErrorEvent(req, ret);
				return;
			}
			if (parser.upgraded && incoming?.upgrade && req) {
				const res = incoming;
				req.res = res;
				socket.removeListener("data", onData);
				socket.removeListener("end", onEnd);
				socket.removeListener("drain", ondrain);
				if (req.timeoutCb) socket.removeListener("timeout", req.timeoutCb);
				socket.removeListener("timeout", responseOnTimeout);
				freeParser();
				const bodyHead = d.subarray(ret);
				const eventName = req.method === "CONNECT" ? "connect" : "upgrade";
				if (req.listenerCount(eventName) > 0) {
					req.upgradeOrConnect = true;
					socket.emit("agentRemove");
					socket.removeListener("close", socketCloseListener);
					socket.removeListener("error", socketErrorListener);
					socket._httpMessage = null;
					(socket as unknown as { readableFlowing: boolean | null }).readableFlowing = null;
					req.emit(eventName, res, socket, bodyHead);
					req.destroyed = true;
					req._closed = true;
					req.emit("close");
				} else {
					socket.destroy();
				}
			} else if (incoming?.complete && !statusIsInformational(incoming.statusCode ?? 0) && req?.res === incoming) {
				socket.removeListener("data", onData);
				socket.removeListener("end", onEnd);
				socket.removeListener("drain", ondrain);
				freeParser();
			}
		};
		const onEnd = () => {
			const req = socket._httpMessage;
			if (req && !req.res && !socket._hadError) {
				socket._hadError = true;
				emitErrorEvent(req, connResetException("socket hang up"));
			}
			parser.finish();
			freeParser();
			socket.destroy();
		};
		const freeParser = () => {
			parser.close();
			parser.onHeadersComplete = null;
			parser.onBody = null;
			parser.onMessageComplete = null;
			if (socket.parser === parser) socket.parser = null;
			if (this.parser === parser) this.parser = null;
		};

		socket.on("data", onData);
		socket.on("end", onEnd);
		socket.on("close", socketCloseListener);
		socket.on("drain", ondrain);
		if (this.timeout !== undefined || this.agent?.options?.timeout) this.#listenSocketTimeout();
		if (this.timeout !== undefined && !this.agent) setSocketTimeout(socket, this.timeout);
		this.emit("socket", socket);
		// the response body is read by the parser: start the flow
		if (socket.isPaused()) socket.resume();
	}

	#onIncoming(socket: ClientSocket, res: ClientResponse, shouldKeepAlive: boolean): number {
		const req = this;
		if (req.res) {
			socket.destroy();
			return NORMAL;
		}
		req.res = res;
		if (res.upgrade) return UPGRADE;
		const method = req.method;
		if (method === "CONNECT") {
			res.upgrade = true;
			return UPGRADE;
		}
		const status = res.statusCode ?? 0;
		if (statusIsInformational(status)) {
			req.res = null;
			if (status === 100) req.emit("continue");
			req.emit("information", {
				statusCode: res.statusCode,
				statusMessage: res.statusMessage,
				httpVersion: res.httpVersion,
				httpVersionMajor: res.httpVersionMajor,
				httpVersionMinor: res.httpVersionMinor,
				headers: res.headers,
				rawHeaders: res.rawHeaders,
			});
			return SKIP_BODY;
		}
		if (req.shouldKeepAlive && !shouldKeepAlive && !req.upgradeOrConnect) req.shouldKeepAlive = false;
		res.req = req;
		res.on("end", responseOnEnd);
		req.on("finish", requestOnFinish);
		socket.on("timeout", responseOnTimeout);
		if (req.aborted || !req.emit("response", res)) res._dump();
		if (method === "HEAD") return SKIP_BODY;
		if (status === 304) {
			res.complete = true;
			return SKIP_BODY;
		}
		return NORMAL;
	}

	#listenSocketTimeout(): void {
		if (this.timeoutCb) return;
		this.timeoutCb = emitRequestTimeout;
		if (this.socket) this.socket.once("timeout", emitRequestTimeout);
		else this.on("socket", (socket: unknown) => (socket as net.Socket).once("timeout", emitRequestTimeout));
	}

	#deferToConnect(fn: (socket: net.Socket) => void): void {
		const call = () => fn(this.socket as net.Socket);
		const onSocket = () => {
			const socket = this.socket as net.Socket;
			if (socket.writable && !socket.connecting) call();
			else socket.once("connect", call);
		};
		if (!this.socket) this.once("socket", onSocket);
		else onSocket();
	}

	setTimeout(msecs: number, callback?: () => void): this {
		if (this._ended) return this;
		this.#listenSocketTimeout();
		const ms = getTimerDuration(msecs, "msecs");
		if (callback) this.once("timeout", callback);
		if (this.socket) setSocketTimeout(this.socket, ms);
		else this.once("socket", (sock: unknown) => setSocketTimeout(sock as net.Socket, ms));
		return this;
	}

	setNoDelay(noDelay?: boolean): void {
		this.#deferToConnect((s) => s.setNoDelay(noDelay));
	}

	setSocketKeepAlive(enable?: boolean, initialDelay?: number): void {
		this.#deferToConnect((s) => s.setKeepAlive(enable, initialDelay));
	}

	clearTimeout(cb?: () => void): void {
		this.setTimeout(0, cb);
	}

	/** @internal the error the request was destroyed with */
	get _destroyError(): Error | undefined {
		return this.#error;
	}

	/** @internal remember a socket error for an incomplete response */
	_setError(err: Error): void {
		this.#error ??= err;
	}
}

function setSocketTimeout(sock: net.Socket, msecs: number): void {
	if (sock.connecting) sock.once("connect", () => sock.setTimeout(msecs));
	else sock.setTimeout(msecs);
}

function emitRequestTimeout(this: ClientSocket): void {
	this._httpMessage?.emit("timeout");
}

function ondrain(this: ClientSocket): void {
	const msg = this._httpMessage;
	if (msg && !msg.finished && msg[kNeedDrain]) {
		msg[kNeedDrain] = false;
		msg.emit("drain");
	}
}

function socketCloseListener(this: ClientSocket): void {
	const socket = this;
	const req = socket._httpMessage;
	if (!req) return;
	const parser = socket.parser;
	const res = req.res;
	req.destroyed = true;
	if (res) {
		if (!res.complete) {
			const cause = req._destroyError;
			res.destroy(connResetException("aborted", cause));
		}
		req._closed = true;
		req.emit("close");
		if (!res.aborted && res.readable) res.push(null);
	} else {
		if (!socket._hadError) {
			socket._hadError = true;
			emitErrorEvent(req, connResetException("socket hang up"));
		}
		req._closed = true;
		req.emit("close");
	}
	req.outputData.length = 0;
	if (parser) {
		parser.finish();
		parser.close();
		socket.parser = null;
	}
}

function socketErrorListener(this: ClientSocket, err: Error): void {
	const socket = this;
	const req = socket._httpMessage;
	if (req) {
		socket._hadError = true;
		emitErrorEvent(req, err);
		req._setError(err);
	}
	const parser = socket.parser;
	if (parser) {
		parser.finish();
		parser.close();
		socket.parser = null;
	}
	socket.removeAllListeners("data");
	socket.destroy();
}

function responseKeepAlive(req: ClientRequest): void {
	const socket = req.socket as ClientSocket;
	if (req.timeoutCb) {
		socket.setTimeout(0, req.timeoutCb);
		req.timeoutCb = null;
	}
	socket.removeListener("close", socketCloseListener);
	socket.removeListener("error", socketErrorListener);
	process.nextTick(() => {
		req._closed = true;
		req.emit("close");
		if (req.socket) req.socket.emit("free");
	});
	req.destroyed = true;
	if (req.res) req.res.socket = null;
}

function responseOnEnd(this: ClientResponse): void {
	const req = this.req as ClientRequest;
	const socket = req.socket as ClientSocket | null;
	if (socket) {
		if (req.timeoutCb) socket.removeListener("timeout", emitRequestTimeout);
		socket.removeListener("timeout", responseOnTimeout);
	}
	req._ended = true;
	if (!req.shouldKeepAlive) {
		if (socket?.writable) {
			if (typeof socket.destroySoon === "function") socket.destroySoon();
			else socket.end();
		}
	} else if (req.writableFinished && !this.aborted) {
		responseKeepAlive(req);
	}
}

function responseOnTimeout(this: ClientSocket): void {
	const res = this._httpMessage?.res;
	if (res) res.emit("timeout");
}

function requestOnFinish(this: ClientRequest): void {
	if (this.shouldKeepAlive && this._ended && !this.destroyed) responseKeepAlive(this);
}

export default ClientRequest;
