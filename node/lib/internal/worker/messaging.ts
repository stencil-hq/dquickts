// Message ports over the native channels of qbun:worker: MessagePort (an
// EventEmitter with the EventTarget methods, like Node's), MessageChannel,
// BroadcastChannel, receiveMessageOnPort and the transfer marks.
//
// A message is written natively with the HTML structured clone algorithm of
// the engine (JS_StructuredSerialize) and the clone host of qbun:worker:
// Blob and File objects are kept, the transferred MessagePorts are written
// as indexes and rebuilt on the other side with createPort()
// (setCloneHooks below), SharedArrayBuffers are shared.

import { EventEmitter } from "node:events";
import { Event, EventTarget, MessageEvent } from "internal:event-target";
import { codedError, invalidArgType } from "internal:errors";
import * as native from "qbun:worker";

export type HandlerFn = (type: number, value?: unknown, ports?: (MessagePort | null)[], kind?: number) => void;

/** The native port (qbun:worker MessagePortHandle). */
export interface PortHandle {
	/**
	 * Post `value` with the transfer list `transfer` whose MessagePorts are
	 * `ports` (their handles: `handles`). Returns the number of endpoints reached.
	 */
	postMessage(value: unknown, transfer: unknown[], ports: MessagePort[], handles: PortHandle[], kind: number): number;
	/** [value, the transferred MessagePorts, kind] */
	receive(): [unknown, (MessagePort | null)[] | undefined, number] | undefined;
	start(): void;
	stop(): void;
	ref(): void;
	unref(): void;
	hasRef(): boolean;
	close(): void;
	setHandler(fn: HandlerFn | null): void;
	isClosed(): boolean;
}

interface WorkerNative {
	createChannel(): [PortHandle, PortHandle];
	openBroadcast(name: string): PortHandle;
	setCloneHooks(portClass: typeof MessagePort, createPort: (handle: PortHandle) => MessagePort): void;
	markUncloneable(object: object): void;
}
const nativeApi = native as unknown as WorkerNative;

/** Message kinds (bit flags, opaque to C). */
export const KIND_CONTROL = 2;

const MSG_TYPE = 0,
	MSGERROR_TYPE = 1,
	CLOSE_TYPE = 2;

export function dataCloneError(message: string): DOMException {
	return new DOMException(message, "DataCloneError");
}

// ---------------------------------------------------------------- transfer marks

const kUntransferable = Symbol("qbun.untransferable");
// marks of frozen/sealed objects, which cannot take the symbol properties
const frozenMarks = new WeakMap<object, Set<symbol>>();

function mark(object: object, key: symbol): void {
	if (Object.isExtensible(object)) {
		Object.defineProperty(object, key, { value: true, configurable: true, enumerable: false, writable: false });
		return;
	}
	let set = frozenMarks.get(object);
	if (!set) frozenMarks.set(object, (set = new Set()));
	set.add(key);
}

function hasMark(object: object, key: symbol): boolean {
	return Object.hasOwn(object, key) || (frozenMarks.get(object)?.has(key) ?? false);
}

/** Mark an object as not transferable: listing it in a transfer list throws. */
export function markAsUntransferable(object: unknown): void {
	if ((typeof object !== "object" && typeof object !== "function") || object === null) return;
	mark(object, kUntransferable);
	if (ArrayBuffer.isView(object)) mark(object.buffer, kUntransferable);
}

export function isMarkedAsUntransferable(object: unknown): boolean {
	if ((typeof object !== "object" && typeof object !== "function") || object === null) return false;
	return hasMark(object, kUntransferable);
}

/** Mark an object as not cloneable: serializing it (postMessage, structuredClone) throws a DataCloneError. */
export function markAsUncloneable(object: unknown): void {
	if ((typeof object !== "object" && typeof object !== "function") || object === null) return;
	nativeApi.markUncloneable(object);
}

// ---------------------------------------------------------------- posting

function toTransferList(transfer: unknown): unknown[] {
	if (transfer === undefined || transfer === null) return [];
	if (Array.isArray(transfer)) return transfer;
	if (typeof transfer === "object") {
		const t = transfer as { transfer?: unknown; [Symbol.iterator]?: unknown };
		if (typeof t[Symbol.iterator] === "function") return [...(transfer as Iterable<unknown>)];
		if (t.transfer === undefined || t.transfer === null) return [];
		if (
			typeof t.transfer === "object" &&
			typeof (t.transfer as { [Symbol.iterator]?: unknown })[Symbol.iterator] === "function"
		)
			return [...(t.transfer as Iterable<unknown>)];
		throw invalidArgType("options.transfer", "an Array", t.transfer);
	}
	throw invalidArgType("transferList", "an Array", transfer);
}

/**
 * Post `value` on the native `handle`, transferring the ArrayBuffers and
 * MessagePorts of `transfer` (an array or `{ transfer }`). Returns the number
 * of endpoints reached.
 */
export function postOn(
	handle: PortHandle,
	source: object | null,
	value: unknown,
	transfer: unknown,
	control = false,
): number {
	const list = toTransferList(transfer);
	const portObjects: MessagePort[] = [];
	const portHandles: PortHandle[] = [];
	// the engine checks the ArrayBuffers (detached, duplicate...) and rejects
	// the other values; the MessagePorts are checked here
	for (const item of list) {
		if (item instanceof MessagePort) {
			if (portObjects.includes(item)) throw dataCloneError("Transfer list contains duplicate MessagePort");
			if (item === source) throw dataCloneError("Transfer list contains source port");
			if (hasMark(item, kUntransferable)) throw dataCloneError("MessagePort is marked as untransferable");
			const h = item[kHandle];
			if (!h) throw dataCloneError("MessagePort in transfer list is already detached");
			portObjects.push(item);
			portHandles.push(h);
		} else if (typeof item === "object" && item !== null && hasMark(item, kUntransferable)) {
			throw dataCloneError("An ArrayBuffer is marked as untransferable");
		}
	}
	const reached = handle.postMessage(value, list, portObjects, portHandles, control ? KIND_CONTROL : 0);
	for (const port of portObjects) port[kDetach]();
	return reached;
}

/** The value of a native message and its transferred MessagePorts. */
export function readMessage(
	value: unknown,
	ports: (MessagePort | null)[] | undefined,
	_kind: number,
): { data: unknown; ports: MessagePort[] } {
	return { data: value, ports: (ports ?? []).filter((p): p is MessagePort => p !== null) };
}

// ---------------------------------------------------------------- MessagePort

export const kHandle = Symbol("qbun.port.handle");
export const kDetach = Symbol("qbun.port.detach");
/** Control messages (KIND_CONTROL) of internal ports: (data, ports). */
export const kControl = Symbol("qbun.port.control");
/** Takes the user messages instead of the 'message' event (Worker internals). */
export const kDeliver = Symbol("qbun.port.deliver");
/** An extra receiver counting as a 'message' listener (worker global scope). */
export const kExtraListener = Symbol("qbun.port.extra");
/** Dispatch every queued message now (before a worker's 'exit'). */
export const kDrain = Symbol("qbun.port.drain");
/**
 * Keep the port started for its control messages: user messages without a
 * listener wait in a JS queue (a worker's parentPort).
 */
export const kKeepStarted = Symbol("qbun.port.keepStarted");
/** Take the oldest message of the kKeepStarted queue. */
export const kShiftQueued = Symbol("qbun.port.shift");
const kCreate = Symbol("qbun.port.create");

type Listener = (...args: unknown[]) => unknown;
type DomListener = ((this: unknown, event: Event) => unknown) | { handleEvent(event: Event): unknown };
interface DomOptions {
	once?: boolean;
	capture?: boolean;
	passive?: boolean;
	signal?: AbortSignal;
}
type Deliver = (data: unknown, ports: MessagePort[]) => void;

let closeTicks: (() => void)[] | null = null;
function emitSoon(fn: () => void): void {
	if (closeTicks) {
		closeTicks.push(fn);
		return;
	}
	closeTicks = [fn];
	setImmediate(() => {
		const list = closeTicks ?? [];
		closeTicks = null;
		for (const f of list) f();
	});
}

export class MessagePort extends EventEmitter {
	[kHandle]: PortHandle | null;
	[kControl]: Deliver | null = null;
	[kDeliver]: Deliver | null = null;
	#extra: Deliver | null = null;
	#listening = false;
	#closed = false;
	#event: Event | null = null;
	#dom = new Map<string, Map<DomListener, Listener>>();
	#onmessage: DomListener | null = null;
	#onmessageerror: DomListener | null = null;
	#queue: [unknown, MessagePort[]][] | null = null;
	#flushing = false;

	constructor(token?: symbol, handle?: PortHandle) {
		if (token !== kCreate || !handle)
			throw codedError("ERR_CONSTRUCT_CALL_INVALID", "Constructor cannot be called", TypeError);
		super();
		this[kHandle] = handle;
		handle.setHandler((type, value, ports, kind) => this.#onNative(type, value, ports, kind ?? 0));
	}

	#onNative(type: number, value: unknown, ports: (MessagePort | null)[] | undefined, kind: number): void {
		if (type === CLOSE_TYPE) {
			this.#finishClose();
			return;
		}
		if (type === MSGERROR_TYPE) {
			this.#dispatch("messageerror", value, []);
			return;
		}
		let message: { data: unknown; ports: MessagePort[] };
		try {
			message = readMessage(value, ports, kind);
		} catch (err) {
			this.#dispatch("messageerror", err, []);
			return;
		}
		if (kind & KIND_CONTROL) {
			this[kControl]?.(message.data, message.ports);
			return;
		}
		const deliver = this[kDeliver];
		if (deliver) {
			deliver(message.data, message.ports);
			return;
		}
		const queue = this.#queue;
		if (queue && (queue.length > 0 || !this.#listening)) {
			queue.push([message.data, message.ports]);
			if (this.#listening) this.#scheduleFlush();
			return;
		}
		this.#deliverUser(message.data, message.ports);
	}

	#deliverUser(data: unknown, ports: MessagePort[]): void {
		this.#dispatch("message", data, ports);
		this.#extra?.(data, ports);
	}

	#scheduleFlush(): void {
		if (this.#flushing) return;
		this.#flushing = true;
		setImmediate(() => {
			this.#flushing = false;
			const queue = this.#queue;
			while (queue && queue.length > 0 && this.#listening) {
				const [data, ports] = queue.shift()!;
				this.#deliverUser(data, ports);
			}
		});
	}

	[kKeepStarted](): void {
		this.#queue = [];
		this[kHandle]?.start();
	}

	[kShiftQueued](): [unknown, MessagePort[]] | undefined {
		return this.#queue?.shift();
	}

	#dispatch(type: "message" | "messageerror", data: unknown, ports: MessagePort[]): void {
		const event = new MessageEvent(type, { data, ports });
		const prev = this.#event;
		this.#event = event;
		try {
			this.emit(type, data);
		} finally {
			this.#event = prev;
		}
		if (type === "messageerror") this.#extraError?.(data);
	}

	#extraError: ((err: unknown) => void) | null = null;

	#finishClose(): void {
		if (this.#closed) return;
		this.#closed = true;
		this[kHandle] = null;
		this.#listening = false;
		this.emit("close");
	}

	[kDrain](): void {
		for (;;) {
			const h = this[kHandle];
			if (!h) return;
			let raw: [unknown, (MessagePort | null)[] | undefined, number] | undefined;
			try {
				raw = h.receive();
			} catch (err) {
				this.#dispatch("messageerror", err, []);
				continue;
			}
			if (!raw) return;
			this.#onNative(MSG_TYPE, raw[0], raw[1], raw[2]);
		}
	}

	[kDetach](): void {
		this[kHandle] = null;
		this.#closed = true;
		this.#listening = false;
	}

	/** Set the worker global scope receiver (counts as a listener). */
	[kExtraListener](deliver: Deliver | null, onError: ((err: unknown) => void) | null = null): void {
		this.#extra = deliver;
		this.#extraError = onError;
		this.#sync();
	}

	#sync(): void {
		const h = this[kHandle];
		if (!h) return;
		const want = this.listenerCount("message") > 0 || this.#extra !== null;
		if (want === this.#listening) return;
		this.#listening = want;
		if (want) {
			h.ref();
			h.start();
			if (this.#queue && this.#queue.length > 0) this.#scheduleFlush();
		} else {
			if (!this.#queue) h.stop();
			h.unref();
		}
	}

	override addListener(type: string | symbol, listener: Listener): this {
		super.addListener(type, listener);
		if (type === "message") this.#sync();
		return this;
	}
	override on(type: string | symbol, listener: Listener): this {
		super.on(type, listener);
		if (type === "message") this.#sync();
		return this;
	}
	override prependListener(type: string | symbol, listener: Listener): this {
		super.prependListener(type, listener);
		if (type === "message") this.#sync();
		return this;
	}
	override removeListener(type: string | symbol, listener: Listener): this {
		super.removeListener(type, listener);
		if (type === "message") this.#sync();
		return this;
	}
	override off(type: string | symbol, listener: Listener): this {
		return this.removeListener(type, listener);
	}
	override removeAllListeners(type?: string | symbol): this {
		if (arguments.length === 0) super.removeAllListeners();
		else super.removeAllListeners(type);
		this.#sync();
		return this;
	}

	postMessage(value: unknown, transfer?: unknown): void {
		if (arguments.length === 0)
			throw codedError("ERR_MISSING_ARGS", 'The "message" argument must be specified', TypeError);
		const h = this[kHandle];
		if (!h) return;
		postOn(h, this, value, transfer);
	}

	start(): void {
		this[kHandle]?.start();
	}

	close(): void {
		const h = this[kHandle];
		if (!h || this.#closed) return;
		h.close();
		this[kHandle] = null;
		emitSoon(() => this.#finishClose());
	}

	ref(): this {
		this[kHandle]?.ref();
		return this;
	}

	unref(): this {
		this[kHandle]?.unref();
		return this;
	}

	hasRef(): boolean {
		return this[kHandle]?.hasRef() ?? false;
	}

	// EventTarget interface (listeners get Event objects)
	addEventListener(type: string, listener: DomListener | null, options: boolean | DomOptions = {}): void {
		if (listener === null || listener === undefined) return;
		type = String(type);
		let map = this.#dom.get(type);
		if (!map) this.#dom.set(type, (map = new Map()));
		if (map.has(listener)) return;
		const opts = typeof options === "boolean" ? {} : (options ?? {});
		if (opts.signal?.aborted) return;
		const wrapper: Listener = (data?: unknown) => {
			if (opts.once) this.removeEventListener(type, listener);
			const event =
				this.#event ??
				(type === "message" || type === "messageerror" ? new MessageEvent(type, { data }) : new Event(type));
			if (typeof listener === "function") listener.call(this, event);
			else listener.handleEvent(event);
		};
		map.set(listener, wrapper);
		this.on(type, wrapper);
		opts.signal?.addEventListener("abort", () => this.removeEventListener(type, listener), { once: true });
	}

	removeEventListener(type: string, listener: DomListener | null): void {
		if (!listener) return;
		type = String(type);
		const map = this.#dom.get(type);
		const wrapper = map?.get(listener);
		if (!map || !wrapper) return;
		map.delete(listener);
		if (map.size === 0) this.#dom.delete(type);
		this.removeListener(type, wrapper);
	}

	dispatchEvent(event: Event): boolean {
		const prev = this.#event;
		this.#event = event;
		try {
			this.emit(event.type, (event as MessageEvent).data);
		} finally {
			this.#event = prev;
		}
		return !event.defaultPrevented;
	}

	get onmessage(): DomListener | null {
		return this.#onmessage;
	}
	set onmessage(fn: DomListener | null) {
		if (this.#onmessage) this.removeEventListener("message", this.#onmessage);
		this.#onmessage = typeof fn === "function" || (fn && typeof fn === "object") ? fn : null;
		if (this.#onmessage) this.addEventListener("message", this.#onmessage);
	}

	get onmessageerror(): DomListener | null {
		return this.#onmessageerror;
	}
	set onmessageerror(fn: DomListener | null) {
		if (this.#onmessageerror) this.removeEventListener("messageerror", this.#onmessageerror);
		this.#onmessageerror = typeof fn === "function" || (fn && typeof fn === "object") ? fn : null;
		if (this.#onmessageerror) this.addEventListener("messageerror", this.#onmessageerror);
	}

	get [Symbol.toStringTag](): string {
		return "MessagePort";
	}
}

/** Wrap a native port handle of this runtime. */
export function createPort(handle: PortHandle): MessagePort {
	return new MessagePort(kCreate, handle);
}

// the clone host writes the MessagePorts of a transfer list as indexes and
// wraps the received handles with createPort()
nativeApi.setCloneHooks(MessagePort, createPort);

export class MessageChannel {
	readonly port1: MessagePort;
	readonly port2: MessagePort;
	constructor() {
		const [a, b] = nativeApi.createChannel();
		this.port1 = createPort(a);
		this.port2 = createPort(b);
	}
	get [Symbol.toStringTag](): string {
		return "MessageChannel";
	}
}

/** Dequeue one message of `port` synchronously: `{ message }` or undefined. */
export function receiveMessageOnPort(port: MessagePort): { message: unknown } | undefined {
	if (!(port instanceof MessagePort)) throw invalidArgType("port", "an instance of MessagePort", port);
	const queued = port[kShiftQueued]();
	if (queued) return { message: queued[0] };
	const h = port[kHandle];
	if (!h) return undefined;
	for (;;) {
		const raw = h.receive();
		if (!raw) return undefined;
		const message = readMessage(raw[0], raw[1], raw[2]);
		if (raw[2] & KIND_CONTROL) {
			port[kControl]?.(message.data, message.ports);
			continue;
		}
		return { message: message.data };
	}
}

export function moveMessagePortToContext(port: unknown): never {
	if (!(port instanceof MessagePort)) throw invalidArgType("port", "an instance of MessagePort", port);
	throw codedError("ERR_NOT_SUPPORTED", "moveMessagePortToContext() is not supported by qbun");
}

// ---------------------------------------------------------------- BroadcastChannel

export class BroadcastChannel extends EventTarget {
	readonly name: string;
	#handle: PortHandle | null;
	#refed = true;
	#listening = false;
	#onmessage: ((event: MessageEvent) => unknown) | null = null;
	#onmessageerror: ((event: MessageEvent) => unknown) | null = null;

	constructor(name: unknown) {
		if (arguments.length === 0)
			throw codedError("ERR_MISSING_ARGS", 'The "name" argument must be specified', TypeError);
		super();
		this.name = `${name}`;
		this.#handle = nativeApi.openBroadcast(this.name);
		this.#handle.setHandler((type, value, ports, kind) => {
			if (type === CLOSE_TYPE) return;
			if (type === MSGERROR_TYPE) {
				this.dispatchEvent(new MessageEvent("messageerror", { data: value }));
				return;
			}
			let data: unknown;
			try {
				data = readMessage(value, ports, kind ?? 0).data;
			} catch (err) {
				this.dispatchEvent(new MessageEvent("messageerror", { data: err }));
				return;
			}
			this.dispatchEvent(new MessageEvent("message", { data }));
		});
	}

	#sync(): void {
		const h = this.#handle;
		if (!h) return;
		const listeners = (this as unknown as Record<symbol, (type: string) => unknown[]>)[
			Symbol.for("qbun.eventTargetListeners")
		]("message");
		const want = listeners.length > 0;
		if (want !== this.#listening) {
			this.#listening = want;
			if (want) h.start();
			else h.stop();
		}
		if (this.#listening && this.#refed) h.ref();
		else h.unref();
	}

	override addEventListener(
		type: string,
		listener: Parameters<EventTarget["addEventListener"]>[1],
		options?: Parameters<EventTarget["addEventListener"]>[2],
	): void {
		super.addEventListener(type, listener, options);
		if (String(type) === "message") this.#sync();
	}

	override removeEventListener(
		type: string,
		listener: Parameters<EventTarget["removeEventListener"]>[1],
		options?: Parameters<EventTarget["removeEventListener"]>[2],
	): void {
		super.removeEventListener(type, listener, options);
		if (String(type) === "message") this.#sync();
	}

	postMessage(message: unknown): void {
		if (arguments.length === 0)
			throw codedError("ERR_MISSING_ARGS", 'The "message" argument must be specified', TypeError);
		const h = this.#handle;
		if (!h) throw new DOMException("BroadcastChannel is closed.", "InvalidStateError");
		postOn(h, null, message, undefined);
	}

	close(): void {
		const h = this.#handle;
		if (!h) return;
		this.#handle = null;
		h.close();
	}

	ref(): this {
		this.#refed = true;
		this.#sync();
		return this;
	}

	unref(): this {
		this.#refed = false;
		this.#sync();
		return this;
	}

	get onmessage(): ((event: MessageEvent) => unknown) | null {
		return this.#onmessage;
	}
	set onmessage(fn: ((event: MessageEvent) => unknown) | null) {
		if (this.#onmessage)
			this.removeEventListener(
				"message",
				this.#onmessage as unknown as Parameters<EventTarget["removeEventListener"]>[1],
			);
		this.#onmessage = typeof fn === "function" ? fn : null;
		if (this.#onmessage)
			this.addEventListener("message", this.#onmessage as unknown as Parameters<EventTarget["addEventListener"]>[1]);
	}

	get onmessageerror(): ((event: MessageEvent) => unknown) | null {
		return this.#onmessageerror;
	}
	set onmessageerror(fn: ((event: MessageEvent) => unknown) | null) {
		if (this.#onmessageerror)
			this.removeEventListener(
				"messageerror",
				this.#onmessageerror as unknown as Parameters<EventTarget["removeEventListener"]>[1],
			);
		this.#onmessageerror = typeof fn === "function" ? fn : null;
		if (this.#onmessageerror)
			this.addEventListener(
				"messageerror",
				this.#onmessageerror as unknown as Parameters<EventTarget["addEventListener"]>[1],
			);
	}

	get [Symbol.toStringTag](): string {
		return "BroadcastChannel";
	}
}
