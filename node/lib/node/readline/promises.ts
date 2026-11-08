// node:readline/promises — promise questions and transactional terminal actions.
import {
	Interface as CallbackInterface,
	type Input,
	type Output,
	type ReadLineOptions,
	type QuestionOptions,
	type Completer,
	type WriteCallback,
} from "node:readline";
import { abortError, codedError, invalidArgType, outOfRange } from "internal:errors";
import process from "node:process";

export class Interface extends CallbackInterface {
	question(
		query: string,
		options: QuestionOptions | ((answer: string) => void) = {},
		callback?: (answer: string) => void,
	): Promise<string> {
		const deferred = Promise.withResolvers<string>();
		const signal = typeof options === "object" ? options.signal : undefined;
		if (this.closed) {
			deferred.reject(codedError("ERR_USE_AFTER_CLOSE", "readline was closed"));
			return deferred.promise;
		}
		if (signal?.aborted) {
			deferred.reject(abortError(signal.reason));
			return deferred.promise;
		}
		const cleanup = (): void => {
			signal?.removeEventListener("abort", cancel);
			this.off("close", onClose);
		};
		const cancel = (): void => {
			this._cancelQuestion();
			cleanup();
			deferred.reject(abortError(signal?.reason));
		};
		const onClose = (): void => {
			cleanup();
			deferred.reject(abortError());
		};
		signal?.addEventListener("abort", cancel, { once: true });
		this.once("close", onClose);
		try {
			super.question(query, typeof options === "object" ? options : {}, (answer) => {
				cleanup();
				if (typeof options === "function") options(answer);
				callback?.(answer);
				deferred.resolve(answer);
			});
		} catch (error) {
			cleanup();
			deferred.reject(error);
		}
		return deferred.promise;
	}
}
export function createInterface(
	input: Input | ReadLineOptions,
	output?: Output,
	completer?: Completer,
	terminal?: boolean,
): Interface {
	return new Interface(input, output, completer, terminal);
}
function integer(value: number, name: string, min = Number.MIN_SAFE_INTEGER, max = Number.MAX_SAFE_INTEGER): void {
	if (typeof value !== "number") throw invalidArgType(name, "of type number", value);
	if (!Number.isInteger(value) || value < min || value > max)
		throw outOfRange(name, `an integer >= ${min} && <= ${max}`, value);
}
export class Readline {
	private _stream: Output;
	private _autoCommit: boolean;
	private _pending: string[] = [];
	constructor(stream: Output, options: { autoCommit?: boolean } = {}) {
		if (!stream || typeof stream.write !== "function") throw invalidArgType("stream", "a Writable stream", stream);
		if (options.autoCommit !== undefined && typeof options.autoCommit !== "boolean")
			throw invalidArgType("options.autoCommit", "of type boolean", options.autoCommit);
		this._stream = stream;
		this._autoCommit = options.autoCommit ?? false;
	}
	private _enqueue(action: string): this {
		if (this._autoCommit) process.nextTick(() => this._stream.write(action));
		else this._pending.push(action);
		return this;
	}
	cursorTo(x: number, y?: number): this {
		integer(x, "x");
		if (y !== undefined) integer(y, "y");
		return this._enqueue(y === undefined ? `\x1b[${x + 1}G` : `\x1b[${y + 1};${x + 1}H`);
	}
	moveCursor(dx: number, dy: number): this {
		if (!(dx || dy)) return this;
		integer(dx, "dx");
		integer(dy, "dy");
		return this._enqueue(
			(dx < 0 ? `\x1b[${-dx}D` : dx > 0 ? `\x1b[${dx}C` : "") + (dy < 0 ? `\x1b[${-dy}A` : dy > 0 ? `\x1b[${dy}B` : ""),
		);
	}
	clearLine(direction: number): this {
		integer(direction, "dir", -1, 1);
		return this._enqueue(`\x1b[${direction < 0 ? 1 : direction > 0 ? 0 : 2}K`);
	}
	clearScreenDown(): this {
		return this._enqueue("\x1b[0J");
	}
	commit(): Promise<void> {
		const deferred = Promise.withResolvers<void>();
		const data = this._pending.join("");
		this._pending = [];
		const callback: WriteCallback = (error) => {
			if (error) deferred.reject(error);
			else deferred.resolve();
		};
		try {
			this._stream.write(data, callback);
		} catch (error) {
			deferred.reject(error);
		}
		return deferred.promise;
	}
	rollback(): this {
		this._pending = [];
		return this;
	}
}
export default { Interface, createInterface, Readline };
