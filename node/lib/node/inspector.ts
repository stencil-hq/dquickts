import { EventEmitter } from "node:events";
import { codedError } from "internal:errors";
export class Session extends EventEmitter {
	private connected = false;
	connect(): void {
		if (this.connected)
			throw codedError("ERR_INSPECTOR_ALREADY_CONNECTED", "The inspector session is already connected");
		this.connected = true;
	}
	connectToMainThread(): void {
		this.connect();
	}
	disconnect(): void {
		this.connected = false;
	}
	post(
		_method: string,
		params?: object | ((error: Error, result?: object) => void),
		callback?: (error: Error, result?: object) => void,
	): void {
		if (!this.connected) throw codedError("ERR_INSPECTOR_NOT_CONNECTED", "Session is not connected");
		const cb = typeof params === "function" ? params : callback;
		const error = codedError("ERR_INSPECTOR_NOT_AVAILABLE", "Inspector is not available");
		if (cb) queueMicrotask(() => cb(error));
	}
}
export function open(): void {}
export function close(): void {}
export function url(): undefined {
	return undefined;
}
export function waitForDebugger(): never {
	throw codedError("ERR_INSPECTOR_NOT_ACTIVE", "Inspector is not active");
}
export const console = globalThis.console;
export default { Session, open, close, url, waitForDebugger, console };
