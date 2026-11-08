// internal:child/ipc — the IPC channel of child_process.fork(), spawn() with
// an 'ipc' stdio entry and Bun.spawn({ ipc }), over a qbun:net IPC pipe.
//
// Wire formats (Node's internal/child_process/serialization):
// - "json": newline-delimited JSON;
// - "advanced": structured-clone serialized messages, each prefixed with its
//   length as a big-endian uint32 (qbun's serializer, so both ends must be qbun).
//
// Sending handles (sockets/servers) over the channel is not supported.
//
// No static import of node:process: setupChildChannel() runs while node:process
// itself is being evaluated.

import * as core from "qbun:core";
import * as netBinding from "qbun:net";
import { codedError, invalidArgType, notSupported } from "internal:errors";
import { bind } from "internal:async-context";
import { serialize, deserialize } from "qbun:worker";

export type Serialization = "json" | "advanced";

interface StreamHandle {
	readonly fd: number;
	onread: ((chunk: Uint8Array | null, err?: Error) => void) | null;
	reading?: boolean;
	readStart(): void;
	readStop(): void;
	write(data: Uint8Array | string, cb: (err: Error | null) => void): void;
	readonly writeQueueSize: number;
	close(cb?: () => void): void;
	ref(): void;
	unref(): void;
	hasRef(): boolean;
	open(fd: number): void;
}

const INTERNAL_PREFIX = "NODE_";

/** Node's isInternal(): `{ cmd: "NODE_..." }` control messages. */
export function isInternalMessage(message: unknown): boolean {
	if (message === null || typeof message !== "object") return false;
	const cmd = (message as { cmd?: unknown }).cmd;
	return typeof cmd === "string" && cmd.length > INTERNAL_PREFIX.length && cmd.startsWith(INTERNAL_PREFIX);
}

export interface ChannelHooks {
	/** a decoded message (`internal`: a NODE_* control message) */
	onMessage(message: unknown, internal: boolean): void;
	/** the channel is gone (disconnect() or the peer closed it); called once */
	onDisconnect(): void;
	/** a send failure with no callback to report it to */
	onError(err: Error): void;
}

export type SendCallback = (err: Error | null) => void;

/**
 * One end of an IPC channel. `connected` turns false as soon as disconnect()
 * is requested; the pipe closes once queued writes are flushed.
 */
export class Channel {
	connected = true;
	#handle: StreamHandle | null;
	#hooks: ChannelHooks;
	#serialization: Serialization;
	#pendingWrites = 0;
	#closing = false;
	#refs = 0;
	#refExplicitlySet = false;
	// json decoding state
	#jsonBuffer = "";
	#decoder: TextDecoder | undefined;
	// advanced decoding state
	#chunks: Uint8Array[] = [];
	#chunksSize = 0;

	constructor(handle: StreamHandle, serialization: Serialization, hooks: ChannelHooks) {
		this.#handle = handle;
		this.#serialization = serialization;
		this.#hooks = hooks;
		handle.onread = bind((chunk: Uint8Array | null) => this.#onRead(chunk));
		handle.reading = true;
		handle.readStart();
	}

	get fd(): number | undefined {
		return this.#handle ? this.#handle.fd : undefined;
	}

	#onRead(chunk: Uint8Array | null): void {
		if (chunk) {
			let messages: unknown[];
			try {
				messages = this.#serialization === "json" ? this.#parseJSON(chunk) : this.#parseAdvanced(chunk);
			} catch (e) {
				// a malformed frame from the peer is a process-level failure, like in Node/Bun
				process.nextTick(() => {
					throw e;
				});
				return;
			}
			for (const message of messages) this.#hooks.onMessage(message, isInternalMessage(message));
			return;
		}
		// EOF or read error: the peer is gone
		this.connected = false;
		this.#finish();
	}

	#parseJSON(chunk: Uint8Array): unknown[] {
		this.#decoder ??= new TextDecoder("utf-8");
		const text = this.#decoder.decode(chunk, { stream: true });
		const lines = text.split("\n");
		const complete = lines.length - 1;
		if (complete === 0) {
			this.#jsonBuffer += lines[0];
			return [];
		}
		lines[0] = this.#jsonBuffer + lines[0];
		this.#jsonBuffer = lines[complete];
		const out: unknown[] = [];
		for (let i = 0; i < complete; i++) out.push(JSON.parse(lines[i]));
		return out;
	}

	#parseAdvanced(chunk: Uint8Array): unknown[] {
		if (chunk.length === 0) return [];
		this.#chunks.push(chunk);
		this.#chunksSize += chunk.length;
		const out: unknown[] = [];
		while (this.#chunksSize >= 4) {
			let head = this.#chunks[0];
			if (head.length < 4) {
				head = this.#flatten();
			}
			const size = ((head[0] << 24) | (head[1] << 16) | (head[2] << 8) | head[3]) >>> 0;
			const full = size + 4;
			if (this.#chunksSize < full) break;
			const all = this.#chunks.length === 1 ? this.#chunks[0] : this.#flatten();
			out.push(deserialize(all.subarray(4, full)));
			const rest = all.subarray(full);
			this.#chunks = rest.length ? [rest] : [];
			this.#chunksSize = rest.length;
		}
		return out;
	}

	#flatten(): Uint8Array {
		const all = new Uint8Array(this.#chunksSize);
		let offset = 0;
		for (const c of this.#chunks) {
			all.set(c, offset);
			offset += c.length;
		}
		this.#chunks = [all];
		return all;
	}

	#encode(message: unknown): Uint8Array | string {
		if (this.#serialization === "json") return `${JSON.stringify(message)}\n`;
		const body = serialize(message);
		const frame = new Uint8Array(body.length + 4);
		const n = body.length;
		frame[0] = (n >>> 24) & 0xff;
		frame[1] = (n >>> 16) & 0xff;
		frame[2] = (n >>> 8) & 0xff;
		frame[3] = n & 0xff;
		frame.set(body, 4);
		return frame;
	}

	/**
	 * Node's process.send()/subprocess.send(): false when the peer is behind
	 * (backpressure). Throws for unserializable messages.
	 */
	send(message: unknown, handle?: unknown, options?: unknown, callback?: SendCallback): boolean {
		if (typeof handle === "function") {
			callback = handle as SendCallback;
			handle = undefined;
			options = undefined;
		} else if (typeof options === "function") {
			callback = options as SendCallback;
			options = undefined;
		} else if (options !== undefined && (options === null || typeof options !== "object")) {
			throw invalidArgType("options", "of type object", options);
		}
		if (callback !== undefined && typeof callback !== "function")
			throw invalidArgType("callback", "of type function", callback);
		const swallowErrors = (options as { swallowErrors?: boolean } | undefined)?.swallowErrors === true;
		if (!this.connected || !this.#handle) {
			const ex = codedError("ERR_IPC_CHANNEL_CLOSED", "Channel closed");
			if (callback) process.nextTick(callback, ex);
			else process.nextTick(() => this.#hooks.onError(ex));
			return false;
		}
		return this.sendInternal(message, handle, swallowErrors, callback);
	}

	/** send() without the connected check (NODE_* control messages during a disconnect). */
	sendInternal(message: unknown, handle: unknown, swallowErrors: boolean, callback?: SendCallback): boolean {
		if (message === undefined)
			throw codedError("ERR_MISSING_ARGS", 'The "message" argument must be specified', TypeError);
		if (
			typeof message !== "string" &&
			typeof message !== "object" &&
			typeof message !== "number" &&
			typeof message !== "boolean"
		) {
			throw invalidArgType("message", "one of type string, object, number, or boolean", message);
		}
		if (handle !== undefined && handle !== null) throw notSupported("Sending handles over an IPC channel");
		const h = this.#handle;
		if (!h) return false;
		const data = this.#encode(message);
		this.#pendingWrites++;
		this.refCounted();
		h.write(data, (err) => {
			this.#pendingWrites--;
			this.unrefCounted();
			if (err) {
				if (!swallowErrors) {
					if (callback) callback(err);
					else this.#hooks.onError(err);
				}
			} else if (callback) {
				callback(null);
			}
			if (this.#closing && this.#pendingWrites === 0) this.#closeHandle();
		});
		return h.writeQueueSize < 65536 * 2;
	}

	/** Stop sending; the channel closes once queued writes are flushed. */
	disconnect(): void {
		this.connected = false;
		this.#finish();
	}

	#finish(): void {
		if (this.#closing) return;
		this.#closing = true;
		const h = this.#handle;
		if (h) {
			h.readStop();
			h.reading = false;
			h.onread = null;
		}
		if (this.#pendingWrites === 0) this.#closeHandle();
		process.nextTick(() => this.#hooks.onDisconnect());
	}

	#closeHandle(): void {
		const h = this.#handle;
		if (!h) return;
		this.#handle = null;
		h.close();
	}

	// Node's Control: refs track 'message' listeners and pending writes until
	// the user calls ref()/unref() explicitly.
	refCounted(): void {
		if (++this.#refs === 1 && !this.#refExplicitlySet) this.#handle?.ref();
	}

	unrefCounted(): void {
		if (--this.#refs === 0 && !this.#refExplicitlySet) this.#handle?.unref();
	}

	ref(): void {
		this.#refExplicitlySet = true;
		this.#handle?.ref();
	}

	unref(): void {
		this.#refExplicitlySet = true;
		this.#handle?.unref();
	}

	hasRef(): boolean {
		return this.#handle ? this.#handle.hasRef() : false;
	}
}

interface NetNative {
	createPipe(ipc?: boolean): StreamHandle;
}

/** Open an IPC pipe end over `fd`. */
export function openChannelHandle(fd: number): StreamHandle {
	const handle = (netBinding as unknown as NetNative).createPipe(true);
	handle.open(fd);
	return handle;
}

export function validateSerialization(value: unknown, name = "options.serialization"): Serialization {
	if (value === undefined) return "json";
	if (value === "json" || value === "advanced") return value;
	throw codedError(
		"ERR_INVALID_ARG_VALUE",
		`The property '${name}' must be one of: undefined, 'json', 'advanced'. Received ${typeof value === "string" ? `'${value}'` : String(value)}`,
		TypeError,
	);
}

interface EmitterTarget {
	emit(event: string | symbol, ...args: unknown[]): boolean;
	on(event: string | symbol, listener: (...args: unknown[]) => void): unknown;
	listenerCount(event: string | symbol): number;
}

export interface NodeChannelTarget extends EmitterTarget {
	channel: Channel | null | undefined;
	connected: boolean | undefined;
	send: unknown;
	disconnect: unknown;
	_send?: unknown;
	_disconnect?: unknown;
}

/**
 * Node's setupChannel(): wire `channel` to an EventEmitter (a ChildProcess or
 * the process object): send(), disconnect(), connected, channel,
 * 'message'/'internalMessage'/'disconnect'/'error' events. Messages that
 * arrive before a 'message' listener exists are queued.
 */
export function setupNodeChannel(
	target: NodeChannelTarget,
	handle: StreamHandle,
	serialization: Serialization,
	onClosed?: () => void,
): Channel {
	const pending: [string, unknown][] = [];
	const flushPending = (): void => {
		if (!target.channel || target.listenerCount("message") === 0 || pending.length === 0) return;
		const queued = pending.splice(0, pending.length);
		for (const [event, message] of queued) target.emit(event, message);
	};
	const channel = new Channel(handle, serialization, {
		onMessage(message, internal) {
			const event = internal ? "internalMessage" : "message";
			process.nextTick(() => {
				if (!target.channel) return;
				if (internal || target.listenerCount("message") > 0) target.emit(event, message);
				else pending.push([event, message]);
			});
		},
		onDisconnect() {
			target.connected = false;
			target.channel = null;
			target.emit("disconnect");
			onClosed?.();
		},
		onError(err) {
			target.emit("error", err);
		},
	});
	target.channel = channel;
	target.connected = true;
	target.on("newListener", () => process.nextTick(flushPending));
	target.send = function send(
		message: unknown,
		sendHandle?: unknown,
		options?: unknown,
		callback?: SendCallback,
	): boolean {
		return channel.send(message, sendHandle, options, callback);
	};
	target.disconnect = function disconnect(): void {
		if (!channel.connected) {
			target.emit("error", codedError("ERR_IPC_DISCONNECTED", "IPC channel is already disconnected"));
			return;
		}
		target.connected = false;
		channel.disconnect();
	};
	return channel;
}

/**
 * Child side (process.send/process.on("message")): adopt the fd named by
 * NODE_CHANNEL_FD. Called by node:process at startup when the variable is set.
 * The channel keeps the process alive only while 'message' or 'disconnect'
 * listeners exist (like Node).
 */
export function setupChildChannel(target: NodeChannelTarget & { env?: Record<string, string | undefined> }): void {
	const env = target.env ?? (core.environ() as Record<string, string | undefined>);
	const fdText = env.NODE_CHANNEL_FD;
	if (fdText === undefined) return;
	const mode = env.NODE_CHANNEL_SERIALIZATION_MODE;
	delete env.NODE_CHANNEL_FD;
	delete env.NODE_CHANNEL_SERIALIZATION_MODE;
	const fd = Number.parseInt(fdText, 10);
	if (!Number.isInteger(fd) || fd < 0) return;
	_forkChild(target, fd, mode === "advanced" ? "advanced" : "json");
}

/** Node's child_process._forkChild(fd, serializationMode) on `target` (the process object). */
export function _forkChild(target: NodeChannelTarget, fd: number, serialization: Serialization): void {
	const handle = openChannelHandle(fd);
	handle.unref();
	const channel = setupNodeChannel(target, handle, serialization);
	// listeners that existed before (none at startup, normally)
	const initial = target.listenerCount("message") + target.listenerCount("disconnect");
	for (let i = 0; i < initial; i++) channel.refCounted();
	target.on("newListener", (name: unknown) => {
		if (name === "message" || name === "disconnect") channel.refCounted();
	});
	target.on("removeListener", (name: unknown) => {
		if (name === "message" || name === "disconnect") channel.unrefCounted();
	});
}
