import { EventEmitter } from "node:events";
import { notSupported, codedError } from "internal:errors";
export class Socket extends EventEmitter {
	type: string;
	constructor(type: string | { type: string }, listener?: (...args: unknown[]) => void) {
		super();
		this.type = typeof type === "string" ? type : type.type;
		if (listener) this.on("message", listener);
	}
	bind(..._args: unknown[]): this {
		queueMicrotask(() => this.emit("error", notSupported("UDP sockets")));
		return this;
	}
	send(...args: unknown[]): void {
		const cb = args[args.length - 1];
		const err = notSupported("UDP sockets");
		queueMicrotask(() => {
			if (typeof cb === "function") cb(err);
			else this.emit("error", err);
		});
	}
	close(callback?: () => void): this {
		queueMicrotask(() => {
			this.emit("close");
			callback?.();
		});
		return this;
	}
	address(): never {
		throw codedError("ERR_SOCKET_DGRAM_NOT_RUNNING", "Not running");
	}
	connect(): never {
		throw notSupported("UDP sockets");
	}
	disconnect(): never {
		throw notSupported("UDP sockets");
	}
	remoteAddress(): never {
		return this.address();
	}
	ref(): this {
		return this;
	}
	unref(): this {
		return this;
	}
	setBroadcast(): this {
		return this;
	}
	setTTL(): this {
		return this;
	}
	setMulticastTTL(): this {
		return this;
	}
	setMulticastLoopback(): this {
		return this;
	}
	setMulticastInterface(): this {
		return this;
	}
	addMembership(): never {
		throw notSupported("UDP multicast");
	}
	dropMembership(): never {
		throw notSupported("UDP multicast");
	}
	addSourceSpecificMembership(): never {
		throw notSupported("UDP multicast");
	}
	dropSourceSpecificMembership(): never {
		throw notSupported("UDP multicast");
	}
	getRecvBufferSize(): number {
		return 0;
	}
	getSendBufferSize(): number {
		return 0;
	}
	setRecvBufferSize(): void {}
	setSendBufferSize(): void {}
	getSendQueueSize(): number {
		return 0;
	}
	getSendQueueCount(): number {
		return 0;
	}
}
export function createSocket(type: string | { type: string }, listener?: (...args: unknown[]) => void): Socket {
	return new Socket(type, listener);
}
export default { Socket, createSocket };
