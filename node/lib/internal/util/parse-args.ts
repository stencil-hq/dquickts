import { codedError, invalidArgType, invalidArgValue } from "internal:errors";
interface Option {
	type: "boolean" | "string";
	short?: string;
	multiple?: boolean;
	default?: boolean | string | boolean[] | string[];
}
interface Config {
	args?: string[];
	options?: Record<string, Option>;
	strict?: boolean;
	allowPositionals?: boolean;
	allowNegative?: boolean;
	tokens?: boolean;
}
type Token =
	| {
			kind: "option";
			name: string;
			rawName: string;
			index: number;
			value: string | undefined;
			inlineValue: boolean | undefined;
	  }
	| { kind: "positional"; index: number; value: string }
	| { kind: "option-terminator"; index: number };
export function parseArgs(config: Config = {}): {
	values: Record<string, unknown>;
	positionals: string[];
	tokens?: Token[];
} {
	if (config === null || typeof config !== "object") throw invalidArgType("config", "of type object", config);
	const options = config.options ?? {};
	const args = config.args ?? process.argv.slice(2);
	const strict = config.strict ?? true;
	const allow = config.allowPositionals ?? !strict;
	const values: Record<string, unknown> = Object.create(null);
	const positionals: string[] = [];
	const tokens: Token[] = [];
	const shorts: Record<string, string> = Object.create(null);
	if (!Array.isArray(args)) throw invalidArgType("args", "an Array", args);
	for (const key of ["strict", "allowPositionals", "allowNegative", "tokens"] as const)
		if (config[key] !== undefined && typeof config[key] !== "boolean")
			throw invalidArgType(`config.${key}`, "of type boolean", config[key]);
	for (const name in options) {
		const o = options[name];
		if (!o || typeof o !== "object") throw invalidArgType(`options.${name}`, "of type object", o);
		if (o.type !== "boolean" && o.type !== "string") throw invalidArgValue(`options.${name}.type`, o.type);
		if (o.short !== undefined) {
			if (typeof o.short !== "string" || o.short.length !== 1) throw invalidArgValue(`options.${name}.short`, o.short);
			shorts[o.short] = name;
		}
		if (o.multiple !== undefined && typeof o.multiple !== "boolean")
			throw invalidArgType(`options.${name}.multiple`, "of type boolean", o.multiple);
		if (o.default !== undefined) {
			const ds = o.multiple ? o.default : [o.default];
			if (!Array.isArray(ds) || ds.some((v) => typeof v !== o.type))
				throw invalidArgType(
					`options.${name}.default`,
					o.multiple ? `an array of ${o.type}` : `of type ${o.type}`,
					o.default,
				);
		}
	}
	let ended = false;
	for (let i = 0; i < args.length; i++) {
		const arg = args[i];
		if (typeof arg !== "string") throw invalidArgType(`args[${i}]`, "of type string", arg);
		if (!ended && arg === "--") {
			tokens.push({ kind: "option-terminator", index: i });
			ended = true;
			continue;
		}
		if (ended || arg === "-" || !arg.startsWith("-")) {
			if (!allow)
				throw codedError(
					"ERR_PARSE_ARGS_UNEXPECTED_POSITIONAL",
					`Unexpected argument '${arg}'. This command does not take positional arguments`,
				);
			positionals.push(arg);
			tokens.push({ kind: "positional", index: i, value: arg });
			continue;
		}
		const long = arg.startsWith("--");
		let offset = long ? 2 : 1;
		while (offset < arg.length) {
			const index = i;
			let rawName: string, name: string, value: string | undefined, inlineValue: boolean | undefined;
			if (long) {
				const equal = arg.indexOf("=", 2);
				name = arg.slice(2, equal < 0 ? undefined : equal);
				rawName = `--${name}`;
				if (equal >= 0) {
					value = arg.slice(equal + 1);
					inlineValue = true;
				}
				offset = arg.length;
			} else {
				const short = arg[offset++];
				name = shorts[short] ?? short;
				rawName = `-${short}`;
			}
			let opt = Object.hasOwn(options, name) ? options[name] : undefined;
			let negative = false;
			if (!opt && config.allowNegative && name.startsWith("no-") && options[name.slice(3)]?.type === "boolean") {
				negative = true;
				opt = options[name.slice(3)];
			}
			if (strict && !opt)
				throw codedError(
					"ERR_PARSE_ARGS_UNKNOWN_OPTION",
					`Unknown option '${rawName}'${allow ? "" : ". To specify a positional argument starting with a '-', place it at the end of the command after '--'"}`,
				);
			if (opt?.type === "string" && value === undefined) {
				if (!long && offset < arg.length) {
					value = arg.slice(offset);
					inlineValue = true;
					offset = arg.length;
				} else {
					value = args[++i];
					inlineValue = false;
				}
				if (strict && (value === undefined || (value.startsWith("-") && value !== "-")))
					throw codedError(
						"ERR_PARSE_ARGS_INVALID_OPTION_VALUE",
						`Option '${rawName} <value>' argument ${value === undefined ? "missing" : "is ambiguous"}`,
					);
			}
			if (strict && opt?.type === "boolean" && value !== undefined)
				throw codedError("ERR_PARSE_ARGS_INVALID_OPTION_VALUE", `Option '${rawName}' does not take an argument`);
			const store = negative ? name.slice(3) : name;
			tokens.push({ kind: "option", name: store, rawName, index, value, inlineValue });
			const v = negative ? false : (value ?? true);
			if (opt?.multiple) {
				if (!Object.hasOwn(values, store)) values[store] = [];
				(values[store] as unknown[]).push(v);
			} else values[store] = v;
		}
	}
	for (const name in options)
		if (!Object.hasOwn(values, name) && options[name].default !== undefined)
			values[name] = Array.isArray(options[name].default)
				? (options[name].default as unknown[]).slice()
				: options[name].default;
	return config.tokens ? { values, positionals, tokens } : { values, positionals };
}
