// http.IncomingMessage: the request (server) or response (client) being
// received. Adapted from Node.js lib/_http_incoming.js (MIT).

import { Readable } from "node:stream";
import process from "node:process";
import type { Socket } from "node:net";

type HeaderMap = Record<string, string | string[] | undefined>;

/** headers whose duplicates are dropped (Node's kFirstWins fields) */
const FIRST_WINS: Record<string, true> = {
	age: true,
	host: true,
	from: true,
	etag: true,
	server: true,
	referer: true,
	expires: true,
	location: true,
	"user-agent": true,
	"retry-after": true,
	"content-type": true,
	"max-forwards": true,
	authorization: true,
	"last-modified": true,
	"content-length": true,
	"if-modified-since": true,
	"proxy-authorization": true,
	"if-unmodified-since": true,
};

/** merge one header line into `dest` with Node's duplicate rules */
function addHeaderLine(field: string, value: string, dest: HeaderMap, joinDuplicates: boolean): void {
	const name = field.toLowerCase();
	if (name === "set-cookie") {
		const cur = dest[name];
		if (Array.isArray(cur)) cur.push(value);
		else dest[name] = [value];
	} else if (FIRST_WINS[name] === true) {
		if (dest[name] === undefined) dest[name] = value;
		else if (joinDuplicates) dest[name] += `, ${value}`;
	} else if (typeof dest[name] === "string") {
		dest[name] += (name === "cookie" ? "; " : ", ") + value;
	} else {
		dest[name] = value;
	}
}

function buildHeaders(raw: string[], joinDuplicates: boolean): HeaderMap {
	const dest: HeaderMap = Object.create(null) as HeaderMap;
	for (let i = 0; i < raw.length; i += 2) addHeaderLine(raw[i], raw[i + 1], dest, joinDuplicates);
	return dest;
}

function buildDistinct(raw: string[]): Record<string, string[]> {
	const dest = Object.create(null) as Record<string, string[]>;
	for (let i = 0; i < raw.length; i += 2) {
		const name = raw[i].toLowerCase();
		const list = dest[name];
		if (list) list.push(raw[i + 1]);
		else dest[name] = [raw[i + 1]];
	}
	return dest;
}

export class IncomingMessage extends Readable {
	socket: Socket | null;
	httpVersionMajor: number | null = null;
	httpVersionMinor: number | null = null;
	httpVersion: string | null = null;
	complete = false;
	rawHeaders: string[] = [];
	rawTrailers: string[] = [];
	joinDuplicateHeaders = false;
	aborted = false;
	upgrade: boolean | null = null;
	url = "";
	method: string | null = null;
	statusCode: number | null = null;
	statusMessage: string | null = null;
	client: Socket | null;
	req: unknown = null;
	_consuming = false;
	_dumped = false;
	#headers: HeaderMap | null = null;
	#headersDistinct: Record<string, string[]> | null = null;
	#trailers: HeaderMap | null = null;
	#trailersDistinct: Record<string, string[]> | null = null;
	/** resumes the socket when the body is read (set by the owner) */
	_onRead: (() => void) | null = null;

	constructor(socket: Socket | null, options?: { highWaterMark?: number }) {
		let streamOptions: Record<string, unknown> | undefined;
		if (socket) {
			streamOptions = { highWaterMark: options?.highWaterMark ?? socket.readableHighWaterMark };
		}
		super(streamOptions);
		this.socket = socket;
		this.client = socket;
	}

	get connection(): Socket | null {
		return this.socket;
	}

	set connection(value: Socket | null) {
		this.socket = value;
	}

	get headers(): HeaderMap {
		if (!this.#headers) this.#headers = buildHeaders(this.rawHeaders, this.joinDuplicateHeaders);
		return this.#headers;
	}

	set headers(value: HeaderMap) {
		this.#headers = value;
	}

	get headersDistinct(): Record<string, string[]> {
		if (!this.#headersDistinct) this.#headersDistinct = buildDistinct(this.rawHeaders);
		return this.#headersDistinct;
	}

	set headersDistinct(value: Record<string, string[]>) {
		this.#headersDistinct = value;
	}

	get trailers(): HeaderMap {
		if (!this.#trailers) this.#trailers = buildHeaders(this.rawTrailers, this.joinDuplicateHeaders);
		return this.#trailers;
	}

	set trailers(value: HeaderMap) {
		this.#trailers = value;
	}

	get trailersDistinct(): Record<string, string[]> {
		if (!this.#trailersDistinct) this.#trailersDistinct = buildDistinct(this.rawTrailers);
		return this.#trailersDistinct;
	}

	set trailersDistinct(value: Record<string, string[]>) {
		this.#trailersDistinct = value;
	}

	setTimeout(msecs: number, callback?: () => void): this {
		if (callback) this.on("timeout", callback);
		this.socket?.setTimeout(msecs);
		return this;
	}

	_read(): void {
		if (!this._consuming) this._consuming = true;
		if (this.socket?.readable) this._onRead?.();
	}

	_destroy(err: Error | null, cb: (err?: Error | null) => void): void {
		if (!this.readableEnded || !this.complete) {
			this.aborted = true;
			this.emit("aborted");
		}
		const socket = this.socket;
		if (socket && !socket.destroyed && this.aborted) {
			socket.destroy(err ?? undefined);
			// the socket was live, so its 'close' is still to come
			socket.once("close", () => process.nextTick(() => cb(this.listenerCount("error") === 0 ? null : err)));
		} else if (err == null && !this.aborted) {
			cb();
		} else {
			process.nextTick(() => cb(this.listenerCount("error") === 0 ? null : err));
		}
	}

	/** discard an unread body so the connection can go on */
	_dump(): void {
		if (this._dumped) return;
		this._dumped = true;
		this.removeAllListeners("data");
		if (this.complete && !this.destroyed && this.readableLength === 0) this.read(0);
		else this.resume();
	}

	/** set by the parser owner from a MessageHead */
	_addHead(rawHeaders: string[], versionMajor: number, versionMinor: number): void {
		this.rawHeaders = rawHeaders;
		this.#headers = null;
		this.#headersDistinct = null;
		this.httpVersionMajor = versionMajor;
		this.httpVersionMinor = versionMinor;
		this.httpVersion = `${versionMajor}.${versionMinor}`;
	}

	_addTrailers(rawTrailers: string[]): void {
		this.rawTrailers = rawTrailers;
		this.#trailers = null;
		this.#trailersDistinct = null;
	}
}

export default IncomingMessage;
