// node:readline — streaming UTF-8 line input and ANSI terminal editing.
// Escape grammar and terminal behavior adapted from Node.js (MIT).
import { EventEmitter } from "node:events";
import { StringDecoder } from "node:string_decoder";
import type { Timeout } from "node:timers";
import { abortError, codedError, invalidArgType, invalidArgValue, outOfRange } from "internal:errors";

export interface Key {
	sequence?: string;
	name?: string;
	ctrl?: boolean;
	meta?: boolean;
	shift?: boolean;
	code?: string;
}
export type WriteCallback = (error?: Error | null) => void;
export interface Output extends EventEmitter {
	write(data: string, callback?: WriteCallback): boolean;
	isTTY?: boolean;
	columns?: number;
}
export interface Input extends EventEmitter {
	pause(): unknown;
	resume(): unknown;
	setRawMode?(raw: boolean): unknown;
	isRaw?: boolean;
	isTTY?: boolean;
}
export type CompleterResult = [string[], string];
export type Completer = (
	line: string,
	callback: (error: Error | null, result?: CompleterResult) => void,
) => CompleterResult | void | Promise<CompleterResult>;
export interface ReadLineOptions {
	input: Input;
	output?: Output;
	terminal?: boolean;
	completer?: Completer;
	history?: string[];
	historySize?: number;
	prompt?: string;
	crlfDelay?: number;
	removeHistoryDuplicates?: boolean;
	signal?: AbortSignal;
	tabSize?: number;
	escapeCodeTimeout?: number;
}
export interface QuestionOptions {
	signal?: AbortSignal;
}
const CSI = "\x1b[";
function writeControl(stream: Output | null | undefined, data: string, callback?: WriteCallback): boolean {
	if (callback !== undefined && typeof callback !== "function")
		throw invalidArgType("callback", "of type function", callback);
	if (!stream || !data) {
		if (callback) process.nextTick(callback, null);
		return true;
	}
	return stream.write(data, callback);
}
export function cursorTo(
	stream: Output | null | undefined,
	x: number,
	y?: number | WriteCallback,
	callback?: WriteCallback,
): boolean {
	if (typeof y === "function") {
		callback = y;
		y = undefined;
	}
	if (Number.isNaN(x)) throw invalidArgValue("x", x);
	if (Number.isNaN(y)) throw invalidArgValue("y", y);
	if (!stream || (typeof x !== "number" && typeof y !== "number")) return writeControl(null, "", callback);
	if (typeof x !== "number")
		throw codedError("ERR_INVALID_CURSOR_POS", "Cannot set cursor row without setting its column", TypeError);
	return writeControl(stream, typeof y === "number" ? `${CSI}${y + 1};${x + 1}H` : `${CSI}${x + 1}G`, callback);
}
export function moveCursor(
	stream: Output | null | undefined,
	dx: number,
	dy: number,
	callback?: WriteCallback,
): boolean {
	const data =
		(dx < 0 ? `${CSI}${-dx}D` : dx > 0 ? `${CSI}${dx}C` : "") +
		(dy < 0 ? `${CSI}${-dy}A` : dy > 0 ? `${CSI}${dy}B` : "");
	return writeControl(stream, data, callback);
}
export function clearLine(stream: Output | null | undefined, direction: number, callback?: WriteCallback): boolean {
	return writeControl(stream, `${CSI}${direction < 0 ? 1 : direction > 0 ? 0 : 2}K`, callback);
}
export function clearScreenDown(stream: Output | null | undefined, callback?: WriteCallback): boolean {
	return writeControl(stream, `${CSI}0J`, callback);
}
const kKeyDecoder = Symbol("readline.keyDecoder");
interface KeypressInput extends Input {
	[kKeyDecoder]?: StringDecoder;
}
interface LineWaiter {
	promise: Promise<IteratorResult<string>>;
	resolve(value: IteratorResult<string>): void;
	reject(reason?: unknown): void;
}
const keyNames: Record<string, string> = {
	"[P": "f1",
	"[Q": "f2",
	"[R": "f3",
	"[S": "f4",
	OP: "f1",
	OQ: "f2",
	OR: "f3",
	OS: "f4",
	"[11~": "f1",
	"[12~": "f2",
	"[13~": "f3",
	"[14~": "f4",
	"[15~": "f5",
	"[17~": "f6",
	"[18~": "f7",
	"[19~": "f8",
	"[20~": "f9",
	"[21~": "f10",
	"[23~": "f11",
	"[24~": "f12",
	"[[A": "f1",
	"[[B": "f2",
	"[[C": "f3",
	"[[D": "f4",
	"[[E": "f5",
	"[A": "up",
	"[B": "down",
	"[C": "right",
	"[D": "left",
	"[E": "clear",
	"[F": "end",
	"[H": "home",
	OA: "up",
	OB: "down",
	OC: "right",
	OD: "left",
	OE: "clear",
	OF: "end",
	OH: "home",
	"[1~": "home",
	"[2~": "insert",
	"[3~": "delete",
	"[4~": "end",
	"[5~": "pageup",
	"[6~": "pagedown",
	"[7~": "home",
	"[8~": "end",
	"[[5~": "pageup",
	"[[6~": "pagedown",
	"[200~": "paste-start",
	"[201~": "paste-end",
	"[Z": "tab",
	"[a": "up",
	"[b": "down",
	"[c": "right",
	"[d": "left",
	"[e": "clear",
	Oa: "up",
	Ob: "down",
	Oc: "right",
	Od: "left",
	Oe: "clear",
};
function decodeKey(sequence: string): { text: string | undefined; key: Key } {
	const key: Key = { sequence, name: undefined, ctrl: false, meta: false, shift: false };
	let text: string | undefined = sequence;
	if (sequence.startsWith("\x1b") && sequence.length > 1) {
		text = undefined;
		let body = sequence.slice(sequence[1] === "\x1b" ? 2 : 1);
		if (body[0] === "[" || body[0] === "O") {
			let modifier = 0;
			const number = /^(\[\[?)(\d+)(?:;(\d+))?([~^$])$/.exec(body);
			const letter = /^(\[|O)(?:(\d+);)?(\d+)?([A-Za-z])$/.exec(body);
			if (number) {
				body = `${number[1]}${number[2]}${number[4]}`;
				modifier = Number(number[3] ?? 1) - 1;
			} else if (letter) {
				body = `${letter[1]}${letter[4]}`;
				modifier = Number(letter[3] ?? 1) - 1;
			}
			key.code = body;
			key.ctrl = Boolean(modifier & 4);
			key.meta = Boolean(modifier & 10);
			key.shift = Boolean(modifier & 1);
			if (body.endsWith("$") || /^\[[a-e]$/.test(body) || body === "[Z") key.shift = true;
			if (body.endsWith("^") || /^O[a-e]$/.test(body)) key.ctrl = true;
			key.name = keyNames[body] ?? keyNames[body.replace(/[\^$]$/, "~")] ?? "undefined";
		} else {
			const nested = decodeKey(body);
			Object.assign(key, nested.key, { sequence, meta: true });
		}
	} else {
		const cp = sequence.codePointAt(0) ?? 0;
		if (sequence === "\r") key.name = "return";
		else if (sequence === "\n") key.name = "enter";
		else if (sequence === "\t") key.name = "tab";
		else if (sequence === "\b" || sequence === "\x7f") key.name = "backspace";
		else if (sequence === "\x1b") {
			key.name = "escape";
			key.meta = true;
		} else if (sequence === " ") key.name = "space";
		else if (cp <= 26) {
			key.name = String.fromCharCode(cp + 96);
			key.ctrl = true;
		} else if (/^[0-9A-Za-z]$/.test(sequence)) {
			key.name = sequence.toLowerCase();
			key.shift = /^[A-Z]$/.test(sequence);
		}
	}
	return { text, key };
}
export function emitKeypressEvents(
	stream: Input,
	iface: { escapeCodeTimeout?: number; isCompletionEnabled?: boolean } = {},
): void {
	const input = stream as KeypressInput;
	if (input[kKeyDecoder]) return;
	const decoder = new StringDecoder("utf8");
	input[kKeyDecoder] = decoder;
	let pending = "";
	let timer: Timeout | undefined;
	const emit = (sequence: string): void => {
		const { text, key } = decodeKey(sequence);
		stream.emit("keypress", text, key);
	};
	const drain = (flush = false): void => {
		while (pending) {
			if (pending[0] !== "\x1b") {
				const char = String.fromCodePoint(pending.codePointAt(0)!);
				pending = pending.slice(char.length);
				emit(char);
				continue;
			}
			const start = pending[1] === "\x1b" ? 2 : 1;
			if (pending.length <= start) {
				if (flush) {
					emit(pending);
					pending = "";
				}
				break;
			}
			if (pending[start] === "[" || pending[start] === "O") {
				const match = /^(?:\[\[?[0-9;]*[A-Za-z~^$]|O[0-9;]*[A-Za-z])/.exec(pending.slice(start));
				if (!match) {
					if (flush) {
						emit(pending);
						pending = "";
					}
					break;
				}
				const length = start + match[0].length,
					sequence = pending.slice(0, length);
				pending = pending.slice(length);
				emit(sequence);
			} else {
				const char = String.fromCodePoint(pending.codePointAt(start)!);
				const sequence = pending.slice(0, start + char.length);
				pending = pending.slice(sequence.length);
				emit(sequence);
			}
		}
	};
	const onData = (chunk: unknown): void => {
		if (!stream.listenerCount("keypress")) {
			stream.removeListener("data", onData);
			stream.on("newListener", onNewListener);
			return;
		}
		if (timer !== undefined) {
			clearTimeout(timer);
			timer = undefined;
		}
		pending += typeof chunk === "string" ? chunk : decoder.write(chunk as Buffer);
		iface.isCompletionEnabled = true;
		drain();
		if (pending) {
			timer = setTimeout(() => {
				timer = undefined;
				drain(true);
			}, iface.escapeCodeTimeout ?? 500);
			timer.unref?.();
		}
	};
	const onNewListener = (event: unknown): void => {
		if (event === "keypress") {
			stream.on("data", onData);
			stream.removeListener("newListener", onNewListener);
		}
	};
	if (stream.listenerCount("keypress")) stream.on("data", onData);
	else stream.on("newListener", onNewListener);
	stream.once("end", () => {
		clearTimeout(timer);
		pending += decoder.end();
		drain(true);
	});
}
function previousChar(line: string, cursor: number): number {
	return cursor > 1 && /[\uDC00-\uDFFF]/.test(line[cursor - 1]) && /[\uD800-\uDBFF]/.test(line[cursor - 2]) ? 2 : 1;
}
function nextChar(line: string, cursor: number): number {
	return (line.codePointAt(cursor) ?? 0) > 65535 ? 2 : 1;
}

export class Interface extends EventEmitter {
	input: Input;
	output?: Output;
	terminal: boolean;
	line = "";
	cursor = 0;
	closed = false;
	paused = false;
	history: string[];
	historySize: number;
	historyIndex = -1;
	removeHistoryDuplicates: boolean;
	completer?: Completer;
	crlfDelay: number;
	escapeCodeTimeout: number;
	tabSize: number;
	isCompletionEnabled = true;
	protected _prompt: string;
	protected _question?: (answer: string) => void;
	protected _questionCleanup?: () => void;
	protected _oldPrompt?: string;
	private _decoder = new StringDecoder("utf8");
	private _buffer = "";
	private _sawCR = 0;
	private _cursorRows = 0;
	private _historyDraft = "";
	private _historyPrefix: string | null = null;
	private _paste = false;
	private _killRing: string[] = [];
	private _killRingCursor = 0;
	private _yanking = false;
	private _previousKey?: Key;
	private _undo: { line: string; cursor: number }[] = [];
	private _redo: { line: string; cursor: number }[] = [];
	private _onData: (data: unknown) => void;
	private _onKey: (text: unknown, key: unknown) => void;
	private _onEnd: () => void;
	private _onError: (error: unknown) => void;
	private _onResize: () => void;
	private _signalCleanup?: () => void;
	constructor(input: Input | ReadLineOptions, output?: Output, completer?: Completer, terminal?: boolean) {
		super();
		const options: ReadLineOptions =
			input && "input" in input ? (input as ReadLineOptions) : { input: input as Input, output, completer, terminal };
		if (!options.input || typeof options.input.on !== "function")
			throw invalidArgType("input", "a Readable stream", options.input);
		if (options.completer !== undefined && typeof options.completer !== "function")
			throw invalidArgType("completer", "of type function", options.completer);
		this.input = options.input;
		this.output = options.output;
		this.completer = options.completer;
		this.terminal = options.terminal ?? Boolean(options.output?.isTTY);
		this.historySize = options.historySize ?? 30;
		if (!Number.isInteger(this.historySize) || this.historySize < 0)
			throw outOfRange("historySize", "an integer >= 0", this.historySize);
		if (
			options.history !== undefined &&
			(!Array.isArray(options.history) || options.history.some((item) => typeof item !== "string"))
		)
			throw invalidArgType("history", "an array of strings", options.history);
		this.history = options.history ? options.history.slice() : [];
		this.removeHistoryDuplicates = Boolean(options.removeHistoryDuplicates);
		this.crlfDelay = Math.max(100, options.crlfDelay || 100);
		this.escapeCodeTimeout = options.escapeCodeTimeout ?? 500;
		if (!Number.isFinite(this.escapeCodeTimeout)) throw invalidArgValue("escapeCodeTimeout", this.escapeCodeTimeout);
		this.tabSize = options.tabSize ?? 8;
		if (!Number.isInteger(this.tabSize) || this.tabSize < 1 || this.tabSize > 4294967295)
			throw outOfRange("tabSize", "an integer >= 1 && <= 4294967295", this.tabSize);
		this._prompt = options.prompt ?? "> ";
		this._onData = (data) => this._normalWrite(data);
		this._onKey = (text, key) => this._ttyWrite(text as string | undefined, key as Key);
		this._onError = (error) => this.emit("error", error);
		this._onResize = () => this._refreshLine();
		this._onEnd = () => {
			const rest = this.terminal ? this.line : this._buffer + this._decoder.end();
			if (rest) this._onLine(rest);
			this.close();
		};
		this.input.on("error", this._onError);
		this.input.on("end", this._onEnd);
		if (this.terminal) {
			emitKeypressEvents(this.input, this);
			this.input.on("keypress", this._onKey);
			this.input.setRawMode?.(true);
			this.output?.on("resize", this._onResize);
		} else this.input.on("data", this._onData);
		if (options.signal) {
			const signal = options.signal;
			if (typeof signal.addEventListener !== "function")
				throw invalidArgType("options.signal", "an AbortSignal", signal);
			const close = (): void => this.close();
			signal.addEventListener("abort", close, { once: true });
			this._signalCleanup = () => signal.removeEventListener("abort", close);
			if (signal.aborted) process.nextTick(close);
		}
		this.input.resume();
	}
	get columns(): number {
		return this.output?.columns || Infinity;
	}
	setPrompt(prompt: string): void {
		this._prompt = prompt;
	}
	getPrompt(): string {
		return this._prompt;
	}
	prompt(preserveCursor = false): void {
		if (this.closed) throw codedError("ERR_USE_AFTER_CLOSE", "readline was closed");
		if (this.paused) this.resume();
		if (!preserveCursor) this.cursor = 0;
		if (this.terminal) this._refreshLine();
		else this.output?.write(this._prompt);
	}
	question(
		query: string,
		optionsOrCallback?: QuestionOptions | ((answer: string) => void),
		callback?: (answer: string) => void,
	): void {
		if (this.closed) throw codedError("ERR_USE_AFTER_CLOSE", "readline was closed");
		const options = typeof optionsOrCallback === "object" && optionsOrCallback !== null ? optionsOrCallback : {};
		const cb = typeof optionsOrCallback === "function" ? optionsOrCallback : callback;
		if (options.signal?.aborted) return;
		if (typeof cb !== "function") return;
		if (this._question) {
			this.prompt();
			return;
		}
		if (options.signal) {
			const signal = options.signal;
			const cancel = (): void => this._cancelQuestion();
			signal.addEventListener("abort", cancel, { once: true });
			this._questionCleanup = () => signal.removeEventListener("abort", cancel);
		}
		this._oldPrompt = this._prompt;
		this._prompt = query;
		this._question = cb;
		this.prompt();
	}
	protected _cancelQuestion(): void {
		this._question = undefined;
		this._questionCleanup?.();
		this._questionCleanup = undefined;
		if (this._oldPrompt !== undefined) {
			this._prompt = this._oldPrompt;
			this._oldPrompt = undefined;
		}
		this.line = "";
		this.cursor = 0;
		if (this.terminal) this._writeToOutput("\r\n");
	}
	close(): void {
		if (this.closed) return;
		this.pause();
		this.closed = true;
		this.input.removeListener("data", this._onData);
		this.input.removeListener("keypress", this._onKey);
		this.input.removeListener("error", this._onError);
		this.input.removeListener("end", this._onEnd);
		this.output?.removeListener("resize", this._onResize);
		if (this.terminal) this.input.setRawMode?.(false);
		this._signalCleanup?.();
		this._questionCleanup?.();
		this.emit("close");
	}
	pause(): this {
		if (this.closed) throw codedError("ERR_USE_AFTER_CLOSE", "readline was closed");
		if (!this.paused) {
			this.input.pause();
			this.paused = true;
			this.emit("pause");
		}
		return this;
	}
	resume(): this {
		if (this.closed) throw codedError("ERR_USE_AFTER_CLOSE", "readline was closed");
		if (this.paused) {
			this.input.resume();
			this.paused = false;
			this.emit("resume");
		}
		return this;
	}
	write(data: string | Uint8Array | null, key?: Key): void {
		if (this.closed) throw codedError("ERR_USE_AFTER_CLOSE", "readline was closed");
		if (this.paused) this.resume();
		if (this.terminal)
			this._ttyWrite(typeof data === "string" ? data : data ? Buffer.from(data).toString() : undefined, key ?? {});
		else if (data !== null) this._normalWrite(data);
	}
	_normalWrite(chunk: unknown): void {
		let text = typeof chunk === "string" ? chunk : this._decoder.write(chunk as Buffer);
		if (this._sawCR && text[0] === "\n" && Date.now() - this._sawCR <= this.crlfDelay) text = text.slice(1);
		this._sawCR = 0;
		const lines = text.split(/\r\n|\n|\r/);
		if (lines.length === 1) {
			this._buffer += text;
			return;
		}
		if (text.endsWith("\r")) this._sawCR = Date.now();
		lines[0] = this._buffer + lines[0];
		this._buffer = lines.pop()!;
		for (const line of lines) this._onLine(line);
	}
	_onLine(line: string): void {
		if (this._question) {
			const callback = this._question;
			this._question = undefined;
			this._questionCleanup?.();
			this._questionCleanup = undefined;
			if (this._oldPrompt !== undefined) {
				this._prompt = this._oldPrompt;
				this._oldPrompt = undefined;
			}
			callback(line);
		} else this.emit("line", line);
	}
	_writeToOutput(text: string): void {
		this.output?.write(text);
	}
	_getDisplayPos(text: string): { cols: number; rows: number } {
		text = text.replace(/\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\x07]*(?:\x07|\x1b\\)/g, "");
		let offset = 0,
			rows = 0;
		const columns = this.columns;
		for (const char of text) {
			if (char === "\n") {
				rows += Math.ceil(offset / columns) || 1;
				offset = 0;
				continue;
			}
			if (char === "\t") {
				offset += this.tabSize - (offset % this.tabSize);
				continue;
			}
			const cp = char.codePointAt(0)!;
			let width = /\p{Mark}/u.test(char) || cp === 0x200d || cp < 32 || (cp >= 0x7f && cp < 0xa0) ? 0 : 1;
			if (
				cp >= 0x1100 &&
				(cp <= 0x115f ||
					cp === 0x2329 ||
					cp === 0x232a ||
					(cp >= 0x2e80 && cp <= 0xa4cf && cp !== 0x303f) ||
					(cp >= 0xac00 && cp <= 0xd7a3) ||
					(cp >= 0xf900 && cp <= 0xfaff) ||
					(cp >= 0xfe10 && cp <= 0xfe19) ||
					(cp >= 0xfe30 && cp <= 0xfe6f) ||
					(cp >= 0xff00 && cp <= 0xff60) ||
					(cp >= 0xffe0 && cp <= 0xffe6) ||
					(cp >= 0x1f300 && cp <= 0x1faff) ||
					(cp >= 0x20000 && cp <= 0x3fffd))
			)
				width = 2;
			if (width === 2 && (offset + 1) % columns === 0) offset++;
			offset += width;
		}
		const cols = Number.isFinite(columns) ? offset % columns : offset;
		return { cols, rows: rows + (Number.isFinite(columns) ? Math.floor(offset / columns) : 0) };
	}
	getCursorPos(): { rows: number; cols: number } {
		return this._getDisplayPos(this._prompt + this.line.slice(0, this.cursor));
	}
	_refreshLine(): void {
		if (!this.terminal || !this.output || this.closed) return;
		if (this._cursorRows) moveCursor(this.output, 0, -this._cursorRows);
		cursorTo(this.output, 0);
		clearScreenDown(this.output);
		const full = this._prompt + this.line;
		this.output.write(full);
		const end = this._getDisplayPos(full),
			cursor = this.getCursorPos();
		if (end.cols === 0 && end.rows) this.output.write(" ");
		cursorTo(this.output, cursor.cols);
		moveCursor(this.output, 0, cursor.rows - end.rows);
		this._cursorRows = cursor.rows;
	}
	_addHistory(): string {
		const line = this.line;
		if (line.trim().length && this.historySize > 0) {
			if (this.removeHistoryDuplicates) {
				for (let i = this.history.length - 1; i >= 0; i--) if (this.history[i] === line) this.history.splice(i, 1);
			}
			if (this.history[0] !== line) this.history.unshift(line);
			if (this.history.length > this.historySize) this.history.length = this.historySize;
			this.emit("history", this.history);
		}
		this.historyIndex = -1;
		this._historyPrefix = null;
		return line;
	}
	clearLine(): void {
		this.cursor = this.line.length;
		this._refreshLine();
		this._writeToOutput("\r\n");
		this.line = "";
		this.cursor = 0;
		this._cursorRows = 0;
	}
	private _submit(): void {
		if (this._sawCR && Date.now() - this._sawCR <= this.crlfDelay) this._sawCR = 0;
		const line = this._addHistory();
		this.clearLine();
		this._undo = [];
		this._redo = [];
		this._onLine(line);
	}
	private _saveUndo(): void {
		this._undo.push({ line: this.line, cursor: this.cursor });
		if (this._undo.length > 2048) this._undo.shift();
		this._redo = [];
	}
	_insertString(text: string): void {
		this._saveUndo();
		this.line = this.line.slice(0, this.cursor) + text + this.line.slice(this.cursor);
		this.cursor += text.length;
		this._refreshLine();
	}
	_moveCursor(offset: number): void {
		this.cursor = Math.max(0, Math.min(this.line.length, this.cursor + offset));
		this._refreshLine();
	}
	private _delete(start: number, end: number, kill = false): void {
		if (start === end) return;
		this._saveUndo();
		if (kill) {
			const deleted = this.line.slice(start, end);
			if (deleted && deleted !== this._killRing[0]) {
				this._killRing.unshift(deleted);
				this._killRingCursor = 0;
				if (this._killRing.length > 32) this._killRing.pop();
			}
		}
		this.line = this.line.slice(0, start) + this.line.slice(end);
		this.cursor = start;
		this._refreshLine();
	}
	_deleteLeft(): void {
		if (this.cursor) this._delete(this.cursor - previousChar(this.line, this.cursor), this.cursor);
	}
	_deleteRight(): void {
		if (this.cursor < this.line.length) this._delete(this.cursor, this.cursor + nextChar(this.line, this.cursor));
	}
	_deleteLineLeft(): void {
		this._delete(0, this.cursor, true);
	}
	_deleteLineRight(): void {
		this._delete(this.cursor, this.line.length, true);
	}
	private _wordLeftPos(): number {
		const before = this.line.slice(0, this.cursor);
		const match = /(?:\w+|[^\w\s]+)\s*$/.exec(before);
		return match ? match.index : 0;
	}
	private _wordRightPos(): number {
		const match = /^(?:\s*)(?:\w+|[^\w\s]+)/.exec(this.line.slice(this.cursor));
		return match ? this.cursor + match[0].length : this.line.length;
	}
	_wordLeft(): void {
		this.cursor = this._wordLeftPos();
		this._refreshLine();
	}
	_wordRight(): void {
		this.cursor = this._wordRightPos();
		this._refreshLine();
	}
	_deleteWordLeft(): void {
		this._delete(this._wordLeftPos(), this.cursor, true);
	}
	_deleteWordRight(): void {
		this._delete(this.cursor, this._wordRightPos(), true);
	}
	private _history(direction: number): void {
		if (this._historyPrefix === null) {
			this._historyPrefix = this.line.slice(0, this.cursor);
			this._historyDraft = this.line;
		}
		let index = this.historyIndex + direction;
		while (index >= 0 && index < this.history.length && !this.history[index].startsWith(this._historyPrefix))
			index += direction;
		if (index >= this.history.length) return;
		this.historyIndex = Math.max(-1, index);
		this.line = index < 0 ? this._historyDraft : this.history[index];
		this.cursor = this.line.length;
		this._refreshLine();
	}
	_historyPrev(): void {
		this._history(1);
	}
	_historyNext(): void {
		this._history(-1);
	}
	_tabComplete(lastKeypressWasTab = false): void {
		if (!this.completer) {
			this._insertString("\t");
			return;
		}
		const line = this.line.slice(0, this.cursor);
		this.pause();
		let completed = false;
		const done = (error: Error | null, result?: CompleterResult): void => {
			if (completed) return;
			completed = true;
			if (this.closed) return;
			this.resume();
			if (error) {
				this._writeToOutput(`\r\nTab completion error: ${error.message}\r\n`);
				this._refreshLine();
				return;
			}
			if (!result || !Array.isArray(result[0]) || !result[0].length) return;
			const [matches, completeOn] = result;
			const candidates = matches.filter((value) => value !== "");
			let prefix = candidates[0] ?? "";
			for (let i = 1; i < candidates.length; i++) {
				let j = 0;
				while (j < prefix.length && prefix[j] === candidates[i][j]) j++;
				prefix = prefix.slice(0, j);
			}
			if (prefix.startsWith(completeOn) && prefix.length > completeOn.length)
				this._insertString(prefix.slice(completeOn.length));
			else if (!completeOn.startsWith(prefix)) {
				this._saveUndo();
				this.line = this.line.slice(0, this.cursor - completeOn.length) + prefix + this.line.slice(this.cursor);
				this.cursor += prefix.length - completeOn.length;
				this._refreshLine();
			} else if (lastKeypressWasTab) {
				const widths = matches.map((value) => this._getDisplayPos(value).cols);
				const width = Math.max(...widths) + 2;
				let columns = Math.floor(this.columns / width) || 1;
				if (!Number.isFinite(columns)) columns = 1;
				let output = "\r\n",
					index = 0,
					padding = 0;
				for (let i = 0; i < matches.length; i++) {
					if (matches[i] === "" || index === columns) {
						output += "\r\n";
						index = 0;
						padding = 0;
					} else output += " ".repeat(padding);
					if (matches[i] !== "") {
						output += matches[i];
						padding = width - widths[i];
						index++;
					} else output += "\r\n";
				}
				if (index) output += "\r\n\r\n";
				this._writeToOutput(output);
				this._cursorRows = 0;
				this._refreshLine();
			}
		};
		try {
			const result = this.completer(line, done);
			if (result && "then" in result)
				result.then(
					(value) => done(null, value),
					(error) => done(error),
				);
			else if (result) done(null, result);
		} catch (error) {
			done(error as Error);
		}
	}
	_ttyWrite(text: string | undefined, key: Key = {}): void {
		if (this.closed) return;
		const previousKey = this._previousKey;
		this._previousKey = key;
		if (!key.meta || key.name !== "y") this._yanking = false;
		if (key.name === "paste-start") {
			this._paste = true;
			return;
		}
		if (key.name === "paste-end") {
			this._paste = false;
			return;
		}
		if (this._paste) {
			this._insertString(text ?? key.sequence ?? "");
			return;
		}
		if (key.name !== "up" && key.name !== "down" && !(key.ctrl && (key.name === "p" || key.name === "n")))
			this._historyPrefix = null;
		if (key.sequence === "\x1f" || key.sequence === "\x1e") {
			const from = key.sequence === "\x1f" ? this._undo : this._redo,
				to = key.sequence === "\x1f" ? this._redo : this._undo;
			const state = from.pop();
			if (state) {
				to.push({ line: this.line, cursor: this.cursor });
				this.line = state.line;
				this.cursor = state.cursor;
				this._refreshLine();
			}
			return;
		}
		if (key.name === "escape") return;
		if (key.ctrl && key.shift && key.name === "backspace") {
			this._deleteLineLeft();
			return;
		}
		if (key.ctrl && key.shift && key.name === "delete") {
			this._deleteLineRight();
			return;
		}
		if (key.ctrl) {
			switch (key.name) {
				case "c":
					if (this.listenerCount("SIGINT")) this.emit("SIGINT");
					else this.close();
					break;
				case "d":
					if (!this.line) this.close();
					else this._deleteRight();
					break;
				case "h":
					this._deleteLeft();
					break;
				case "u":
					this._deleteLineLeft();
					break;
				case "k":
					this._deleteLineRight();
					break;
				case "a":
					this._moveCursor(-Infinity);
					break;
				case "e":
					this._moveCursor(Infinity);
					break;
				case "b":
					this._moveCursor(-previousChar(this.line, this.cursor));
					break;
				case "f":
					this._moveCursor(nextChar(this.line, this.cursor));
					break;
				case "l":
					cursorTo(this.output, 0, 0);
					clearScreenDown(this.output);
					this._cursorRows = 0;
					this._refreshLine();
					break;
				case "n":
					this._historyNext();
					break;
				case "p":
					this._historyPrev();
					break;
				case "y":
					if (this._killRing.length) {
						this._yanking = true;
						this._insertString(this._killRing[this._killRingCursor]);
					}
					break;
				case "w":
				case "backspace":
					this._deleteWordLeft();
					break;
				case "delete":
					this._deleteWordRight();
					break;
				case "left":
					this._wordLeft();
					break;
				case "right":
					this._wordRight();
					break;
				case "z":
					if (this.listenerCount("SIGTSTP")) this.emit("SIGTSTP");
					else {
						process.once("SIGCONT", () => {
							if (!this.paused) {
								this.pause();
								this.emit("SIGCONT");
							}
							this.input.setRawMode?.(true);
							this._refreshLine();
						});
						this.input.setRawMode?.(false);
						process.kill(process.pid, "SIGTSTP");
					}
					break;
			}
		} else if (key.meta) {
			switch (key.name) {
				case "b":
					this._wordLeft();
					break;
				case "f":
					this._wordRight();
					break;
				case "d":
				case "delete":
					this._deleteWordRight();
					break;
				case "backspace":
					this._deleteWordLeft();
					break;
				case "y":
					if (this._yanking && this._killRing.length > 1) {
						const previous = this._killRing[this._killRingCursor];
						this._killRingCursor = (this._killRingCursor + 1) % this._killRing.length;
						const replacement = this._killRing[this._killRingCursor],
							head = this.line.slice(0, this.cursor - previous.length);
						this.line = head + replacement + this.line.slice(this.cursor);
						this.cursor = head.length + replacement.length;
						this._refreshLine();
					}
					break;
			}
		} else {
			switch (key.name) {
				case "return":
					this._submit();
					this._sawCR = Date.now();
					break;
				case "enter":
					if (!(this._sawCR && Date.now() - this._sawCR <= this.crlfDelay)) this._submit();
					this._sawCR = 0;
					break;
				case "backspace":
					this._deleteLeft();
					break;
				case "delete":
					this._deleteRight();
					break;
				case "left":
					this._moveCursor(-previousChar(this.line, this.cursor));
					break;
				case "right":
					this._moveCursor(nextChar(this.line, this.cursor));
					break;
				case "home":
					this._moveCursor(-Infinity);
					break;
				case "end":
					this._moveCursor(Infinity);
					break;
				case "up":
					this._historyPrev();
					break;
				case "down":
					this._historyNext();
					break;
				case "tab":
					if (this.isCompletionEnabled) this._tabComplete(previousKey?.name === "tab");
					break;
				default:
					if (text) {
						const lines = text.split(/\r\n|\r|\n/);
						for (let i = 0; i < lines.length; i++) {
							if (i) this._submit();
							if (lines[i]) this._insertString(lines[i]);
						}
					}
			}
		}
	}
	[Symbol.asyncIterator](): AsyncIterableIterator<string> {
		const queue: string[] = [];
		const waiting: LineWaiter[] = [];
		let done = this.closed,
			failure: unknown;
		const onLine = (line: unknown): void => {
			if (waiting.length) waiting.shift()!.resolve({ value: String(line), done: false });
			else queue.push(String(line));
		};
		const cleanup = (): void => {
			this.off("line", onLine);
			this.off("close", onClose);
			this.off("error", onError);
		};
		const onClose = (): void => {
			done = true;
			for (const waiter of waiting.splice(0)) waiter.resolve({ value: undefined, done: true });
			cleanup();
		};
		const onError = (error: unknown): void => {
			failure = error;
			done = true;
			for (const waiter of waiting.splice(0)) waiter.reject(error);
			cleanup();
		};
		this.on("line", onLine);
		this.on("close", onClose);
		this.on("error", onError);
		const iface = this;
		return {
			[Symbol.asyncIterator]() {
				return this;
			},
			next(): Promise<IteratorResult<string>> {
				if (queue.length) return Promise.resolve({ value: queue.shift()!, done: false });
				if (failure) return Promise.reject(failure);
				if (done) return Promise.resolve({ value: undefined, done: true });
				const waiter = Promise.withResolvers<IteratorResult<string>>();
				waiting.push(waiter);
				return waiter.promise;
			},
			return(): Promise<IteratorResult<string>> {
				onClose();
				iface.close();
				return Promise.resolve({ value: undefined, done: true });
			},
			throw(error: unknown): Promise<IteratorResult<string>> {
				onClose();
				iface.close();
				return Promise.reject(error);
			},
		};
	}
	[Symbol.dispose](): void {
		this.close();
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
Object.defineProperty(Interface.prototype.question, Symbol.for("nodejs.util.promisify.custom"), {
	value: function (this: Interface, query: string, options: QuestionOptions = {}): Promise<string> {
		const deferred = Promise.withResolvers<string>();
		if (options.signal?.aborted) {
			deferred.reject(abortError(options.signal.reason));
			return deferred.promise;
		}
		const signal = options.signal;
		const cancel = (): void => deferred.reject(abortError(signal?.reason));
		signal?.addEventListener("abort", cancel, { once: true });
		try {
			this.question(query, options, (answer) => {
				signal?.removeEventListener("abort", cancel);
				deferred.resolve(answer);
			});
		} catch (error) {
			signal?.removeEventListener("abort", cancel);
			deferred.reject(error);
		}
		return deferred.promise;
	},
});
// node:readline/promises extends Interface during evaluation, so defer the cycle.
export const promises = {
	get Interface(): unknown {
		const module = require("node:readline/promises") as { Interface: unknown };
		return module.Interface;
	},
	get Readline(): unknown {
		const module = require("node:readline/promises") as { Readline: unknown };
		return module.Readline;
	},
	createInterface(...args: Parameters<typeof createInterface>): unknown {
		const module = require("node:readline/promises") as {
			createInterface: (...args: Parameters<typeof createInterface>) => unknown;
		};
		return module.createInterface(...args);
	},
};
export default {
	Interface,
	createInterface,
	emitKeypressEvents,
	cursorTo,
	moveCursor,
	clearLine,
	clearScreenDown,
	promises,
};
