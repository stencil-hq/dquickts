import * as core from "qbun:core";
import types from "node:util/types";
export interface InspectOptions {
	showHidden?: boolean;
	depth?: number | null;
	colors?: boolean;
	customInspect?: boolean;
	showProxy?: boolean;
	maxArrayLength?: number | null;
	maxStringLength?: number | null;
	breakLength?: number;
	compact?: boolean | number;
	sorted?: boolean | ((a: string, b: string) => number);
	getters?: boolean | "get" | "set";
	numericSeparator?: boolean;
}
const custom = Symbol.for("nodejs.util.inspect.custom");
const defaults: InspectOptions = {
	showHidden: false,
	depth: 2,
	colors: false,
	customInspect: true,
	showProxy: false,
	maxArrayLength: 100,
	maxStringLength: 10000,
	breakLength: 80,
	compact: 3,
	sorted: false,
	getters: false,
	numericSeparator: false,
};
const colors: Record<string, [number, number]> = {
	reset: [0, 0],
	bold: [1, 22],
	dim: [2, 22],
	italic: [3, 23],
	underline: [4, 24],
	blink: [5, 25],
	inverse: [7, 27],
	hidden: [8, 28],
	strikethrough: [9, 29],
	doubleunderline: [21, 24],
	black: [30, 39],
	red: [31, 39],
	green: [32, 39],
	yellow: [33, 39],
	blue: [34, 39],
	magenta: [35, 39],
	cyan: [36, 39],
	white: [37, 39],
	gray: [90, 39],
	grey: [90, 39],
	redBright: [91, 39],
	greenBright: [92, 39],
	yellowBright: [93, 39],
	blueBright: [94, 39],
	magentaBright: [95, 39],
	cyanBright: [96, 39],
	whiteBright: [97, 39],
	bgBlack: [40, 49],
	bgRed: [41, 49],
	bgGreen: [42, 49],
	bgYellow: [43, 49],
	bgBlue: [44, 49],
	bgMagenta: [45, 49],
	bgCyan: [46, 49],
	bgWhite: [47, 49],
};
const styles: Record<string, string> = {
	special: "cyan",
	number: "yellow",
	bigint: "yellow",
	boolean: "yellow",
	undefined: "grey",
	null: "bold",
	string: "green",
	symbol: "green",
	date: "magenta",
	regexp: "red",
	module: "underline",
};
export function stripVTControlCharacters(s: string): string {
	if (typeof s !== "string") throw new TypeError('The "str" argument must be of type string');
	return s.replace(
		/[\u001b\u009b][[\]()#;?]*(?:(?:(?:[a-zA-Z\d]*(?:;[-a-zA-Z\d\/#&.:=?%@~_]*)*)?\u0007)|(?:(?:\d{1,4}(?:[;:]\d{0,4})*)?[\dA-PR-TZcf-nq-uy=><~]))/g,
		"",
	);
}
function quote(s: string): string {
	const q = !s.includes("'") ? "'" : !s.includes('"') ? '"' : !s.includes("`") && !s.includes("${") ? "`" : "'";
	return (
		q +
		s
			.replace(/\\/g, "\\\\")
			.replace(
				/[\x00-\x1f\x7f]/g,
				(c) =>
					({ "\n": "\\n", "\r": "\\r", "\t": "\\t", "\b": "\\b", "\f": "\\f" })[c] ??
					`\\x${c.charCodeAt(0).toString(16).padStart(2, "0")}`,
			)
			.replace(new RegExp(q, "g"), `\\${q}`) +
		q
	);
}
export function inspect(
	value: unknown,
	options?: InspectOptions | boolean,
	depth?: number | null,
	useColors?: boolean,
): string {
	const o = {
		...inspect.defaultOptions,
		...(typeof options === "boolean"
			? { showHidden: options, depth: depth ?? 2, colors: useColors ?? false }
			: options),
	};
	const seen = new Map<object, number>();
	const active = new Set<object>();
	const circular = new Set<object>();
	let id = 0;
	const paint = (s: string, type: string): string => {
		const c = inspect.colors[inspect.styles[type]];
		return o.colors && c ? `\x1b[${c[0]}m${s}\x1b[${c[1]}m` : s;
	};
	function render(v: unknown, level: number): string {
		if (v === null) return paint("null", "null");
		if (typeof v === "string") {
			const max = o.maxStringLength ?? Infinity;
			return paint(quote(v.slice(0, max)), "string") + (v.length > max ? `... ${v.length - max} more characters` : "");
		}
		if (typeof v === "number" || typeof v === "bigint") {
			let s = Object.is(v, -0) ? "-0" : String(v);
			if (o.numericSeparator) s = s.replace(/\B(?=(\d{3})+(?!\d))/g, "_");
			return paint(s + (typeof v === "bigint" ? "n" : ""), typeof v);
		}
		if (typeof v === "boolean" || typeof v === "undefined" || typeof v === "symbol") return paint(String(v), typeof v);
		const obj = v as object & Record<PropertyKey, unknown>;
		if (active.has(obj)) {
			circular.add(obj);
			return paint(`[Circular *${seen.get(obj)}]`, "special");
		}
		const target = core.proxyTarget(obj);
		if (target === null) return paint("<Revoked Proxy>", "special");
		if (o.customInspect && typeof obj[custom] === "function" && obj[custom] !== inspect) {
			const r = (obj[custom] as Function).call(
				obj,
				o.depth === null ? null : (o.depth ?? 2) - level,
				{ ...o, stylize: paint },
				inspect,
			);
			if (r !== obj) return typeof r === "string" ? r : render(r, level);
		}
		if (target !== undefined) {
			if (!seen.has(obj)) seen.set(obj, ++id);
			active.add(obj);
			const details = o.showProxy ? core.proxyDetails(obj) : undefined;
			const result = o.showProxy
				? `Proxy [ ${render(details[0], level + 1)}, ${render(details[1], level + 1)} ]`
				: render(target, level);
			active.delete(obj);
			return circular.has(obj) ? `<ref *${seen.get(obj)}> ${result}` : result;
		}
		const tag = Object.prototype.toString.call(obj).slice(8, -1);
		const ctor = Object.getPrototypeOf(obj)?.constructor?.name;
		if (o.depth !== null && level > (o.depth ?? 2))
			return paint(Array.isArray(obj) ? "[Array]" : `[${ctor || tag}]`, "special");
		if (!seen.has(obj)) seen.set(obj, ++id);
		active.add(obj);
		let open = "{",
			close = "}",
			prefix = "",
			parts: string[] = [];
		const excluded = new Set<PropertyKey>();
		const child = (x: unknown): string => render(x, level + 1);
		const limit = o.maxArrayLength ?? Infinity;
		if (Array.isArray(obj)) {
			open = "[";
			close = "]";
			let holes = 0;
			const a = obj as unknown as unknown[];
			for (let i = 0; i < Math.min(a.length, limit); i++) {
				excluded.add(String(i));
				if (!(i in a)) {
					holes++;
					continue;
				}
				if (holes) {
					parts.push(paint(`<${holes} empty item${holes === 1 ? "" : "s"}>`, "undefined"));
					holes = 0;
				}
				parts.push(child(a[i]));
			}
			if (holes) parts.push(paint(`<${holes} empty item${holes === 1 ? "" : "s"}>`, "undefined"));
			if (a.length > limit) parts.push(`... ${a.length - limit} more items`);
			if (!o.showHidden) excluded.add("length");
		} else if (types.isMap(obj)) {
			const m = obj as unknown as Map<unknown, unknown>;
			prefix = `Map(${m.size}) `;
			let n = 0;
			for (const [k, x] of m) {
				if (n++ >= limit) break;
				parts.push(`${child(k)} => ${child(x)}`);
			}
			if (m.size > limit) parts.push(`... ${m.size - limit} more items`);
		} else if (types.isSet(obj)) {
			const s = obj as unknown as Set<unknown>;
			prefix = `Set(${s.size}) `;
			let n = 0;
			for (const x of s) {
				if (n++ >= limit) break;
				parts.push(child(x));
			}
			if (s.size > limit) parts.push(`... ${s.size - limit} more items`);
		} else if (types.isWeakMap(obj) || types.isWeakSet(obj)) {
			prefix = `${tag} `;
			parts.push(paint("<items unknown>", "special"));
		} else if (types.isMapIterator(obj) || types.isSetIterator(obj)) {
			prefix = `[${tag}] `;
			parts.push(paint("<items unknown>", "special"));
		} else if (types.isTypedArray(obj) && ctor === "Buffer") {
			const a = obj as unknown as Uint8Array;
			prefix = `<Buffer ${Array.from(a.slice(0, 50), (n) => n.toString(16).padStart(2, "0")).join(" ")}${a.length > 50 ? ` ... ${a.length - 50} more bytes` : ""}>`;
		} else if (types.isTypedArray(obj)) {
			const a = obj as unknown as Uint8Array;
			prefix = `${ctor}(${a.length}) `;
			open = "[";
			close = "]";
			for (let i = 0; i < Math.min(a.length, limit); i++) {
				excluded.add(String(i));
				parts.push(child(a[i]));
			}
			if (a.length > limit) parts.push(`... ${a.length - limit} more items`);
		} else if (types.isAnyArrayBuffer(obj)) {
			const a = new Uint8Array(obj as unknown as ArrayBuffer);
			prefix = `${tag} `;
			parts.push(
				`[Uint8Contents]: <${Array.from(a.slice(0, limit), (n) => n.toString(16).padStart(2, "0")).join(" ")}${a.length > limit ? " ..." : ""}>`,
				`byteLength: ${a.byteLength}`,
			);
		} else if (types.isDataView(obj)) {
			const d = obj as unknown as DataView;
			prefix = "DataView ";
			parts.push(`byteLength: ${d.byteLength}`, `byteOffset: ${d.byteOffset}`, `buffer: ${child(d.buffer)}`);
		} else if (types.isDate(obj)) {
			const d = obj as unknown as Date;
			prefix = paint(Number.isNaN(d.getTime()) ? "Invalid Date" : d.toISOString(), "date");
		} else if (types.isRegExp(obj)) {
			prefix = paint(String(obj), "regexp");
			if (!o.showHidden) excluded.add("lastIndex");
		} else if (types.isNativeError(obj)) {
			const e = obj as unknown as Error;
			prefix = e.stack || `${e.name}: ${e.message}`;
			if (!prefix.startsWith(e.name)) prefix = `${e.name}: ${e.message}\n${prefix}`;
			excluded.add("stack");
			excluded.add("message");
			if ("cause" in obj) parts.push(`[cause]: ${child(obj.cause)}`);
			excluded.add("cause");
			if ("errors" in obj) parts.push(`[errors]: ${child(obj.errors)}`);
			excluded.add("errors");
		} else if (types.isPromise(obj)) {
			prefix = "Promise ";
			const [state, result] = core.promiseState(obj);
			parts.push(
				state === 0
					? paint("<pending>", "special")
					: state === 2
						? `${paint("<rejected>", "special")} ${child(result)}`
						: child(result),
			);
		} else if (types.isBoxedPrimitive(obj)) {
			prefix = `[${tag}: ${render((obj.valueOf as Function).call(obj), level)}]`;
			if (types.isStringObject(obj)) {
				for (let i = 0; i < String(obj).length; i++) excluded.add(String(i));
				excluded.add("length");
			}
		} else if (typeof obj === "function") {
			const f = obj as unknown as Function;
			prefix = `[${tag === "Function" ? "Function" : tag}${f.name ? `: ${f.name}` : " (anonymous)"}]`;
		} else if (Object.getPrototypeOf(obj) === null)
			prefix = types.isModuleNamespaceObject(obj) ? "[Module: null prototype] " : "[Object: null prototype] ";
		else if (ctor && ctor !== "Object") prefix = `${ctor} `;
		let keys = Reflect.ownKeys(obj).filter(
			(k) =>
				!excluded.has(k) &&
				!((Array.isArray(obj) || types.isTypedArray(obj)) && typeof k === "string" && /^(0|[1-9]\d*)$/.test(k)) &&
				(o.showHidden || Object.getOwnPropertyDescriptor(obj, k)?.enumerable),
		);
		if (o.sorted)
			keys.sort(
				typeof o.sorted === "function"
					? (a, b) => (o.sorted as (a: string, b: string) => number)(String(a), String(b))
					: (a, b) => (String(a) < String(b) ? -1 : String(a) > String(b) ? 1 : 0),
			);
		for (const key of keys) {
			const d = Object.getOwnPropertyDescriptor(obj, key)!;
			let val: string;
			if ("value" in d) val = child(d.value);
			else if (d.get && (o.getters === true || (o.getters === "get" && !d.set) || (o.getters === "set" && d.set))) {
				try {
					val = `[Getter${d.set ? "/Setter" : ""}: ${child(d.get.call(obj))}]`;
				} catch (e) {
					val = `[Getter: <Inspection threw (${(e as Error).message})>]`;
				}
			} else val = paint(`[${d.get ? "Getter" : ""}${d.get && d.set ? "/" : ""}${d.set ? "Setter" : ""}]`, "special");
			const name =
				typeof key === "symbol"
					? `[${paint(String(key), "symbol")}]`
					: !d.enumerable
						? `[${key}]`
						: /^[a-zA-Z_$][\w$]*$/.test(key)
							? key
							: paint(quote(key), "string");
			parts.push(`${name}: ${val}`);
		}
		if (o.sorted && (types.isMap(obj) || types.isSet(obj)))
			parts.sort(typeof o.sorted === "function" ? o.sorted : undefined);
		active.delete(obj);
		let result: string;
		if (parts.length === 0) result = prefix && !prefix.endsWith(" ") ? prefix : `${prefix}${open}${close}`;
		else {
			if (prefix && !prefix.endsWith(" ")) prefix += " ";
			const one = `${prefix}${open} ${parts.join(", ")} ${close}`;
			result =
				o.compact !== false &&
				stripVTControlCharacters(one).length + level * 2 <= (o.breakLength ?? 80) &&
				!one.includes("\n")
					? one
					: `${prefix}${open}\n${"  ".repeat(level + 1)}${parts.join(`,\n${"  ".repeat(level + 1)}`)}\n${"  ".repeat(level)}${close}`;
		}
		return circular.has(obj) ? `<ref *${seen.get(obj)}> ${result}` : result;
	}
	return render(value, 0);
}
inspect.custom = custom;
inspect.defaultOptions = defaults;
inspect.colors = colors;
inspect.styles = styles;
inspect.replDefaults = { ...defaults, colors: true };
export function formatWithOptions(options: InspectOptions, ...args: unknown[]): string {
	if (!args.length) return "";
	if (typeof args[0] !== "string") return args.map((v) => (typeof v === "string" ? v : inspect(v, options))).join(" ");
	let index = 1;
	const text = args[0].replace(/%[%sdifjoOc]/g, (m) => {
		if (m === "%%") return "%";
		if (index >= args.length) return m;
		const v = args[index++];
		switch (m) {
			case "%s":
				return typeof v === "object" && v !== null
					? inspect(v, { ...options, depth: 0, colors: false, compact: 3 })
					: String(v);
			case "%d":
				return typeof v === "bigint" ? `${v}n` : String(Number(v));
			case "%i":
				return typeof v === "bigint" ? `${v}n` : String(parseInt(String(v), 10));
			case "%f":
				return String(parseFloat(String(v)));
			case "%j":
				try {
					return JSON.stringify(v);
				} catch (e) {
					if ((e as Error).message.toLowerCase().includes("circular")) return "[Circular]";
					throw e;
				}
			case "%o":
				return inspect(v, { ...options, showHidden: true, showProxy: true, depth: 4 });
			case "%O":
				return inspect(v, options);
			case "%c":
				return "";
			default:
				return m;
		}
	});
	return (
		text +
		(index < args.length
			? " " +
				args
					.slice(index)
					.map((v) => (typeof v === "string" ? v : inspect(v, options)))
					.join(" ")
			: "")
	);
}
export function format(...args: unknown[]): string {
	return formatWithOptions({}, ...args);
}
