// Standard streams are constructed only when process.stdin/stdout/stderr is read.
import * as core from "qbun:core";
import * as native from "qbun:net";
import { Writable } from "node:stream";
import { Socket } from "node:net";
import { ReadStream, WriteStream } from "node:tty";
import { ReadStream as FileReadStream } from "node:fs";
import { invalidArgType } from "internal:errors";

type Callback = (error?: Error | null) => void;
class SyncWriteStream extends Writable {
	fd: number;
	isTTY = false;
	_isStdio = true;
	_handle = {
		setBlocking(_value: boolean): void {},
		ref(): void {},
		unref(): void {},
		hasRef(): boolean {
			return false;
		},
	};
	constructor(fd: number) {
		super({ autoDestroy: false });
		this.fd = fd;
	}
	write(chunk: string | Uint8Array, encodingOrCallback?: string | Callback, callback?: Callback): true {
		const done = typeof encodingOrCallback === "function" ? encodingOrCallback : callback;
		if (done !== undefined && typeof done !== "function") throw invalidArgType("callback", "of type function", done);
		if (typeof chunk !== "string" && !(chunk instanceof Uint8Array))
			throw invalidArgType("chunk", "of type string or an instance of Buffer or Uint8Array", chunk);
		const bytes =
			typeof chunk === "string" && typeof encodingOrCallback === "string"
				? Buffer.from(chunk, encodingOrCallback)
				: chunk;
		try {
			core.writeSync(this.fd, bytes);
		} catch (error) {
			process.nextTick(() => {
				if (done) done(error as Error);
				this.emit("error", error);
			});
			return true;
		}
		if (done) process.nextTick(done);
		return true;
	}
	_write(chunk: Uint8Array, _encoding: string, callback: Callback): void {
		try {
			core.writeSync(this.fd, chunk);
			callback();
		} catch (error) {
			callback(error as Error);
		}
	}
	_destroy(error: Error | null, callback: Callback): void {
		callback(error);
	}
	destroy(error?: Error): this {
		if (error) process.nextTick(() => this.emit("error", error));
		return this;
	}
	ref(): this {
		return this;
	}
	unref(): this {
		return this;
	}
	destroySoon(): void {
		this.end();
	}
}
let stdin: Socket | ReadStream | FileReadStream | undefined;
let stdout: WriteStream | SyncWriteStream | undefined;
let stderr: WriteStream | SyncWriteStream | undefined;
function output(fd: number): WriteStream | SyncWriteStream {
	const stream = native.guessHandleType(fd) === "TTY" ? new WriteStream(fd) : new SyncWriteStream(fd);
	// process output owns inherited descriptors; user destruction must not close them.
	if (stream instanceof WriteStream) {
		stream.destroy = function (error?: Error): typeof stream {
			if (error) process.nextTick(() => this.emit("error", error));
			return this;
		};
	}
	return stream;
}
export function getStdout(): WriteStream | SyncWriteStream {
	return (stdout ??= output(1));
}
export function getStderr(): WriteStream | SyncWriteStream {
	return (stderr ??= output(2));
}
export function getStdin(): Socket | ReadStream | FileReadStream {
	if (stdin) return stdin;
	const kind = native.guessHandleType(0);
	if (kind === "TTY") stdin = new ReadStream(0);
	else if (kind === "FILE") {
		stdin = new FileReadStream(null, { fd: 0, autoClose: false });
	} else {
		const handle = native.createPipe();
		handle.open(0);
		stdin = new Socket({ handle, readable: true, writable: false, manualStart: true });
		const pause = stdin.pause,
			resume = stdin.resume;
		let referenced = true;
		stdin.pause = function (): Socket {
			pause.call(this);
			handle.reading = false;
			handle.readStop();
			handle.unref();
			return this;
		};
		stdin.resume = function (): Socket {
			resume.call(this);
			if (!this.destroyed && !handle.reading) {
				try {
					handle.readStart();
					handle.reading = true;
				} catch (error) {
					process.nextTick(() => this.emit("error", error));
				}
			}
			if (referenced) handle.ref();
			return this;
		};
		stdin.ref = function (): Socket {
			referenced = true;
			if (!this.isPaused()) handle.ref();
			return this;
		};
		stdin.unref = function (): Socket {
			referenced = false;
			handle.unref();
			return this;
		};
	}
	Object.defineProperty(stdin, "fd", { value: 0, configurable: true, enumerable: true });
	if (kind !== "TTY") Object.defineProperty(stdin, "isTTY", { value: false, configurable: true, enumerable: true });
	// a pipe stays idle (and does not keep the process alive) until read;
	// not pause(), which would stop a later 'data' listener from flowing
	if (kind !== "TTY" && kind !== "FILE") {
		const handle = (stdin as Socket & { _handle: { reading: boolean; readStop(): void; unref(): void } })._handle;
		handle.reading = false;
		handle.readStop();
		handle.unref();
	}
	return stdin;
}
