// node:https — HTTP over TLS: the node:http client and server on node:tls.

import * as tls from "node:tls";
import type * as net from "node:net";
import { Agent as HttpAgent, type AgentOptions, type RequestOptions } from "internal:http/agent";
import { ClientRequest, urlToHttpOptions, type ClientRequestArgs } from "internal:http/client";
import type { IncomingMessage } from "internal:http/incoming";
import {
	setupHttpServer,
	httpServerMethods,
	httpCaptureRejection,
	type HttpServer,
	type ServerOptions,
	type ServerResponse,
} from "internal:http/server";
import { captureRejectionSymbol } from "node:events";
import { validateObject } from "internal:errors";

type RequestListener = (req: IncomingMessage, res: ServerResponse) => void;
type ResponseListener = (res: IncomingMessage) => void;

interface TlsServerLike extends net.Server {
	emit(event: string | symbol, ...args: unknown[]): boolean;
}

type TlsServerCtor = new (options: Record<string, unknown>, listener?: (socket: net.Socket) => void) => TlsServerLike;
type TlsConnect = (options: Record<string, unknown>) => net.Socket;

const TlsServer = (tls as unknown as { Server: TlsServerCtor }).Server;
const tlsConnect = (tls as unknown as { connect: TlsConnect }).connect;

// ---------------------------------------------------------------------------
// Server

export class Server extends TlsServer {
	constructor(options?: ServerOptions | RequestListener, requestListener?: RequestListener) {
		let opts: ServerOptions;
		if (typeof options === "function") {
			requestListener = options;
			opts = {};
		} else if (options == null) {
			opts = {};
		} else {
			validateObject(options, "options");
			opts = { ...options };
		}
		if (!opts.ALPNProtocols && !opts.ALPNCallback) opts.ALPNProtocols = ["http/1.1"];
		super({ noDelay: true, ...opts, allowHalfOpen: true });
		setupHttpServer(this as unknown as HttpServer, opts, requestListener, "secureConnection");
		this.on("tlsClientError", (err: unknown, conn: unknown) => {
			if (!this.emit("clientError", err, conn)) (conn as net.Socket).destroy(err as Error);
		});
	}

	close(cb?: (err?: Error) => void): this {
		httpServerMethods.closeIdleConnections.call(this as unknown as HttpServer);
		super.close(cb);
		return this;
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
			const base = TlsServer.prototype as unknown as Record<symbol, ((...a: unknown[]) => void) | undefined>;
			const fn = base[captureRejectionSymbol];
			if (fn) fn.call(this, err, event, ...args);
			else this.emit("error", err);
		}
	}
}

export function createServer(options?: ServerOptions | RequestListener, requestListener?: RequestListener): Server {
	return new Server(options, requestListener);
}

// ---------------------------------------------------------------------------
// Agent

export interface HttpsAgentOptions extends AgentOptions {
	maxCachedSessions?: number;
}

const TLS_NAME_KEYS = [
	"ca",
	"cert",
	"clientCertEngine",
	"ciphers",
	"key",
	"pfx",
	"rejectUnauthorized",
	"servername",
	"minVersion",
	"maxVersion",
	"secureProtocol",
	"crl",
	"honorCipherOrder",
	"ecdhCurve",
	"dhparam",
	"secureOptions",
	"sessionIdContext",
	"sigalgs",
	"privateKeyIdentifier",
	"privateKeyEngine",
] as const;

export class Agent extends HttpAgent {
	maxCachedSessions: number;
	_sessionCache: { map: Record<string, unknown>; list: string[] };

	constructor(options?: HttpsAgentOptions) {
		super(options);
		this.defaultPort = 443;
		this.protocol = "https:";
		const maxCached = this.options.maxCachedSessions;
		this.maxCachedSessions = typeof maxCached === "number" ? maxCached : 100;
		this._sessionCache = { map: {}, list: [] };
	}

	createConnection(options: RequestOptions): net.Socket & { _httpMessage?: null } {
		let opts: Record<string, unknown> = options;
		const key = options._agentKey;
		if (key) {
			const session = this._getSession(key);
			if (session) opts = { session, ...options };
		}
		const socket = tlsConnect(opts);
		if (key) {
			socket.on("session", (session: unknown) => this._cacheSession(key, session));
			socket.once("close", (hadError: unknown) => {
				if (hadError) this._evictSession(key);
			});
		}
		return socket;
	}

	getName(options: RequestOptions = {}): string {
		let name = super.getName(options);
		for (const k of TLS_NAME_KEYS) {
			name += ":";
			const v = options[k];
			if (v !== undefined && v !== null) name += String(v);
		}
		return name;
	}

	_getSession(key: string): unknown {
		return this._sessionCache.map[key];
	}

	_cacheSession(key: string, session: unknown): void {
		if (this.maxCachedSessions === 0) return;
		if (this._sessionCache.map[key]) {
			this._sessionCache.map[key] = session;
			return;
		}
		if (this._sessionCache.list.length >= this.maxCachedSessions) {
			const oldKey = this._sessionCache.list.shift() as string;
			delete this._sessionCache.map[oldKey];
		}
		this._sessionCache.list.push(key);
		this._sessionCache.map[key] = session;
	}

	_evictSession(key: string): void {
		const index = this._sessionCache.list.indexOf(key);
		if (index === -1) return;
		this._sessionCache.list.splice(index, 1);
		delete this._sessionCache.map[key];
	}
}

const defaultAgent = { current: new Agent({ keepAlive: true, scheduling: "lifo", timeout: 5000 }) };
export const globalAgent = defaultAgent.current;

// ---------------------------------------------------------------------------
// client

function buildArgs(args: unknown[]): [ClientRequestArgs, ...unknown[]] {
	let options: ClientRequestArgs = {};
	if (typeof args[0] === "string") options = urlToHttpOptions(new URL(args.shift() as string));
	else if (args[0] instanceof URL) options = urlToHttpOptions(args.shift() as URL);
	if (args[0] && typeof args[0] !== "function") Object.assign(options, args.shift());
	options._defaultAgent = defaultAgent.current;
	return [options, ...args];
}

export function request(
	url: string | URL | ClientRequestArgs,
	options?: ClientRequestArgs | ResponseListener,
	cb?: ResponseListener,
): ClientRequest {
	const [opts, a1] = buildArgs([url, options, cb].filter((a) => a !== undefined));
	return new ClientRequest(opts, a1 as ResponseListener | undefined);
}

export function get(
	url: string | URL | ClientRequestArgs,
	options?: ClientRequestArgs | ResponseListener,
	cb?: ResponseListener,
): ClientRequest {
	const req = request(url, options, cb);
	req.end();
	return req;
}

const https = { Agent, Server, createServer, request, get };

Object.defineProperty(https, "globalAgent", {
	configurable: true,
	enumerable: true,
	get: () => defaultAgent.current,
	set: (agent: Agent) => {
		defaultAgent.current = agent;
	},
});

export default https;
