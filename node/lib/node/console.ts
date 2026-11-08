import { inspect, formatWithOptions, colorEnabled, stripVTControlCharacters } from "node:util";
import type { InspectOptions } from "internal:util/inspect";
import { codedError, invalidArgType } from "internal:errors";
import * as core from "qbun:core";
interface Output {
	write(data: string): unknown;
	isTTY?: boolean;
}
interface Options {
	stdout: Output;
	stderr?: Output;
	ignoreErrors?: boolean;
	colorMode?: boolean | "auto";
	inspectOptions?: InspectOptions;
	groupIndentation?: number;
}
export class Console {
	private stdout: Output;
	private stderr: Output;
	private ignoreErrors: boolean;
	private colorMode: boolean | "auto";
	private inspectOptions: InspectOptions;
	private groupIndentation: number;
	private indent = 0;
	private times = new Map<string, bigint>();
	private counts = new Map<string, number>();
	constructor(stdout: Output | Options, stderr?: Output, ignoreErrors = true) {
		const o: Options = "stdout" in stdout ? stdout : { stdout, stderr, ignoreErrors };
		if (!o.stdout || typeof o.stdout.write !== "function")
			throw codedError(
				"ERR_CONSOLE_WRITABLE_STREAM",
				"Console expects a writable stream instance for stdout",
				TypeError,
			);
		if (o.stderr && typeof o.stderr.write !== "function")
			throw codedError(
				"ERR_CONSOLE_WRITABLE_STREAM",
				"Console expects a writable stream instance for stderr",
				TypeError,
			);
		if (o.colorMode !== undefined && o.colorMode !== true && o.colorMode !== false && o.colorMode !== "auto")
			throw codedError("ERR_INVALID_ARG_VALUE", "Invalid colorMode", TypeError);
		if (o.colorMode !== undefined && o.inspectOptions?.colors !== undefined)
			throw codedError(
				"ERR_INCOMPATIBLE_OPTION_PAIR",
				'Option "inspectOptions.colors" cannot be used in combination with option "colorMode"',
				TypeError,
			);
		this.stdout = o.stdout;
		this.stderr = o.stderr ?? o.stdout;
		this.ignoreErrors = o.ignoreErrors ?? true;
		this.colorMode = o.colorMode ?? "auto";
		this.inspectOptions = o.inspectOptions ?? {};
		this.groupIndentation = o.groupIndentation ?? 2;
		if (!Number.isInteger(this.groupIndentation) || this.groupIndentation < 0 || this.groupIndentation > 1000)
			throw codedError("ERR_OUT_OF_RANGE", "groupIndentation must be between 0 and 1000", RangeError);
		for (const name of Object.getOwnPropertyNames(Console.prototype)) {
			if (name !== "constructor" && typeof (this as unknown as Record<string, unknown>)[name] === "function")
				Object.defineProperty(this, name, {
					value: (this as unknown as Record<string, Function>)[name].bind(this),
					writable: true,
					configurable: true,
				});
		}
	}
	private options(stream: Output): InspectOptions {
		return { colors: this.colorMode === "auto" ? colorEnabled(stream) : this.colorMode, ...this.inspectOptions };
	}
	private write(stream: Output, text: string): void {
		const prefix = " ".repeat(this.indent);
		text = prefix + text.replace(/\n(?!$)/g, `\n${prefix}`) + "\n";
		try {
			stream.write(text);
		} catch (e) {
			if (!this.ignoreErrors) throw e;
		}
	}
	log(...args: unknown[]): void {
		this.write(this.stdout, formatWithOptions(this.options(this.stdout), ...args));
	}
	info(...args: unknown[]): void {
		this.log(...args);
	}
	debug(...args: unknown[]): void {
		this.log(...args);
	}
	warn(...args: unknown[]): void {
		this.write(this.stderr, formatWithOptions(this.options(this.stderr), ...args));
	}
	error(...args: unknown[]): void {
		this.warn(...args);
	}
	dir(value: unknown, options?: InspectOptions): void {
		this.write(this.stdout, inspect(value, { ...this.options(this.stdout), customInspect: false, ...options }));
	}
	dirxml(...args: unknown[]): void {
		this.log(...args);
	}
	assert(condition: unknown, ...args: unknown[]): void {
		if (!condition)
			this.warn(`Assertion failed${args.length ? ": " + formatWithOptions(this.options(this.stderr), ...args) : ""}`);
	}
	trace(...args: unknown[]): void {
		const e = new Error();
		e.name = "Trace";
		e.message = formatWithOptions(this.options(this.stderr), ...args);
		const stack = e.stack ?? "";
		this.write(
			this.stderr,
			`Trace${e.message ? ": " + e.message : ""}\n${stack
				.split("\n")
				.filter((line) => !line.includes("node/lib/node/console.ts") && !/^Error/.test(line))
				.join("\n")}`,
		);
	}
	clear(): void {
		if (this.stdout.isTTY && process.env.TERM !== "dumb") this.stdout.write("\x1b[1;1H\x1b[0J");
	}
	count(label = "default"): void {
		label = String(label);
		const count = (this.counts.get(label) ?? 0) + 1;
		this.counts.set(label, count);
		this.log(`${label}: ${count}`);
	}
	countReset(label = "default"): void {
		label = String(label);
		if (!this.counts.has(label)) {
			process.emitWarning(`Count for '${label}' does not exist`);
			return;
		}
		this.counts.set(label, 0);
	}
	time(label = "default"): void {
		label = String(label);
		if (this.times.has(label)) {
			process.emitWarning(`Label '${label}' already exists for console.time()`);
			return;
		}
		this.times.set(label, core.hrtime());
	}
	timeLog(label = "default", ...args: unknown[]): void {
		label = String(label);
		const start = this.times.get(label);
		if (start === undefined) {
			process.emitWarning(`No such label '${label}' for console.timeLog()`);
			return;
		}
		const ms = Number(core.hrtime() - start) / 1e6;
		let time: string;
		if (ms >= 3600000) {
			const h = Math.floor(ms / 3600000),
				m = Math.floor(ms / 60000) % 60,
				s = (ms / 1000) % 60;
			time = `${h}:${String(m).padStart(2, "0")}:${s.toFixed(3).padStart(6, "0")} (h:mm:ss.mmm)`;
		} else if (ms >= 60000) {
			time = `${Math.floor(ms / 60000)}:${((ms / 1000) % 60).toFixed(3).padStart(6, "0")} (m:ss.mmm)`;
		} else if (ms >= 1000) time = `${(ms / 1000).toFixed(3)}s`;
		else time = `${Number(ms.toFixed(3))}ms`;
		this.log(`${label}: ${time}`, ...args);
	}
	timeEnd(label = "default"): void {
		label = String(label);
		this.timeLog(label);
		this.times.delete(label);
	}
	group(...args: unknown[]): void {
		if (args.length) this.log(...args);
		this.indent += this.groupIndentation;
	}
	groupCollapsed(...args: unknown[]): void {
		this.group(...args);
	}
	groupEnd(): void {
		this.indent = Math.max(0, this.indent - this.groupIndentation);
	}
	table(data: unknown, properties?: string[]): void {
		if (properties !== undefined && (!Array.isArray(properties) || properties.some((p) => typeof p !== "string")))
			throw invalidArgType("properties", "an Array of strings", properties);
		if (data === null || typeof data !== "object") {
			this.log(data);
			return;
		}
		const columns: string[] = [];
		const rows: { key: string; cells: Map<string, unknown> }[] = [];
		const add = (key: string, v: unknown): void => {
			const cells = new Map<string, unknown>();
			if (v !== null && typeof v === "object") {
				for (const prop of properties ?? (Reflect.ownKeys(v).filter((k) => typeof k === "string") as string[])) {
					if (!Object.hasOwn(v, prop)) continue;
					if (!columns.includes(prop)) columns.push(prop);
					cells.set(prop, (v as Record<string, unknown>)[prop]);
				}
			} else {
				if (!columns.includes("Values")) columns.push("Values");
				cells.set("Values", v);
			}
			rows.push({ key, cells });
		};
		if (data instanceof Map) {
			let i = 0;
			columns.push("Key", "Values");
			for (const [k, v] of data)
				rows.push({
					key: String(i++),
					cells: new Map([
						["Key", k],
						["Values", v],
					]),
				});
		} else if (data instanceof Set) {
			let i = 0;
			for (const v of data) add(String(i++), v);
		} else for (const key in data) if (Object.hasOwn(data, key)) add(key, (data as Record<string, unknown>)[key]);
		const headers = ["(index)", ...columns];
		const rendered = rows.map((row) => [
			row.key,
			...columns.map((c) =>
				row.cells.has(c)
					? inspect(row.cells.get(c), { depth: 0, maxArrayLength: 3, breakLength: Infinity, colors: false })
					: "",
			),
		]);
		const width = (s: string): number =>
			Array.from(stripVTControlCharacters(s)).reduce(
				(n, c) =>
					n +
					(/\p{Mark}/u.test(c)
						? 0
						: c.codePointAt(0)! >= 0x1100 &&
								/[\u1100-\u115f\u2329\u232a\u2e80-\ua4cf\uac00-\ud7a3\uf900-\ufaff\ufe10-\ufe19\ufe30-\ufe6f\uff00-\uff60\uffe0-\uffe6]/u.test(
									c,
								)
							? 2
							: 1),
				0,
			);
		const widths = headers.map((h, i) => Math.max(width(h), ...rendered.map((r) => width(r[i]))));
		const border = (left: string, mid: string, right: string): string =>
			left + widths.map((w) => "─".repeat(w + 2)).join(mid) + right;
		const line = (cells: string[], center = false): string =>
			"│" +
			cells
				.map((s, i) => {
					const gap = widths[i] - width(s),
						l = center ? Math.floor(gap / 2) : 0;
					return " " + " ".repeat(l) + s + " ".repeat(gap - l) + " ";
				})
				.join("│") +
			"│";
		this.write(
			this.stdout,
			[
				border("┌", "┬", "┐"),
				line(headers, true),
				border("├", "┼", "┤"),
				...rendered.map((r) => line(r)),
				border("└", "┴", "┘"),
			].join("\n"),
		);
	}
	profile(): void {}
	profileEnd(): void {}
	timeStamp(): void {}
}
const console = new Console({
	stdout: {
		write(text: string) {
			return process.stdout.write(text);
		},
		get isTTY() {
			return process.stdout.isTTY;
		},
	},
	stderr: {
		write(text: string) {
			return process.stderr.write(text);
		},
		get isTTY() {
			return process.stderr.isTTY;
		},
	},
});
Object.assign(console, { Console });
export const log = console.log,
	info = console.info,
	debug = console.debug,
	warn = console.warn,
	error = console.error,
	dir = console.dir,
	dirxml = console.dirxml,
	table = console.table,
	time = console.time,
	timeEnd = console.timeEnd,
	timeLog = console.timeLog,
	count = console.count,
	countReset = console.countReset,
	group = console.group,
	groupCollapsed = console.groupCollapsed,
	groupEnd = console.groupEnd,
	assert = console.assert,
	trace = console.trace,
	clear = console.clear,
	profile = console.profile,
	profileEnd = console.profileEnd,
	timeStamp = console.timeStamp;
export default console;
