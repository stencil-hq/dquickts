// Server side WebSocket (RFC 6455) over a net.Socket, for Bun.serve's
// `websocket` handlers: handshake, framing, fragmentation, ping/pong, close,
// idle timeout and topic publish/subscribe (ServerWebSocket API of Bun).

import { Buffer } from "node:buffer";
import type { Socket } from "node:net";

const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// ---------------------------------------------------------------------------
// SHA-1 (handshake accept key only)

function rotl(x: number, n: number): number {
	return (x << n) | (x >>> (32 - n));
}

function sha1(data: Uint8Array): Uint8Array {
	const ml = data.length;
	const withPad = ((ml + 9 + 63) >> 6) << 6;
	const msg = new Uint8Array(withPad);
	msg.set(data);
	msg[ml] = 0x80;
	const view = new DataView(msg.buffer);
	view.setUint32(withPad - 8, Math.floor((ml * 8) / 0x100000000));
	view.setUint32(withPad - 4, (ml * 8) >>> 0);
	let h0 = 0x67452301;
	let h1 = 0xefcdab89;
	let h2 = 0x98badcfe;
	let h3 = 0x10325476;
	let h4 = 0xc3d2e1f0;
	const w = new Int32Array(80);
	for (let off = 0; off < withPad; off += 64) {
		for (let i = 0; i < 16; i++) w[i] = view.getInt32(off + i * 4);
		for (let i = 16; i < 80; i++) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		let a = h0;
		let b = h1;
		let c = h2;
		let d = h3;
		let e = h4;
		for (let i = 0; i < 80; i++) {
			let f: number;
			let k: number;
			if (i < 20) {
				f = (b & c) | (~b & d);
				k = 0x5a827999;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ed9eba1;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8f1bbcdc;
			} else {
				f = b ^ c ^ d;
				k = 0xca62c1d6;
			}
			const t = (rotl(a, 5) + f + e + k + w[i]) | 0;
			e = d;
			d = c;
			c = rotl(b, 30);
			b = a;
			a = t;
		}
		h0 = (h0 + a) | 0;
		h1 = (h1 + b) | 0;
		h2 = (h2 + c) | 0;
		h3 = (h3 + d) | 0;
		h4 = (h4 + e) | 0;
	}
	const out = new Uint8Array(20);
	const ov = new DataView(out.buffer);
	ov.setInt32(0, h0);
	ov.setInt32(4, h1);
	ov.setInt32(8, h2);
	ov.setInt32(12, h3);
	ov.setInt32(16, h4);
	return out;
}

/** Sec-WebSocket-Accept for a Sec-WebSocket-Key */
export function acceptKey(key: string): string {
	return Buffer.from(sha1(Buffer.from(key + GUID, "latin1"))).toString("base64");
}

// ---------------------------------------------------------------------------
// types

export type BinaryType = "nodebuffer" | "arraybuffer" | "uint8array" | "blob";
export type WsData = string | ArrayBuffer | ArrayBufferView | Blob;

export interface WebSocketHandler {
	message?(ws: ServerWebSocket, message: string | Buffer | ArrayBuffer | Uint8Array): unknown;
	open?(ws: ServerWebSocket): unknown;
	drain?(ws: ServerWebSocket): unknown;
	close?(ws: ServerWebSocket, code: number, reason: string): unknown;
	ping?(ws: ServerWebSocket, data: Buffer): unknown;
	pong?(ws: ServerWebSocket, data: Buffer): unknown;
	error?(error: Error, ws: ServerWebSocket): unknown;
	maxPayloadLength?: number;
	backpressureLimit?: number;
	closeOnBackpressureLimit?: boolean;
	idleTimeout?: number;
	publishToSelf?: boolean;
	sendPings?: boolean;
	perMessageDeflate?: unknown;
}

/** the topic registry of a server */
export class TopicRegistry {
	readonly topics = new Map<string, Set<ServerWebSocket>>();

	subscribe(ws: ServerWebSocket, topic: string): void {
		let set = this.topics.get(topic);
		if (!set) this.topics.set(topic, (set = new Set()));
		set.add(ws);
	}

	unsubscribe(ws: ServerWebSocket, topic: string): void {
		const set = this.topics.get(topic);
		if (!set) return;
		set.delete(ws);
		if (set.size === 0) this.topics.delete(topic);
	}

	count(topic: string): number {
		return this.topics.get(topic)?.size ?? 0;
	}

	publish(topic: string, data: WsData, except: ServerWebSocket | null): number {
		const set = this.topics.get(topic);
		if (!set) return 0;
		const { opcode, payload } = encodeData(data);
		let sent = 0;
		for (const ws of set) {
			if (ws === except) continue;
			const r = ws._sendFrame(opcode, payload);
			if (r > 0) sent += r;
		}
		return sent;
	}
}

function encodeData(data: WsData, forceBinary = false): { opcode: number; payload: Buffer } {
	if (typeof data === "string") return { opcode: forceBinary ? 2 : 1, payload: Buffer.from(data, "utf8") };
	if (data instanceof ArrayBuffer) return { opcode: 2, payload: Buffer.from(data) };
	if (ArrayBuffer.isView(data))
		return { opcode: 2, payload: Buffer.from(data.buffer, data.byteOffset, data.byteLength) };
	throw new TypeError(
		"WebSocket data must be a string, an ArrayBuffer or an ArrayBufferView (Blob is sent with sendBinary after reading it)",
	);
}

function frameHeader(opcode: number, length: number): Buffer {
	let header: Buffer;
	if (length < 126) {
		header = Buffer.alloc(2);
		header[1] = length;
	} else if (length < 0x10000) {
		header = Buffer.alloc(4);
		header[1] = 126;
		header.writeUInt16BE(length, 2);
	} else {
		header = Buffer.alloc(10);
		header[1] = 127;
		header.writeUInt32BE(Math.floor(length / 0x100000000), 2);
		header.writeUInt32BE(length >>> 0, 6);
	}
	header[0] = 0x80 | opcode;
	return header;
}

const decoder = new TextDecoder("utf-8", { fatal: true });

// ---------------------------------------------------------------------------
// ServerWebSocket

export class ServerWebSocket {
	data: unknown;
	binaryType: BinaryType = "nodebuffer";
	readyState: 0 | 1 | 2 | 3 = 1;
	readonly remoteAddress: string;
	#socket: Socket;
	#handler: WebSocketHandler;
	#topics: TopicRegistry;
	#subscriptions = new Set<string>();
	#buffer: Buffer = Buffer.alloc(0);
	#fragments: Buffer[] = [];
	#fragmentOpcode = 0;
	#fragmentSize = 0;
	#closeSent = false;
	#closeEmitted = false;
	#paused = false;
	#pingOutstanding = false;
	#onClosed: (ws: ServerWebSocket) => void;

	constructor(
		socket: Socket,
		handler: WebSocketHandler,
		topics: TopicRegistry,
		data: unknown,
		onClosed: (ws: ServerWebSocket) => void,
	) {
		this.#socket = socket;
		this.#handler = handler;
		this.#topics = topics;
		this.data = data;
		this.remoteAddress = socket.remoteAddress ?? "";
		this.#onClosed = onClosed;
		socket.setNoDelay(true);
		socket.on("data", (chunk: Buffer) => this.#onData(chunk));
		socket.on("drain", () => this.#call("drain"));
		socket.on("error", (err: Error) => {
			if (this.#handler.error) this.#handler.error(err, this);
		});
		socket.on("close", () => this.#finish(1006, ""));
		socket.on("end", () => {
			if (!this.#closeSent) socket.end();
		});
		socket.on("timeout", () => this.#onIdle());
		this.#applyIdleTimeout();
	}

	/** swap the handlers (server.reload) */
	_setHandler(handler: WebSocketHandler): void {
		this.#handler = handler;
		this.#applyIdleTimeout();
	}

	#applyIdleTimeout(): void {
		const idle = this.#handler.idleTimeout ?? 120;
		this.#socket.setTimeout(idle > 0 ? idle * 1000 : 0);
	}

	#onIdle(): void {
		if (this.#handler.sendPings !== false && !this.#pingOutstanding && this.readyState === 1) {
			this.#pingOutstanding = true;
			this._sendFrame(9, Buffer.alloc(0));
			return;
		}
		this.terminate();
	}

	#call(name: "drain" | "open"): void {
		const fn = this.#handler[name];
		if (!fn) return;
		try {
			const r = fn.call(this.#handler, this);
			if (r instanceof Promise) r.catch((e: unknown) => this.#reportError(e));
		} catch (e) {
			this.#reportError(e);
		}
	}

	#reportError(e: unknown): void {
		const err = e instanceof Error ? e : new Error(String(e));
		if (this.#handler.error) this.#handler.error(err, this);
		else
			queueMicrotask(() => {
				throw err;
			});
	}

	_open(): void {
		this.#call("open");
	}

	#onData(chunk: Buffer): void {
		this.#pingOutstanding = false;
		this.#buffer = this.#buffer.length ? Buffer.concat([this.#buffer, chunk]) : chunk;
		while (this.readyState !== 3) {
			const buf = this.#buffer;
			if (buf.length < 2) return;
			const fin = (buf[0] & 0x80) !== 0;
			const rsv = buf[0] & 0x70;
			const opcode = buf[0] & 0x0f;
			const masked = (buf[1] & 0x80) !== 0;
			let len = buf[1] & 0x7f;
			let off = 2;
			if (len === 126) {
				if (buf.length < 4) return;
				len = buf.readUInt16BE(2);
				off = 4;
			} else if (len === 127) {
				if (buf.length < 10) return;
				len = buf.readUInt32BE(2) * 0x100000000 + buf.readUInt32BE(6);
				off = 10;
			}
			if (rsv !== 0) return this.#protocolError(1002, "Invalid frame header");
			if (!masked) return this.#protocolError(1002, "Unmasked client frame");
			const max = this.#handler.maxPayloadLength ?? 16 * 1024 * 1024;
			if (len > max || this.#fragmentSize + len > max) return this.#protocolError(1009, "Message too big");
			if (buf.length < off + 4 + len) return;
			const mask = buf.subarray(off, off + 4);
			const payload = Buffer.from(buf.subarray(off + 4, off + 4 + len));
			for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
			this.#buffer = buf.subarray(off + 4 + len);
			this.#onFrame(fin, opcode, payload);
		}
	}

	#onFrame(fin: boolean, opcode: number, payload: Buffer): void {
		switch (opcode) {
			case 0x0:
				if (!this.#fragmentOpcode) return this.#protocolError(1002, "Unexpected continuation frame");
				this.#fragments.push(payload);
				this.#fragmentSize += payload.length;
				if (fin) {
					const op = this.#fragmentOpcode;
					const data = Buffer.concat(this.#fragments);
					this.#fragments = [];
					this.#fragmentOpcode = 0;
					this.#fragmentSize = 0;
					this.#deliver(op, data);
				}
				return;
			case 0x1:
			case 0x2:
				if (this.#fragmentOpcode) return this.#protocolError(1002, "Expected continuation frame");
				if (!fin) {
					this.#fragmentOpcode = opcode;
					this.#fragments = [payload];
					this.#fragmentSize = payload.length;
					return;
				}
				this.#deliver(opcode, payload);
				return;
			case 0x8: {
				let code = 1005;
				let reason = "";
				if (payload.length >= 2) {
					code = payload.readUInt16BE(0);
					try {
						reason = decoder.decode(payload.subarray(2));
					} catch {
						return this.#protocolError(1007, "Invalid UTF-8 in close reason");
					}
				}
				if (!this.#closeSent) {
					this.#closeSent = true;
					const echo = code === 1005 ? Buffer.alloc(0) : payload.subarray(0, 2);
					this.#writeFrame(8, echo);
				}
				this.readyState = 2;
				this.#socket.end();
				this.#finish(code, reason);
				return;
			}
			case 0x9:
				if (!fin || payload.length > 125) return this.#protocolError(1002, "Invalid ping frame");
				this.#writeFrame(10, payload);
				this.#handlerCall("ping", payload);
				return;
			case 0xa:
				if (!fin || payload.length > 125) return this.#protocolError(1002, "Invalid pong frame");
				this.#handlerCall("pong", payload);
				return;
		}
		this.#protocolError(1002, "Unknown opcode");
	}

	#handlerCall(name: "ping" | "pong", payload: Buffer): void {
		const fn = this.#handler[name];
		if (!fn) return;
		try {
			const r = fn.call(this.#handler, this, payload);
			if (r instanceof Promise) r.catch((e: unknown) => this.#reportError(e));
		} catch (e) {
			this.#reportError(e);
		}
	}

	#deliver(opcode: number, data: Buffer): void {
		let message: string | Buffer | ArrayBuffer | Uint8Array;
		if (opcode === 1) {
			try {
				message = decoder.decode(data);
			} catch {
				this.#protocolError(1007, "Invalid UTF-8");
				return;
			}
		} else if (this.binaryType === "arraybuffer") {
			message = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength) as ArrayBuffer;
		} else if (this.binaryType === "uint8array") {
			message = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
		} else {
			message = data;
		}
		const fn = this.#handler.message;
		if (!fn) return;
		try {
			const r = fn.call(this.#handler, this, message);
			if (r instanceof Promise) r.catch((e: unknown) => this.#reportError(e));
		} catch (e) {
			this.#reportError(e);
		}
	}

	#protocolError(code: number, reason: string): void {
		this.close(code, reason);
		this.#buffer = Buffer.alloc(0);
	}

	#finish(code: number, reason: string): void {
		if (this.#closeEmitted) return;
		this.#closeEmitted = true;
		this.readyState = 3;
		for (const topic of this.#subscriptions) this.#topics.unsubscribe(this, topic);
		this.#subscriptions.clear();
		this.#onClosed(this);
		const fn = this.#handler.close;
		if (fn) {
			try {
				const r = fn.call(this.#handler, this, code, reason);
				if (r instanceof Promise) r.catch((e: unknown) => this.#reportError(e));
			} catch (e) {
				this.#reportError(e);
			}
		}
	}

	#writeFrame(opcode: number, payload: Uint8Array): boolean {
		const socket = this.#socket;
		if (socket.destroyed || !socket.writable) return false;
		socket.cork();
		socket.write(frameHeader(opcode, payload.length));
		const ok = payload.length ? socket.write(payload) : true;
		socket.uncork();
		return ok;
	}

	/** send one frame; Bun's status: bytes sent, -1 backpressure, 0 dropped */
	_sendFrame(opcode: number, payload: Uint8Array): number {
		if (this.readyState !== 1) return 0;
		const limit = this.#handler.backpressureLimit ?? 16 * 1024 * 1024;
		if (this.#socket.writableLength > limit) {
			if (this.#handler.closeOnBackpressureLimit) this.close(1008, "Backpressure limit exceeded");
			return 0;
		}
		const ok = this.#writeFrame(opcode, payload);
		return ok ? payload.length : -1;
	}

	send(data: WsData, _compress?: boolean): number {
		const { opcode, payload } = encodeData(data);
		return this._sendFrame(opcode, payload);
	}

	sendText(data: string, _compress?: boolean): number {
		return this._sendFrame(1, Buffer.from(String(data), "utf8"));
	}

	sendBinary(data: ArrayBuffer | ArrayBufferView, _compress?: boolean): number {
		return this._sendFrame(2, encodeData(data, true).payload);
	}

	ping(data?: WsData): number {
		const payload = data === undefined ? Buffer.alloc(0) : encodeData(data).payload;
		return this._sendFrame(9, payload);
	}

	pong(data?: WsData): number {
		const payload = data === undefined ? Buffer.alloc(0) : encodeData(data).payload;
		return this._sendFrame(10, payload);
	}

	close(code?: number, reason?: string): void {
		if (this.readyState >= 2) return;
		this.readyState = 2;
		if (!this.#closeSent) {
			this.#closeSent = true;
			let payload = Buffer.alloc(0);
			if (code !== undefined) {
				const r = Buffer.from(reason ?? "", "utf8").subarray(0, 123);
				payload = Buffer.alloc(2 + r.length);
				payload.writeUInt16BE(code, 0);
				r.copy(payload, 2);
			}
			this.#writeFrame(8, payload);
		}
		this.#socket.end();
		this.#finish(code ?? 1000, reason ?? "");
	}

	terminate(): void {
		if (this.readyState === 3) return;
		this.readyState = 3;
		this.#socket.destroy();
		this.#finish(1006, "");
	}

	subscribe(topic: string): boolean {
		const t = String(topic);
		if (this.readyState !== 1) return false;
		this.#subscriptions.add(t);
		this.#topics.subscribe(this, t);
		return true;
	}

	unsubscribe(topic: string): boolean {
		const t = String(topic);
		const had = this.#subscriptions.delete(t);
		this.#topics.unsubscribe(this, t);
		return had;
	}

	isSubscribed(topic: string): boolean {
		return this.#subscriptions.has(String(topic));
	}

	get subscriptions(): string[] {
		return [...this.#subscriptions];
	}

	publish(topic: string, data: WsData, _compress?: boolean): number {
		return this.#topics.publish(String(topic), data, this.#handler.publishToSelf ? null : this);
	}

	publishText(topic: string, data: string, compress?: boolean): number {
		return this.publish(topic, String(data), compress);
	}

	publishBinary(topic: string, data: ArrayBuffer | ArrayBufferView, compress?: boolean): number {
		return this.publish(topic, data, compress);
	}

	cork<R>(callback: (ws: ServerWebSocket) => R): R {
		this.#socket.cork();
		try {
			return callback(this);
		} finally {
			this.#socket.uncork();
		}
	}

	getBufferedAmount(): number {
		return this.#socket.writableLength;
	}

	pause(): void {
		this.#paused = true;
		this.#socket.pause();
	}

	resume(): void {
		this.#paused = false;
		this.#socket.resume();
	}

	get isPaused(): boolean {
		return this.#paused;
	}

	/** feed bytes received with the upgrade request */
	_push(head: Buffer): void {
		if (head.length) this.#onData(head);
	}
}
