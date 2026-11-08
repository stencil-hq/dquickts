// HTTP/1.x message parser (requests and responses), incremental, in the
// spirit of llhttp as used by Node: execute() feeds bytes, callbacks report
// the head, body chunks and the end of each message; pipelined messages,
// chunked transfer coding with trailers, Content-Length and read-until-EOF
// bodies, upgrades and CONNECT are handled.

import { Buffer } from "node:buffer";

export const REQUEST = 1;
export const RESPONSE = 2;

/** onHeadersComplete() return values */
export const NORMAL = 0;
export const SKIP_BODY = 1;
export const UPGRADE = 2;

export interface MessageHead {
	versionMajor: number;
	versionMinor: number;
	rawHeaders: string[];
	method: string | null;
	url: string | null;
	statusCode: number;
	statusMessage: string;
	/** Upgrade/CONNECT request, or a 101 response */
	upgrade: boolean;
	shouldKeepAlive: boolean;
}

export interface ParseError extends Error {
	code: string;
	reason: string;
	bytesParsed: number;
	rawPacket?: Buffer;
}

const State = {
	Head: 0,
	BodyLength: 1,
	ChunkSize: 2,
	ChunkData: 3,
	ChunkDataEnd: 4,
	Trailers: 5,
	BodyEof: 6,
	Upgraded: 7,
	Closed: 8,
} as const;
type ParserState = (typeof State)[keyof typeof State];

const TOKEN = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
const BAD_VALUE = /[\x00-\x08\x0a-\x1f\x7f]/;
const HEAD_END = /\r?\n\r?\n/g;
const MAX_CHUNK_LINE = 4096;

function parseError(code: string, reason: string, bytesParsed: number): ParseError {
	const err = new Error(`Parse Error: ${reason}`) as ParseError;
	err.code = code;
	err.reason = reason;
	err.bytesParsed = bytesParsed;
	return err;
}

function hasToken(value: string, token: string): boolean {
	for (const part of value.split(",")) if (part.trim().toLowerCase() === token) return true;
	return false;
}

export class HTTPParser {
	static REQUEST = REQUEST;
	static RESPONSE = RESPONSE;

	type: number;
	maxHeaderSize: number;
	/** headers beyond this many name/value pairs are dropped (0 = no limit) */
	maxHeaderPairs = 4000;
	lenient: boolean;
	onHeadersComplete: ((head: MessageHead) => number) | null = null;
	onBody: ((chunk: Buffer) => void) | null = null;
	onMessageComplete: ((rawTrailers: string[]) => void) | null = null;
	/** set when the last execute() stopped at an upgrade (bytes after it belong to the new protocol) */
	upgraded = false;

	#state: ParserState = State.Head;
	#head = "";
	#remaining = 0;
	#line = "";
	#trailers = "";
	#lastStatus = 0;
	#consumed = 0;

	constructor(type: number, options: { maxHeaderSize?: number; lenient?: boolean } = {}) {
		this.type = type;
		this.maxHeaderSize = options.maxHeaderSize || 16384;
		this.lenient = Boolean(options.lenient);
	}

	/** reuse the parser for a new connection */
	reinitialize(type: number): void {
		this.type = type;
		this.#state = State.Head;
		this.#head = "";
		this.#line = "";
		this.#trailers = "";
		this.#remaining = 0;
		this.upgraded = false;
	}

	/** stop delivering events (the connection is gone) */
	close(): void {
		this.#state = State.Closed;
	}

	get closed(): boolean {
		return this.#state === State.Closed;
	}

	/** bytes consumed by the last execute() */
	get consumed(): number {
		return this.#consumed;
	}

	/** feed bytes; the number of bytes consumed or a ParseError */
	execute(data: Buffer): number | ParseError {
		let i = 0;
		const len = data.length;
		this.upgraded = false;
		while (i < len) {
			switch (this.#state) {
				case State.Closed:
				case State.Upgraded:
					this.#consumed = i;
					return i;
				case State.Head: {
					if (this.#head.length === 0) {
						// empty lines before a message are ignored
						while (i < len && (data[i] === 13 || data[i] === 10)) i++;
						if (i >= len) break;
					}
					const old = this.#head.length;
					const take = Math.min(len - i, this.maxHeaderSize + 4 - old);
					if (take <= 0) return parseError("HPE_HEADER_OVERFLOW", "Header overflow", i);
					this.#head += data.toString("latin1", i, i + take);
					HEAD_END.lastIndex = Math.max(0, old - 3);
					const m = HEAD_END.exec(this.#head);
					if (!m) {
						if (this.#head.length > this.maxHeaderSize) return parseError("HPE_HEADER_OVERFLOW", "Header overflow", i);
						i += take;
						break;
					}
					const end = m.index + m[0].length;
					i += end - old;
					const head = this.#head.slice(0, m.index);
					this.#head = "";
					const r = this.#onHead(head, i);
					if (r) return r;
					break;
				}
				case State.BodyLength: {
					const n = Math.min(this.#remaining, len - i);
					const chunk = data.subarray(i, i + n);
					i += n;
					this.#remaining -= n;
					this.onBody?.(chunk);
					if (this.#remaining === 0 && this.#state === State.BodyLength) this.#complete([]);
					break;
				}
				case State.ChunkSize: {
					const nl = data.indexOf(10, i);
					const stop = nl === -1 ? len : nl;
					this.#line += data.toString("latin1", i, stop);
					if (this.#line.length > MAX_CHUNK_LINE)
						return parseError("HPE_INVALID_CHUNK_SIZE", "Invalid character in chunk size", i);
					if (nl === -1) {
						i = len;
						break;
					}
					i = nl + 1;
					let line = this.#line;
					this.#line = "";
					const semi = line.indexOf(";");
					if (semi >= 0) line = line.slice(0, semi);
					line = line.trim();
					if (!/^[0-9a-fA-F]+$/.test(line) || line.length > 15) {
						return parseError("HPE_INVALID_CHUNK_SIZE", "Invalid character in chunk size", i);
					}
					const size = parseInt(line, 16);
					if (size === 0) {
						this.#state = State.Trailers;
						this.#trailers = "";
					} else {
						this.#remaining = size;
						this.#state = State.ChunkData;
					}
					break;
				}
				case State.ChunkData: {
					const n = Math.min(this.#remaining, len - i);
					const chunk = data.subarray(i, i + n);
					i += n;
					this.#remaining -= n;
					if (this.#remaining === 0) this.#state = State.ChunkDataEnd;
					this.onBody?.(chunk);
					break;
				}
				case State.ChunkDataEnd: {
					const c = data[i];
					if (c === 13 && this.#line === "") {
						this.#line = "\r";
						i++;
					} else if (c === 10) {
						this.#line = "";
						i++;
						this.#state = State.ChunkSize;
					} else {
						return parseError("HPE_STRICT", "Expected LF after chunk data", i);
					}
					break;
				}
				case State.Trailers: {
					const old = this.#trailers.length;
					const take = Math.min(len - i, this.maxHeaderSize + 4 - old);
					if (take <= 0) return parseError("HPE_HEADER_OVERFLOW", "Header overflow", i);
					this.#trailers += data.toString("latin1", i, i + take);
					const empty = /^\r?\n/.exec(this.#trailers);
					let end: number;
					let body: string;
					if (empty) {
						end = empty[0].length;
						body = "";
					} else {
						HEAD_END.lastIndex = Math.max(0, old - 3);
						const m = HEAD_END.exec(this.#trailers);
						if (!m) {
							if (this.#trailers.length > this.maxHeaderSize)
								return parseError("HPE_HEADER_OVERFLOW", "Header overflow", i);
							i += take;
							break;
						}
						end = m.index + m[0].length;
						body = this.#trailers.slice(0, m.index);
					}
					i += end - old;
					this.#trailers = "";
					const raw: string[] = [];
					if (body) {
						const err = this.#parseFields(body.split(/\r?\n/), raw, i);
						if (err) return err;
					}
					this.#complete(raw);
					break;
				}
				case State.BodyEof: {
					const chunk = data.subarray(i);
					i = len;
					this.onBody?.(chunk);
					break;
				}
			}
		}
		this.#consumed = i;
		return i;
	}

	/** the connection ended: completes an EOF-delimited body, or an error for a truncated message */
	finish(): ParseError | null {
		switch (this.#state) {
			case State.BodyEof:
				this.#complete([]);
				return null;
			case State.Head:
				if (this.#head.length === 0) return null;
				break;
			case State.Upgraded:
			case State.Closed:
				return null;
		}
		const err = parseError("HPE_INVALID_EOF_STATE", "Invalid EOF state", 0);
		this.#state = State.Closed;
		return err;
	}

	#complete(rawTrailers: string[]): void {
		if (this.#state === State.Closed) return;
		this.#state = State.Head;
		this.onMessageComplete?.(rawTrailers);
	}

	#parseFields(lines: string[], raw: string[], at: number): ParseError | null {
		for (const line of lines) {
			if (line.length === 0) continue;
			const c = line.charCodeAt(0);
			if (c === 32 || c === 9) {
				// obsolete line folding
				if (raw.length === 0) return parseError("HPE_INVALID_HEADER_TOKEN", "Invalid header field char", at);
				raw[raw.length - 1] += ` ${line.trim()}`;
				continue;
			}
			const colon = line.indexOf(":");
			if (colon <= 0) return parseError("HPE_INVALID_HEADER_TOKEN", "Invalid header field char", at);
			const name = line.slice(0, colon);
			if (!TOKEN.test(name)) return parseError("HPE_INVALID_HEADER_TOKEN", "Invalid header token", at);
			const value = line.slice(colon + 1).replace(/^[ \t]+|[ \t]+$/g, "");
			if (!this.lenient && BAD_VALUE.test(value))
				return parseError("HPE_INVALID_HEADER_TOKEN", "Invalid header value char", at);
			if (this.maxHeaderPairs > 0 && raw.length >= this.maxHeaderPairs * 2) continue;
			raw.push(name, value);
		}
		return null;
	}

	#onHead(head: string, at: number): ParseError | null {
		const lines = head.split(/\r?\n/);
		const start = lines[0];
		const info: MessageHead = {
			versionMajor: 1,
			versionMinor: 1,
			rawHeaders: [],
			method: null,
			url: null,
			statusCode: 0,
			statusMessage: "",
			upgrade: false,
			shouldKeepAlive: false,
		};
		if (this.type === REQUEST) {
			const parts = start.split(" ");
			if (parts.length !== 3 || !TOKEN.test(parts[0]))
				return parseError("HPE_INVALID_METHOD", "Invalid method encountered", at);
			if (!parts[1] || /[\x00-\x20\x7f]/.test(parts[1])) return parseError("HPE_INVALID_URL", "Invalid URL", at);
			const v = /^HTTP\/(\d)\.(\d)$/.exec(parts[2]);
			if (!v) return parseError("HPE_INVALID_VERSION", "Invalid HTTP version", at);
			info.method = parts[0];
			info.url = parts[1];
			info.versionMajor = +v[1];
			info.versionMinor = +v[2];
		} else {
			const m = /^HTTP\/(\d)\.(\d) (\d{3})(?: (.*))?$/.exec(start);
			if (!m) {
				return /^HTTP\/\d\.\d/.test(start)
					? parseError("HPE_INVALID_STATUS", "Invalid status code", at)
					: parseError("HPE_INVALID_CONSTANT", "Expected HTTP/", at);
			}
			info.versionMajor = +m[1];
			info.versionMinor = +m[2];
			info.statusCode = +m[3];
			info.statusMessage = m[4] ?? "";
		}
		if (info.versionMajor !== 1 && !(info.versionMajor === 0 && info.versionMinor === 9)) {
			return parseError("HPE_INVALID_VERSION", "Invalid HTTP version", at);
		}
		const err = this.#parseFields(lines.slice(1), info.rawHeaders, at);
		if (err) return err;

		let connection = "";
		let te: string | null = null;
		let cl: string | null = null;
		let hasUpgrade = false;
		const raw = info.rawHeaders;
		for (let k = 0; k < raw.length; k += 2) {
			const name = raw[k].toLowerCase();
			const value = raw[k + 1];
			if (name === "connection") connection += `${value},`;
			else if (name === "transfer-encoding") te = te === null ? value : `${te}, ${value}`;
			else if (name === "content-length") {
				if (!/^\d+$/.test(value))
					return parseError("HPE_INVALID_CONTENT_LENGTH", "Invalid character in Content-Length", at);
				if (cl !== null && cl !== value)
					return parseError("HPE_UNEXPECTED_CONTENT_LENGTH", "Duplicate Content-Length", at);
				cl = value;
			} else if (name === "upgrade") hasUpgrade = true;
		}

		const v11 = info.versionMajor > 1 || (info.versionMajor === 1 && info.versionMinor >= 1);
		let keepAlive = v11 ? !hasToken(connection, "close") : hasToken(connection, "keep-alive");

		if (this.type === REQUEST) {
			info.upgrade = info.method === "CONNECT" || (hasUpgrade && hasToken(connection, "upgrade"));
		} else {
			info.upgrade = info.statusCode === 101;
		}

		// body framing (RFC 9112 6.3)
		let mode: "none" | "length" | "chunked" | "eof" = "none";
		let length = 0;
		if (te !== null) {
			const codings = te.split(",").map((s) => s.trim().toLowerCase());
			const chunked = codings[codings.length - 1] === "chunked";
			if (chunked) {
				if (cl !== null && this.type === REQUEST && !this.lenient) {
					return parseError(
						"HPE_UNEXPECTED_CONTENT_LENGTH",
						"Content-Length can't be present with Transfer-Encoding",
						at,
					);
				}
				mode = "chunked";
			} else if (this.type === REQUEST) {
				return parseError("HPE_INVALID_TRANSFER_ENCODING", "Request has invalid `Transfer-Encoding`", at);
			} else {
				mode = "eof";
			}
		} else if (cl !== null) {
			length = parseInt(cl, 10);
			if (!Number.isSafeInteger(length))
				return parseError("HPE_INVALID_CONTENT_LENGTH", "Invalid character in Content-Length", at);
			mode = length > 0 ? "length" : "none";
		} else if (this.type === RESPONSE) {
			mode = "eof";
		}
		const status = info.statusCode;
		if (this.type === RESPONSE && ((status >= 100 && status < 200) || status === 204 || status === 304)) mode = "none";
		if (mode === "eof") keepAlive = false;
		info.shouldKeepAlive = keepAlive;
		this.#lastStatus = status;

		const ret = this.onHeadersComplete ? this.onHeadersComplete(info) : NORMAL;
		if (this.#state === State.Closed) return null;
		if (ret === UPGRADE) {
			this.#complete([]);
			if (this.#state !== State.Closed) this.#state = State.Upgraded;
			this.upgraded = true;
			return null;
		}
		if (ret === SKIP_BODY) mode = "none";
		switch (mode) {
			case "none":
				this.#complete([]);
				break;
			case "length":
				this.#remaining = length;
				this.#state = State.BodyLength;
				break;
			case "chunked":
				this.#line = "";
				this.#state = State.ChunkSize;
				break;
			case "eof":
				this.#state = State.BodyEof;
				break;
		}
		return null;
	}

	/** status code of the last response head (1xx handling by the client) */
	get lastStatus(): number {
		return this.#lastStatus;
	}
}

export default HTTPParser;
