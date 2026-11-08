// http.Agent: connection pooling for the HTTP client.
// Adapted from Node.js lib/_http_agent.js (MIT).

import { EventEmitter } from "node:events";
import * as net from "node:net";
import { invalidArgValue, validateNumber } from "internal:errors";

type Socket = net.Socket & {
	_httpMessage?: AgentRequest | null;
	timeout?: number;
	writable: boolean;
	[kFreeGuard]?: (() => void) | null;
};

/** what the agent needs from a ClientRequest */
export interface AgentRequest {
	timeout?: number;
	reusedSocket: boolean;
	shouldKeepAlive: boolean;
	res?: { headers: Record<string, string | string[] | undefined> } | null;
	onSocket(socket: Socket | undefined | null, err?: Error): void;
	getHeader(name: string): unknown;
}

export interface AgentOptions {
	keepAlive?: boolean;
	keepAliveMsecs?: number;
	maxSockets?: number;
	maxTotalSockets?: number;
	maxFreeSockets?: number;
	scheduling?: "fifo" | "lifo";
	timeout?: number;
	defaultPort?: number;
	protocol?: string;
	noDelay?: boolean;
	agentKeepAliveTimeoutBuffer?: number;
	[key: string]: unknown;
}

export type RequestOptions = Record<string, unknown> & {
	host?: string;
	port?: number | string;
	localAddress?: string;
	family?: number;
	socketPath?: string;
	servername?: string;
	path?: string | null;
	_agentKey?: string;
};

type CreateCallback = (err: Error | null, socket?: Socket) => void;

const kFreeGuard = Symbol("freeSocketGuard");

function freeSocketErrorListener(this: Socket): void {
	this.destroy();
	this.emit("agentRemove");
}

/** data arriving on an idle pooled socket would be taken as the next response: drop the socket */
function installFreeGuard(socket: Socket): void {
	if (socket.readableLength > 0) {
		socket.destroy();
		return;
	}
	const guard = () => socket.destroy();
	socket[kFreeGuard] = guard;
	socket.on("data", guard);
}

function removeFreeGuard(socket: Socket): void {
	const guard = socket[kFreeGuard];
	if (guard) {
		socket.removeListener("data", guard);
		socket[kFreeGuard] = null;
	}
}

function calculateServerName(options: RequestOptions, req: AgentRequest): string {
	let servername = options.host || "";
	const hostHeader = req.getHeader("host");
	if (typeof hostHeader === "string") {
		if (hostHeader.startsWith("[")) {
			const index = hostHeader.indexOf("]");
			servername = index === -1 ? hostHeader : hostHeader.slice(1, index);
		} else {
			servername = hostHeader.split(":", 1)[0];
		}
	}
	if (net.isIP(servername)) servername = "";
	return servername;
}

export class Agent extends EventEmitter {
	static defaultMaxSockets = Infinity;
	/** the agent of requests without one (http.globalAgent) */
	static globalAgent: Agent;

	options: AgentOptions;
	defaultPort: number;
	protocol: string;
	requests: Record<string, AgentRequest[]> = Object.create(null) as Record<string, AgentRequest[]>;
	sockets: Record<string, Socket[]> = Object.create(null) as Record<string, Socket[]>;
	freeSockets: Record<string, Socket[]> = Object.create(null) as Record<string, Socket[]>;
	keepAliveMsecs: number;
	keepAlive: boolean;
	maxSockets: number;
	maxFreeSockets: number;
	maxTotalSockets: number;
	scheduling: "fifo" | "lifo";
	totalSocketCount = 0;
	agentKeepAliveTimeoutBuffer: number;

	constructor(options?: AgentOptions) {
		super();
		this.options = { ...options };
		this.defaultPort = this.options.defaultPort || 80;
		this.protocol = this.options.protocol || "http:";
		if (this.options.noDelay === undefined) this.options.noDelay = true;
		this.options.path = null;
		this.keepAliveMsecs = this.options.keepAliveMsecs || 1000;
		this.keepAlive = this.options.keepAlive || false;
		this.maxSockets = this.options.maxSockets || Agent.defaultMaxSockets;
		this.maxFreeSockets = this.options.maxFreeSockets || 256;
		this.scheduling = this.options.scheduling || "lifo";
		const buffer = this.options.agentKeepAliveTimeoutBuffer;
		this.agentKeepAliveTimeoutBuffer =
			typeof buffer === "number" && buffer >= 0 && Number.isFinite(buffer) ? buffer : 1000;
		if (this.scheduling !== "fifo" && this.scheduling !== "lifo") {
			throw invalidArgValue("scheduling", this.scheduling, "must be one of: 'fifo', 'lifo'");
		}
		if (this.options.maxTotalSockets !== undefined) {
			validateNumber(this.options.maxTotalSockets, "maxTotalSockets", 1);
			this.maxTotalSockets = this.options.maxTotalSockets;
		} else {
			this.maxTotalSockets = Infinity;
		}

		this.on("free", (s: unknown, o: unknown) => this.#onFree(s as Socket, o as RequestOptions));
	}

	#onFree(socket: Socket, options: RequestOptions): void {
		const name = this.getName(options);
		if (!socket.writable) {
			socket.destroy();
			return;
		}
		const requests = this.requests[name];
		if (requests?.length) {
			const req = requests.shift() as AgentRequest;
			this.#setRequestSocket(req, socket);
			if (requests.length === 0) delete this.requests[name];
			return;
		}
		const req = socket._httpMessage;
		if (!req || !req.shouldKeepAlive || !this.keepAlive) {
			socket.destroy();
			return;
		}
		const freeSockets = this.freeSockets[name] || [];
		const freeLen = freeSockets.length;
		let count = freeLen;
		if (this.sockets[name]) count += this.sockets[name].length;
		if (
			this.totalSocketCount > this.maxTotalSockets ||
			count > this.maxSockets ||
			freeLen >= this.maxFreeSockets ||
			!this.keepSocketAlive(socket)
		) {
			socket.destroy();
			return;
		}
		this.freeSockets[name] = freeSockets;
		socket._httpMessage = null;
		this.removeSocket(socket, options);
		socket.once("error", freeSocketErrorListener);
		installFreeGuard(socket);
		freeSockets.push(socket);
	}

	createConnection(options: RequestOptions, _cb?: CreateCallback): Socket | undefined {
		return net.createConnection(options as net.TcpSocketConnectOpts) as Socket;
	}

	getName(options: RequestOptions = {}): string {
		let name = options.host || "localhost";
		name += ":";
		if (options.port) name += options.port;
		name += ":";
		if (options.localAddress) name += options.localAddress;
		if (options.family === 4 || options.family === 6) name += `:${options.family}`;
		if (options.socketPath) name += `:${options.socketPath}`;
		return name;
	}

	#normalize(options: RequestOptions, req: AgentRequest | null): RequestOptions {
		const opts: RequestOptions = { ...options, ...this.options };
		if (opts.socketPath) opts.path = opts.socketPath;
		if (req && !opts.servername && opts.servername !== "") opts.servername = calculateServerName(opts, req);
		return opts;
	}

	addRequest(req: AgentRequest, options: RequestOptions): void {
		const opts = this.#normalize(options, req);
		const name = this.getName(opts);
		if (!this.sockets[name]) this.sockets[name] = [];
		const freeSockets = this.freeSockets[name];
		let socket: Socket | undefined;
		if (freeSockets) {
			while (freeSockets.length && freeSockets[0].destroyed) freeSockets.shift();
			socket = this.scheduling === "fifo" ? freeSockets.shift() : freeSockets.pop();
			if (!freeSockets.length) delete this.freeSockets[name];
		}
		const freeLen = freeSockets ? freeSockets.length : 0;
		const sockLen = freeLen + this.sockets[name].length;
		if (socket) {
			removeFreeGuard(socket);
			socket.removeListener("error", freeSocketErrorListener);
			this.reuseSocket(socket, req);
			this.#setRequestSocket(req, socket);
			this.sockets[name].push(socket);
		} else if (sockLen < this.maxSockets && this.totalSocketCount < this.maxTotalSockets) {
			this.createSocket(req, opts, (err, s) => {
				if (err) req.onSocket(s, err);
				else this.#setRequestSocket(req, s as Socket);
			});
		} else {
			if (!this.requests[name]) this.requests[name] = [];
			this.requests[name].push(req);
		}
	}

	createSocket(req: AgentRequest, options: RequestOptions, cb: CreateCallback): void {
		const opts = this.#normalize(options, req);
		const name = this.getName(opts);
		opts._agentKey = name;
		opts.encoding = null;
		let called = false;
		const oncreate: CreateCallback = (err, s) => {
			if (called) return;
			called = true;
			if (err || !s) {
				cb(err ?? new Error("createConnection returned no socket"));
				return;
			}
			if (!this.sockets[name]) this.sockets[name] = [];
			this.sockets[name].push(s);
			this.totalSocketCount++;
			this.#installListeners(s, opts);
			cb(null, s);
		};
		if (this.keepAlive) {
			opts.keepAlive = this.keepAlive;
			opts.keepAliveInitialDelay = this.keepAliveMsecs;
		}
		let newSocket: Socket | undefined;
		try {
			newSocket = this.createConnection(opts, oncreate);
		} catch (e) {
			oncreate(e as Error);
			return;
		}
		if (newSocket) oncreate(null, newSocket);
	}

	#installListeners(s: Socket, options: RequestOptions): void {
		const onFree = () => this.emit("free", s, options);
		const onClose = () => {
			this.totalSocketCount--;
			this.removeSocket(s, options);
		};
		const onTimeout = () => {
			for (const name in this.freeSockets) {
				if (this.freeSockets[name].includes(s)) {
					s.destroy();
					return;
				}
			}
		};
		const onRemove = () => {
			this.totalSocketCount--;
			this.removeSocket(s, options);
			s.removeListener("close", onClose);
			s.removeListener("free", onFree);
			s.removeListener("timeout", onTimeout);
			s.removeListener("agentRemove", onRemove);
		};
		s.on("free", onFree);
		s.on("close", onClose);
		s.on("timeout", onTimeout);
		s.on("agentRemove", onRemove);
	}

	removeSocket(s: Socket, options: RequestOptions): void {
		const name = this.getName(options);
		const sets: Record<string, Socket[]>[] = [this.sockets];
		if (!s.writable) sets.push(this.freeSockets);
		for (const set of sets) {
			const list = set[name];
			if (!list) continue;
			const index = list.indexOf(s);
			if (index !== -1) {
				list.splice(index, 1);
				if (list.length === 0) delete set[name];
			}
		}
		const pending = this.requests[name];
		if (
			pending?.length &&
			(this.sockets[name]?.length ?? 0) < this.maxSockets &&
			this.totalSocketCount < this.maxTotalSockets
		) {
			const req = pending[0];
			this.createSocket(req, options, (err, socket) => {
				if (err) {
					pending.shift();
					req.onSocket(socket, err);
				} else {
					(socket as Socket).emit("free");
				}
			});
		}
	}

	keepSocketAlive(socket: Socket): boolean {
		socket.setKeepAlive(true, this.keepAliveMsecs);
		socket.unref();
		let agentTimeout = this.options.timeout || 0;
		let canKeepSocketAlive = true;
		const hint = socket._httpMessage?.res?.headers["keep-alive"];
		if (typeof hint === "string") {
			const m = /(?:^|[\s,])timeout=(\d+)/.exec(hint);
			if (m) {
				const serverHintTimeout = parseInt(m[1], 10) * 1000 - this.agentKeepAliveTimeoutBuffer;
				if (serverHintTimeout <= 0) canKeepSocketAlive = false;
				else if (agentTimeout === 0 || serverHintTimeout < agentTimeout) agentTimeout = serverHintTimeout;
			}
		}
		if (socket.timeout !== agentTimeout) socket.setTimeout(agentTimeout);
		return canKeepSocketAlive;
	}

	reuseSocket(socket: Socket, req: AgentRequest): void {
		req.reusedSocket = true;
		socket.ref();
	}

	destroy(): void {
		for (const set of [this.freeSockets, this.sockets]) {
			for (const name in set) for (const s of set[name].slice()) s.destroy();
		}
	}

	#setRequestSocket(req: AgentRequest, socket: Socket): void {
		req.onSocket(socket);
		const agentTimeout = this.options.timeout || 0;
		if (req.timeout === undefined || req.timeout === agentTimeout) return;
		socket.setTimeout(req.timeout);
	}
}

export const globalAgent = new Agent({ keepAlive: true, scheduling: "lifo", timeout: 5000 });
Agent.globalAgent = globalAgent;

export default Agent;
