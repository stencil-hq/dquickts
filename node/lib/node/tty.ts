import * as readline from "node:readline";
// node:tty — libuv terminal streams. Color heuristics follow Node's internal/tty.
import * as native from "qbun:net";
import * as core from "qbun:core";
import { Socket } from "node:net";
import { codedError, invalidArgType, outOfRange } from "internal:errors";

type Callback = (error?: Error | null) => void;
export function isatty(fd: number): boolean {
	return Number.isInteger(fd) && fd >= 0 && fd <= 2147483647 && native.isatty(fd);
}
function validateFd(fd: number): void {
	if (!Number.isInteger(fd) || fd < 0 || fd > 2147483647)
		throw codedError("ERR_INVALID_FD", `"fd" must be a positive integer: ${String(fd)}`, RangeError);
}
export class ReadStream extends Socket {
	isRaw = false;
	isTTY = true;
	rawMode: false | "raw" | "io" = false;
	fd: number;
	private _stdioRef = true;
	constructor(fd: number, options: Record<string, unknown> = {}) {
		validateFd(fd);
		super({
			...options,
			handle: native.createTTY(fd, true),
			readable: true,
			writable: false,
			manualStart: true,
			readableHighWaterMark: 0,
		});
		this.fd = fd;
		// idle until a reader starts it; not pause(): an explicitly paused stream
		// would not flow when a 'data' listener is added
		this._handle.reading = false;
		this._handle.readStop();
		this._handle.unref();
	}
	setRawMode(mode: boolean | "raw" | "io"): this {
		if (typeof mode === "string" && mode !== "raw" && mode !== "io")
			throw codedError("ERR_INVALID_ARG_VALUE", "mode must be true, false, 'raw', or 'io'", TypeError);
		const raw = Boolean(mode);
		try {
			this._handle?.setRawMode(raw);
			this.isRaw = raw;
			this.rawMode = raw ? (typeof mode === "string" ? mode : "raw") : false;
		} catch (error) {
			this.emit("error", error);
		}
		return this;
	}
	pause(): this {
		super.pause();
		if (this._handle) {
			this._handle.reading = false;
			this._handle.readStop();
			this._handle.unref();
		}
		return this;
	}
	resume(): this {
		super.resume();
		if (!this.destroyed && this._handle && !this._handle.reading) {
			try {
				this._handle.readStart();
				this._handle.reading = true;
			} catch (error) {
				process.nextTick(() => this.emit("error", error));
			}
		}
		if (this._stdioRef) this._handle?.ref();
		return this;
	}
	ref(): this {
		this._stdioRef = true;
		if (!this.isPaused()) this._handle?.ref();
		return this;
	}
	unref(): this {
		this._stdioRef = false;
		this._handle?.unref();
		return this;
	}
}
let warnedColorOverride = false;
function colorDepth(env: Record<string, string | undefined>): number {
	if (env.FORCE_COLOR !== undefined) {
		const forced: Record<string, number> = { "": 4, "1": 4, true: 4, "2": 8, "3": 24 };
		const depth = forced[env.FORCE_COLOR] ?? 1;
		if (depth !== 1 && !warnedColorOverride && (env.NO_COLOR || env.NODE_DISABLE_COLORS)) {
			const names =
				env.NO_COLOR && env.NODE_DISABLE_COLORS
					? "NODE_DISABLE_COLORS' and 'NO_COLOR"
					: env.NO_COLOR
						? "NO_COLOR"
						: "NODE_DISABLE_COLORS";
			process.emitWarning(`The '${names}' env is ignored due to the 'FORCE_COLOR' env being set.`);
			warnedColorOverride = true;
		}
		return depth;
	}
	if (env.NODE_DISABLE_COLORS || env.NO_COLOR || env.TERM === "dumb") return 1;
	if (env.TMUX) return 24;
	if ("TF_BUILD" in env && "AGENT_NAME" in env) return 4;
	if ("CI" in env) {
		const ci: Record<string, number> = {
			APPVEYOR: 8,
			BUILDKITE: 8,
			CIRCLECI: 24,
			DRONE: 8,
			GITEA_ACTIONS: 24,
			GITHUB_ACTIONS: 24,
			GITLAB_CI: 8,
			TRAVIS: 8,
		};
		for (const key in ci) if (key in env) return ci[key];
		return env.CI_NAME === "codeship" ? 8 : 1;
	}
	if ("TEAMCITY_VERSION" in env) return /^(9\.(0*[1-9]\d*)\.|\d{2,}\.)/.test(env.TEAMCITY_VERSION ?? "") ? 4 : 1;
	if (env.TERM_PROGRAM === "iTerm.app")
		return !env.TERM_PROGRAM_VERSION || /^[0-2]\./.test(env.TERM_PROGRAM_VERSION) ? 8 : 24;
	if (env.TERM_PROGRAM === "HyperTerm" || env.TERM_PROGRAM === "MacTerm") return 24;
	if (env.TERM_PROGRAM === "Apple_Terminal") return 8;
	if (env.COLORTERM === "truecolor" || env.COLORTERM === "24bit") return 24;
	if (env.TERM) {
		if (/truecolor/.test(env.TERM)) return 24;
		if (/^xterm-256/.test(env.TERM)) return 8;
		const term = env.TERM.toLowerCase();
		const exact: Record<string, number> = {
			eterm: 4,
			cons25: 4,
			console: 4,
			cygwin: 4,
			dtterm: 4,
			gnome: 4,
			hurd: 4,
			jfbterm: 4,
			konsole: 4,
			kterm: 4,
			mlterm: 4,
			mosh: 24,
			putty: 4,
			st: 4,
			"rxvt-unicode-24bit": 24,
			terminator: 24,
			"xterm-kitty": 24,
		};
		if (exact[term]) return exact[term];
		if (/ansi|color|linux|direct|^con[0-9]*x[0-9]|^rxvt|^screen|^xterm|^vt100|^vt220/.test(term)) return 4;
	}
	return env.COLORTERM ? 4 : 1;
}
export class WriteStream extends Socket {
	fd: number;
	isTTY = true;
	columns!: number;
	rows!: number;
	constructor(fd: number) {
		validateFd(fd);
		super({ handle: native.createTTY(fd, false), readable: false, writable: true, manualStart: true });
		this.fd = fd;
		this._handle.setBlocking(true);
		this._handle.unref();
		const size = this._handle.getWindowSize();
		if (size) [this.columns, this.rows] = size;
	}
	write(chunk: string | Uint8Array, encodingOrCallback?: string | Callback, callback?: Callback): true {
		const done = typeof encodingOrCallback === "function" ? encodingOrCallback : callback;
		if (done !== undefined && typeof done !== "function") throw invalidArgType("callback", "of type function", done);
		if (typeof chunk !== "string" && !(chunk instanceof Uint8Array))
			throw invalidArgType("chunk", "of type string or an instance of Buffer, TypedArray, or DataView", chunk);
		const data =
			typeof chunk === "string" && typeof encodingOrCallback === "string"
				? Buffer.from(chunk, encodingOrCallback)
				: chunk;
		try {
			core.writeSync(this.fd, data);
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
	_refreshSize(): void {
		const size = this._handle?.getWindowSize();
		if (size && (size[0] !== this.columns || size[1] !== this.rows)) {
			[this.columns, this.rows] = size;
			this.emit("resize");
		}
	}
	getWindowSize(): [number, number] {
		return [this.columns, this.rows];
	}
	getColorDepth(env: Record<string, string | undefined> = process.env): number {
		return colorDepth(env);
	}
	hasColors(count?: number | Record<string, string | undefined>, env?: Record<string, string | undefined>): boolean {
		if (count === undefined || (typeof count === "object" && count !== null)) {
			env = count;
			count = 16;
		}
		if (typeof count !== "number") throw invalidArgType("count", "of type number", count);
		if (!Number.isInteger(count) || count < 2) throw outOfRange("count", "an integer >= 2", count);
		return count <= 2 ** colorDepth(env ?? process.env);
	}
	cursorTo(x: number, y?: number | Callback, callback?: Callback): boolean {
		return readline.cursorTo(this, x, y, callback);
	}
	moveCursor(dx: number, dy: number, callback?: Callback): boolean {
		return readline.moveCursor(this, dx, dy, callback);
	}
	clearLine(dir: number, callback?: Callback): boolean {
		return readline.clearLine(this, dir, callback);
	}
	clearScreenDown(callback?: Callback): boolean {
		return readline.clearScreenDown(this, callback);
	}
}
export default { isatty, ReadStream, WriteStream };
