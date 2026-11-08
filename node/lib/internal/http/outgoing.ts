// http.OutgoingMessage: the common base of ClientRequest and ServerResponse.
// Adapted from Node.js lib/_http_outgoing.js (MIT).

import { Stream } from "node:stream";
import { Buffer } from "node:buffer";
import process from "node:process";
import type { Socket } from "node:net";
import { captureRejectionSymbol } from "node:events";
import { codedError, invalidArgType } from "internal:errors";
import {
	checkInvalidHeaderChar,
	headersSentError,
	streamDestroyed,
	streamWriteAfterEnd,
	utcDateHeader,
	validateHeaderName,
	validateHeaderValue,
	type HeaderValue,
} from "internal:http/common";

const RE_CONN_CLOSE = /(?:^|\W)close(?:$|\W)/i;
const RE_TE_CHUNKED = /(?:^|\W)chunked(?:$|\W)/i;
const CRLF = "\r\n";
const HIGH_WATER_MARK = 16384;

type WriteCallback = (err?: Error | null) => void;
type Chunk = string | Uint8Array;

interface Pending {
	data: Chunk;
	encoding: BufferEncoding | null;
	callback: WriteCallback | null;
}

interface HeaderState {
	connection: boolean;
	contLen: boolean;
	te: boolean;
	date: boolean;
	expect: boolean;
	trailer: boolean;
	header: string;
}

export const kOutHeaders: unique symbol = Symbol("kOutHeaders");
export const kNeedDrain: unique symbol = Symbol("kNeedDrain");
export const kUniqueHeaders: unique symbol = Symbol("kUniqueHeaders");

export type StoredHeaders = Record<string, [string, HeaderValue]>;
export type HeadersInput =
	| StoredHeaders
	| Record<string, HeaderValue | undefined>
	| (string | HeaderValue)[]
	| [string, HeaderValue][];

function nop(): void {}

function byteLength(chunk: Chunk, encoding?: BufferEncoding | null): number {
	return typeof chunk === "string" ? Buffer.byteLength(chunk, encoding || "utf8") : chunk.byteLength;
}

function isCookieField(s: string): boolean {
	return s.length === 6 && s.toLowerCase() === "cookie";
}

export interface OutgoingMessage extends Stream {
	on(event: string, listener: (...args: unknown[]) => void): this;
	once(event: string, listener: (...args: unknown[]) => void): this;
	emit(event: string, ...args: unknown[]): boolean;
	removeListener(event: string, listener: (...args: unknown[]) => void): this;
	listenerCount(event: string): number;
}

export class OutgoingMessage extends Stream {
	outputData: Pending[] = [];
	outputSize = 0;
	writable = true;
	destroyed = false;
	_last = false;
	chunkedEncoding = false;
	shouldKeepAlive = true;
	maxRequestsOnConnectionReached = false;
	_defaultKeepAlive = true;
	useChunkedEncodingByDefault = true;
	sendDate = false;
	_removedConnection = false;
	_removedContLen = false;
	_removedTE = false;
	strictContentLength = false;
	_contentLength: number | null = null;
	_hasBody = true;
	_trailer = "";
	finished = false;
	_headerSent = false;
	_header: string | null = null;
	_closed = false;
	_keepAliveTimeout = 0;
	_maxRequestsPerSocket = 0;
	agent: unknown = null;
	statusCode?: number;
	_onPendingData: (delta: number) => void = nop;
	[kOutHeaders]: StoredHeaders | null = null;
	[kNeedDrain] = false;
	[kUniqueHeaders]: Set<string> | null = null;
	#socket: Socket | null = null;
	#corked = 0;
	#highWaterMark: number;
	#bytesWritten = 0;
	#errored: Error | null = null;
	#writableFinished = false;
	#endCallbacks: WriteCallback[] | null = null;
	#flushError: Error | null = null;

	constructor(options?: { highWaterMark?: number }) {
		super();
		this.#highWaterMark = options?.highWaterMark ?? HIGH_WATER_MARK;
	}

	get errored(): Error | null {
		return this.#errored;
	}

	get closed(): boolean {
		return this._closed;
	}

	get writableFinished(): boolean {
		return this.finished && this.outputSize === 0 && (!this.#socket || this.#socket.writableLength === 0);
	}

	get writableObjectMode(): boolean {
		return false;
	}

	get writableLength(): number {
		return this.outputSize + (this.#socket ? this.#socket.writableLength : 0);
	}

	get writableHighWaterMark(): number {
		return this.#socket ? this.#socket.writableHighWaterMark : this.#highWaterMark;
	}

	get writableCorked(): number {
		return this.#corked;
	}

	get writableEnded(): boolean {
		return this.finished;
	}

	get writableNeedDrain(): boolean {
		return !this.destroyed && !this.finished && this[kNeedDrain];
	}

	get connection(): Socket | null {
		return this.#socket;
	}

	set connection(value: Socket | null) {
		this.socket = value;
	}

	get socket(): Socket | null {
		return this.#socket;
	}

	set socket(value: Socket | null) {
		for (let n = this.#corked; n > 0; n--) value?.cork();
		this.#socket = value;
	}

	get headersSent(): boolean {
		return Boolean(this._header);
	}

	cork(): void {
		this.#corked++;
		this.#socket?.cork();
	}

	uncork(): void {
		if (this.#corked > 0) this.#corked--;
		this.#socket?.uncork();
	}

	setTimeout(msecs: number, callback?: () => void): this {
		if (callback) this.on("timeout", callback);
		if (!this.#socket) {
			this.once("socket", (socket: unknown) => (socket as Socket).setTimeout(msecs));
		} else {
			this.#socket.setTimeout(msecs);
		}
		return this;
	}

	destroy(error?: Error | null): this {
		if (this.destroyed) return this;
		this.destroyed = true;
		this.#errored = error ?? null;
		if (this.#socket) {
			this.#socket.destroy(error ?? undefined);
		} else {
			this.once("socket", (socket: unknown) => (socket as Socket).destroy(error ?? undefined));
		}
		return this;
	}

	// -- headers -------------------------------------------------------------

	setHeader(name: string, value: HeaderValue): this {
		if (this._header) throw headersSentError("set");
		validateHeaderName(name);
		validateHeaderValue(name, value);
		let headers = this[kOutHeaders];
		if (headers === null) this[kOutHeaders] = headers = Object.create(null) as StoredHeaders;
		headers[name.toLowerCase()] = [name, value];
		return this;
	}

	setHeaders(headers: Headers | Map<string, HeaderValue>): this {
		if (this._header) throw headersSentError("set");
		if (
			!headers ||
			Array.isArray(headers) ||
			typeof (headers as Map<string, unknown>).keys !== "function" ||
			typeof (headers as Map<string, unknown>).get !== "function"
		) {
			throw invalidArgType("headers", "an instance of Headers or Map", headers);
		}
		const cookies: string[] = [];
		for (const [key, value] of headers as Iterable<[string, HeaderValue]>) {
			if (key === "set-cookie") {
				if (Array.isArray(value)) cookies.push(...value);
				else cookies.push(String(value));
				continue;
			}
			this.setHeader(key, value);
		}
		if (cookies.length) this.setHeader("set-cookie", cookies);
		return this;
	}

	appendHeader(name: string, value: HeaderValue): this {
		if (this._header) throw headersSentError("append");
		validateHeaderName(name);
		validateHeaderValue(name, value);
		const field = name.toLowerCase();
		let headers = this[kOutHeaders];
		if (headers === null) this[kOutHeaders] = headers = Object.create(null) as StoredHeaders;
		const existing = headers[field];
		if (existing === undefined) {
			headers[field] = [name, value];
			return this;
		}
		const prev = Array.isArray(existing[1]) ? existing[1] : [String(existing[1])];
		const next = Array.isArray(value) ? value : [String(value)];
		existing[1] = [...prev, ...next];
		return this;
	}

	getHeader(name: string): HeaderValue | undefined {
		if (typeof name !== "string") throw invalidArgType("name", "of type string", name);
		const entry = this[kOutHeaders]?.[name.toLowerCase()];
		return entry?.[1];
	}

	getHeaderNames(): string[] {
		return this[kOutHeaders] !== null ? Object.keys(this[kOutHeaders]) : [];
	}

	getRawHeaderNames(): string[] {
		const headers = this[kOutHeaders];
		if (headers === null) return [];
		const out: string[] = [];
		for (const k in headers) out.push(headers[k][0]);
		return out;
	}

	getHeaders(): Record<string, HeaderValue> {
		const headers = this[kOutHeaders];
		const ret = Object.create(null) as Record<string, HeaderValue>;
		if (headers) for (const k in headers) ret[k] = headers[k][1];
		return ret;
	}

	hasHeader(name: string): boolean {
		if (typeof name !== "string") throw invalidArgType("name", "of type string", name);
		return this[kOutHeaders] !== null && Boolean(this[kOutHeaders][name.toLowerCase()]);
	}

	removeHeader(name: string): void {
		if (typeof name !== "string") throw invalidArgType("name", "of type string", name);
		if (this._header) throw headersSentError("remove");
		const key = name.toLowerCase();
		switch (key) {
			case "connection":
				this._removedConnection = true;
				break;
			case "content-length":
				this._removedContLen = true;
				break;
			case "transfer-encoding":
				this._removedTE = true;
				break;
			case "date":
				this.sendDate = false;
				break;
		}
		if (this[kOutHeaders] !== null) delete this[kOutHeaders][key];
	}

	_implicitHeader(): void {
		throw codedError("ERR_METHOD_NOT_IMPLEMENTED", "The _implicitHeader() method is not implemented");
	}

	_renderHeaders(): Record<string, HeaderValue> {
		if (this._header) throw headersSentError("render");
		const headers = this[kOutHeaders];
		const out: Record<string, HeaderValue> = {};
		if (headers) for (const k in headers) out[headers[k][0]] = headers[k][1];
		return out;
	}

	_storeHeader(firstLine: string, headers: HeadersInput | null): void {
		const state: HeaderState = {
			connection: false,
			contLen: false,
			te: false,
			date: false,
			expect: false,
			trailer: false,
			header: firstLine,
		};
		if (headers) {
			if (headers === this[kOutHeaders]) {
				const stored = headers as StoredHeaders;
				for (const key in stored) this.#processHeader(state, stored[key][0], stored[key][1], false);
			} else if (Array.isArray(headers)) {
				if (headers.length && Array.isArray(headers[0])) {
					for (const entry of headers as [string, HeaderValue][]) this.#processHeader(state, entry[0], entry[1], true);
				} else {
					if (headers.length % 2 !== 0)
						throw codedError("ERR_INVALID_ARG_VALUE", "The argument 'headers' is invalid.", TypeError);
					for (let n = 0; n < headers.length; n += 2)
						this.#processHeader(state, headers[n] as string, headers[n + 1] as HeaderValue, true);
				}
			} else {
				const obj = headers as Record<string, HeaderValue | undefined>;
				for (const key in obj) {
					if (Object.hasOwn(obj, key)) this.#processHeader(state, key, obj[key] as HeaderValue, true);
				}
			}
		}
		let { header } = state;
		if (this.sendDate && !state.date) header += utcDateHeader();

		if (this.chunkedEncoding && (this.statusCode === 204 || this.statusCode === 304)) {
			this.chunkedEncoding = false;
			this.shouldKeepAlive = false;
		}

		if (this._removedConnection) {
			this._last = !this.shouldKeepAlive;
		} else if (!state.connection) {
			const shouldSendKeepAlive =
				this.shouldKeepAlive && (state.contLen || this.useChunkedEncodingByDefault || this.agent);
			if (shouldSendKeepAlive && this.maxRequestsOnConnectionReached) {
				header += "Connection: close\r\n";
			} else if (shouldSendKeepAlive) {
				header += "Connection: keep-alive\r\n";
				if (this._keepAliveTimeout && this._defaultKeepAlive) {
					const max = ~~this._maxRequestsPerSocket;
					header += `Keep-Alive: timeout=${Math.floor(this._keepAliveTimeout / 1000)}${max > 0 ? `, max=${max}` : ""}\r\n`;
				}
			} else {
				this._last = true;
				header += "Connection: close\r\n";
			}
		}

		if (!state.contLen && !state.te) {
			if (!this._hasBody) {
				this.chunkedEncoding = false;
			} else if (!this.useChunkedEncodingByDefault) {
				this._last = true;
			} else if (!state.trailer && !this._removedContLen && typeof this._contentLength === "number") {
				header += `Content-Length: ${this._contentLength}\r\n`;
			} else if (!this._removedTE) {
				header += "Transfer-Encoding: chunked\r\n";
				this.chunkedEncoding = true;
			} else {
				this._last = true;
			}
		}

		if (this.chunkedEncoding !== true && state.trailer) {
			throw codedError("ERR_HTTP_TRAILER_INVALID", "Trailers are invalid with this transfer encoding");
		}
		this._header = header + CRLF;
		this._headerSent = false;
		if (state.expect) this._send("");
	}

	#processHeader(state: HeaderState, key: string, value: HeaderValue, validate: boolean): void {
		if (validate) validateHeaderName(key);
		if (Array.isArray(value)) {
			const unique = this[kUniqueHeaders];
			if ((value.length < 2 || !isCookieField(key)) && (!unique || !unique.has(key.toLowerCase()))) {
				for (const v of value) this.#storeHeader(state, key, v, validate);
				return;
			}
			value = value.join(unique?.has(key.toLowerCase()) ? ", " : "; ");
		}
		this.#storeHeader(state, key, value, validate);
	}

	#storeHeader(state: HeaderState, key: string, value: string | number, validate: boolean): void {
		if (validate) validateHeaderValue(key, value);
		state.header += `${key}: ${value}${CRLF}`;
		if (key.length < 4 || key.length > 17) return;
		switch (key.toLowerCase()) {
			case "connection":
				state.connection = true;
				this._removedConnection = false;
				if (RE_CONN_CLOSE.test(String(value))) this._last = true;
				else this.shouldKeepAlive = true;
				break;
			case "transfer-encoding":
				state.te = true;
				this._removedTE = false;
				if (RE_TE_CHUNKED.test(String(value))) this.chunkedEncoding = true;
				break;
			case "content-length":
				state.contLen = true;
				this._contentLength = +value;
				this._removedContLen = false;
				break;
			case "date":
				state.date = true;
				break;
			case "expect":
				state.expect = true;
				break;
			case "trailer":
				state.trailer = true;
				break;
			case "keep-alive":
				this._defaultKeepAlive = false;
				break;
		}
	}

	addTrailers(headers: Record<string, HeaderValue> | [string, HeaderValue][]): void {
		this._trailer = "";
		if (Array.isArray(headers)) {
			for (const [field, value] of headers) this.#addTrailer(field, value);
		} else {
			for (const field in headers) this.#addTrailer(field, headers[field]);
		}
	}

	#addTrailer(field: string, raw: HeaderValue): void {
		validateHeaderName(field, "Trailer name");
		let value = raw;
		if (Array.isArray(value) && value.length > 1 && !this[kUniqueHeaders]?.has(field.toLowerCase())) {
			for (const v of value) {
				if (checkInvalidHeaderChar(v))
					throw codedError("ERR_INVALID_CHAR", `Invalid character in trailer content ["${field}"]`, TypeError);
				this._trailer += `${field}: ${v}${CRLF}`;
			}
			return;
		}
		if (Array.isArray(value)) value = value.join("; ");
		if (checkInvalidHeaderChar(value))
			throw codedError("ERR_INVALID_CHAR", `Invalid character in trailer content ["${field}"]`, TypeError);
		this._trailer += `${field}: ${value}${CRLF}`;
	}

	// -- body ----------------------------------------------------------------

	_send(data: Chunk, encoding?: BufferEncoding | null, callback?: WriteCallback | null): boolean {
		if (!this._headerSent && this._header !== null) {
			if (typeof data === "string" && (encoding === "utf8" || encoding === "latin1" || !encoding)) {
				data = this._header + data;
			} else {
				const header = this._header;
				this.outputData.unshift({ data: header, encoding: "latin1", callback: null });
				this.outputSize += header.length;
				this._onPendingData(header.length);
			}
			this._headerSent = true;
		}
		return this._writeRaw(data, encoding ?? null, callback ?? null);
	}

	_writeRaw(data: Chunk, encoding: BufferEncoding | null, callback: WriteCallback | null): boolean {
		const conn = this.#socket;
		if (conn?.destroyed) return false;
		if (conn && (conn as unknown as { _httpMessage: unknown })._httpMessage === this && conn.writable) {
			if (this.outputData.length) this._flushOutput(conn);
			return conn.write(data, encoding ?? undefined, callback ?? undefined);
		}
		this.outputData.push({ data, encoding, callback });
		this.outputSize += data.length;
		this._onPendingData(data.length);
		return this.outputSize < this.#highWaterMark;
	}

	write(chunk: Chunk, encoding?: BufferEncoding | WriteCallback | null, callback?: WriteCallback): boolean {
		if (typeof encoding === "function") {
			callback = encoding;
			encoding = null;
		}
		const ret = this.#write(chunk, encoding ?? null, callback ?? nop, false);
		if (!ret) this[kNeedDrain] = true;
		return ret;
	}

	#onError(err: Error, callback: WriteCallback): void {
		if (this.destroyed) return;
		process.nextTick(() => {
			callback(err);
			if (!this.destroyed) this.emit("error", err);
		});
	}

	#strictContentLength(): boolean {
		return (
			this.strictContentLength &&
			this._contentLength != null &&
			this._hasBody &&
			!this._removedContLen &&
			!this.chunkedEncoding &&
			!this.hasHeader("transfer-encoding")
		);
	}

	#write(chunk: Chunk, encoding: BufferEncoding | null, callback: WriteCallback, fromEnd: boolean): boolean {
		if (chunk === null) throw codedError("ERR_STREAM_NULL_VALUES", "May not write null values to stream", TypeError);
		if (typeof chunk !== "string" && !(chunk instanceof Uint8Array)) {
			throw invalidArgType("chunk", "of type string or an instance of Buffer or Uint8Array", chunk);
		}
		let err: Error | null = null;
		if (this.finished) err = streamWriteAfterEnd();
		else if (this.destroyed) err = streamDestroyed("write");
		if (err) {
			if (!this.destroyed) this.#onError(err, callback);
			else process.nextTick(callback, err);
			return false;
		}
		let len: number | undefined;
		if (this.strictContentLength) {
			len = byteLength(chunk, encoding);
			if (
				this.#strictContentLength() &&
				(fromEnd
					? this.#bytesWritten + len !== this._contentLength
					: this.#bytesWritten + len > (this._contentLength as number))
			) {
				throw codedError(
					"ERR_HTTP_CONTENT_LENGTH_MISMATCH",
					`Response body's content-length of ${len + this.#bytesWritten} byte(s) does not match the content-length of ${this._contentLength} byte(s) set in header`,
				);
			}
			this.#bytesWritten += len;
		}
		if (!this._header) {
			if (fromEnd) this._contentLength = len ?? byteLength(chunk, encoding);
			this._implicitHeader();
		}
		if (!this._hasBody) {
			if (this.rejectNonStandardBodyWrites) {
				throw codedError(
					"ERR_HTTP_BODY_NOT_ALLOWED",
					"Adding content for this request method or response status is not allowed.",
				);
			}
			process.nextTick(callback);
			return true;
		}
		if (!fromEnd && this.#socket && !this.#socket.writableCorked) {
			this.cork();
			process.nextTick(() => {
				if (this.#corked) this.uncork();
			});
		}
		if (this.chunkedEncoding && chunk.length !== 0) {
			len ??= byteLength(chunk, encoding);
			this._send(len.toString(16), "latin1", null);
			this._send(CRLF, "latin1", null);
			this._send(chunk, encoding, null);
			return this._send(CRLF, "latin1", callback);
		}
		return this._send(chunk, encoding, callback);
	}

	rejectNonStandardBodyWrites = false;

	#flushEndCallbacks(err: Error | null): void {
		const callbacks = this.#endCallbacks;
		if (callbacks === null) return;
		this.#endCallbacks = null;
		for (const cb of callbacks) cb(err);
	}

	#queueEndCallback(callback: WriteCallback): void {
		if (this.#writableFinished) {
			callback(codedError("ERR_STREAM_ALREADY_FINISHED", "Cannot call end after a stream was finished"));
			return;
		}
		if (this.#flushError !== null) {
			process.nextTick(callback, this.#flushError);
			return;
		}
		(this.#endCallbacks ??= []).push(callback);
	}

	#onFinish(err?: Error | null): void {
		const socket = this.#socket as (Socket & { errored?: Error | null; _hadError?: boolean }) | null;
		if (err || this.#errored || socket?.errored || socket?._hadError) {
			this.#flushError = err ?? this.#errored ?? socket?.errored ?? streamDestroyed("end");
			this.#flushEndCallbacks(this.#flushError);
			return;
		}
		this.#writableFinished = true;
		this.#flushEndCallbacks(null);
		this.emit("finish");
	}

	end(
		chunk?: Chunk | WriteCallback | null,
		encoding?: BufferEncoding | WriteCallback | null,
		callback?: WriteCallback,
	): this {
		if (typeof chunk === "function") {
			callback = chunk;
			chunk = null;
			encoding = null;
		} else if (typeof encoding === "function") {
			callback = encoding;
			encoding = null;
		}
		const enc = (encoding as BufferEncoding | null) ?? null;
		let finishCallback: WriteCallback | null = null;
		if (chunk) {
			if (this.finished) {
				this.#onError(streamWriteAfterEnd(), typeof callback !== "function" ? nop : callback);
				return this;
			}
			this.#socket?.cork();
			// one final write carrying the whole body: attach 'finish' to it
			if (!this.destroyed && !this.strictContentLength) {
				if (!this._header) {
					this._contentLength = byteLength(chunk, enc);
					this._implicitHeader();
				}
				if (this._header && this._hasBody && !this.chunkedEncoding) {
					if (typeof callback === "function") {
						this.#queueEndCallback(callback);
						callback = undefined;
					}
					finishCallback = (err?: Error | null) => this.#onFinish(err);
				}
			}
			this.#write(chunk, enc, finishCallback ?? nop, true);
		} else if (this.finished) {
			if (typeof callback === "function") this.#queueEndCallback(callback);
			return this;
		} else if (!this._header) {
			this.#socket?.cork();
			this._contentLength = 0;
			this._implicitHeader();
		}
		if (typeof callback === "function") this.#queueEndCallback(callback);
		if (this.#strictContentLength() && this.#bytesWritten !== this._contentLength) {
			throw codedError(
				"ERR_HTTP_CONTENT_LENGTH_MISMATCH",
				`Response body's content-length of ${this.#bytesWritten} byte(s) does not match the content-length of ${this._contentLength} byte(s) set in header`,
			);
		}
		if (finishCallback === null) {
			const finish = (err?: Error | null) => this.#onFinish(err);
			if (this._hasBody && this.chunkedEncoding) {
				this._send(`0\r\n${this._trailer}\r\n`, "latin1", finish);
			} else if (!this._headerSent || this.writableLength || chunk) {
				this._send("", "latin1", finish);
			} else {
				process.nextTick(finish);
			}
		}
		const socket = this.#socket;
		if (socket) while (socket.writableCorked) socket.uncork();
		this.#corked = 0;
		this.finished = true;
		if (
			this.outputData.length === 0 &&
			socket &&
			(socket as unknown as { _httpMessage: unknown })._httpMessage === this
		) {
			this._finish();
		}
		return this;
	}

	_finish(): void {
		this.emit("prefinish");
	}

	_flush(): void {
		const socket = this.#socket;
		if (socket?.writable) {
			const ret = this._flushOutput(socket);
			if (this.finished) {
				this._finish();
			} else if (this[kNeedDrain] && ret !== false) {
				this[kNeedDrain] = false;
				this.emit("drain");
			}
		}
	}

	_flushOutput(socket: Socket): boolean | undefined {
		const outputLength = this.outputData.length;
		if (outputLength <= 0) return undefined;
		const outputData = this.outputData;
		socket.cork();
		let ret: boolean | undefined;
		for (const { data, encoding, callback } of outputData) {
			ret = socket.write(data, encoding ?? undefined, callback ?? undefined);
		}
		socket.uncork();
		this.outputData = [];
		this._onPendingData(-this.outputSize);
		this.outputSize = 0;
		return ret;
	}

	flushHeaders(): void {
		if (!this._header) this._implicitHeader();
		this._send("");
	}

	pipe(): void {
		this.emit("error", codedError("ERR_STREAM_CANNOT_PIPE", "Cannot pipe, not readable"));
	}

	[captureRejectionSymbol](err: Error): void {
		this.destroy(err);
	}
}

export default OutgoingMessage;
