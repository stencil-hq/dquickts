// internal:transpile/options — Bun.Transpiler option normalization and tsconfig handling.
// Small and dependency-free: the Bun.Transpiler constructor uses it eagerly, while the
// transformer itself (internal:transpile/index) loads on the first transform or scan.

import { codedError, invalidArgType, invalidArgValue, notSupported } from "internal:errors";

export type Loader = "js" | "jsx" | "ts" | "tsx";

export const LOADERS: readonly Loader[] = ["js", "jsx", "ts", "tsx"];

export interface TSConfigCompilerOptions {
	paths?: Record<string, string[]>;
	baseUrl?: string;
	jsx?: string;
	jsxFactory?: string;
	jsxFragmentFactory?: string;
	jsxImportSource?: string;
	useDefineForClassFields?: boolean;
	importsNotUsedAsValues?: string;
	verbatimModuleSyntax?: boolean;
	preserveValueImports?: boolean;
	experimentalDecorators?: boolean;
	emitDecoratorMetadata?: boolean;
	moduleSuffixes?: unknown;
}

export interface TSConfig {
	extends?: string | string[];
	compilerOptions?: TSConfigCompilerOptions;
}

export interface TranspilerOptions {
	define?: Record<string, string>;
	loader?: Loader;
	target?: "browser" | "bun" | "node";
	tsconfig?: string | TSConfig;
	macro?: Record<string, Record<string, string>>;
	autoImportJSX?: boolean;
	allowBunRuntime?: boolean;
	exports?: { eliminate?: string[]; replace?: Record<string, string | [string, unknown]> };
	treeShaking?: boolean;
	trimUnusedImports?: boolean;
	jsxOptimizationInline?: boolean;
	minifyWhitespace?: boolean;
	deadCodeElimination?: boolean;
	inline?: boolean;
	logLevel?: "verbose" | "debug" | "info" | "warn" | "error";
	replMode?: boolean;
}

/** JSX settings. `development: null` means "development unless NODE_ENV=production". */
export interface JsxConfig {
	runtime: "automatic" | "classic" | "preserve";
	development: boolean | null;
	factory: string;
	fragment: string;
	importSource: string;
}

export interface DefineEntry {
	parts: string[];
	value: string;
}

/** Everything the transformer needs, independent of the per-call loader. */
export interface TranspileConfig {
	loader: Loader;
	jsx: JsxConfig;
	legacyDecorators: boolean;
	keepUnusedImports: boolean;
	trimUnusedImports: boolean;
	define: DefineEntry[];
	eliminate: Set<string>;
	replace: Map<string, { name: string; value: unknown }>;
	filePath: string;
}

export function defaultJsxConfig(): JsxConfig {
	return {
		runtime: "automatic",
		development: null,
		factory: "React.createElement",
		fragment: "React.Fragment",
		importSource: "react",
	};
}

/** Apply tsconfig compilerOptions JSX settings to `jsx` (in place). */
export function applyCompilerOptionsJsx(jsx: JsxConfig, co: TSConfigCompilerOptions): void {
	switch (co.jsx) {
		case undefined:
			break;
		case "react":
			jsx.runtime = "classic";
			break;
		case "react-jsx":
			jsx.runtime = "automatic";
			jsx.development = false;
			break;
		case "react-jsxdev":
			jsx.runtime = "automatic";
			jsx.development = true;
			break;
		case "preserve":
		case "react-native":
			jsx.runtime = "preserve";
			break;
		default:
			break;
	}
	if (typeof co.jsxFactory === "string" && co.jsxFactory) jsx.factory = co.jsxFactory;
	if (typeof co.jsxFragmentFactory === "string" && co.jsxFragmentFactory) jsx.fragment = co.jsxFragmentFactory;
	if (typeof co.jsxImportSource === "string" && co.jsxImportSource) jsx.importSource = co.jsxImportSource;
}

/** Parse JSON with comments and trailing commas (tsconfig.json syntax). */
export function parseJSONC(text: string): unknown {
	return JSON.parse(stripTrailingCommas(stripComments(text)));
}

function stringEnd(text: string, start: number): number {
	let j = start + 1;
	while (j < text.length && text[j] !== '"') j += text[j] === "\\" ? 2 : 1;
	return j + 1;
}

function stripComments(text: string): string {
	const parts: string[] = [];
	let from = 0;
	let i = 0;
	while (i < text.length) {
		const c = text[i];
		if (c === '"') {
			i = stringEnd(text, i);
		} else if (c === "/" && (text[i + 1] === "/" || text[i + 1] === "*")) {
			parts.push(text.slice(from, i));
			if (text[i + 1] === "/") {
				while (i < text.length && text[i] !== "\n") i++;
			} else {
				const end = text.indexOf("*/", i + 2);
				i = end === -1 ? text.length : end + 2;
			}
			from = i;
		} else {
			i++;
		}
	}
	parts.push(text.slice(from));
	return parts.join("");
}

function stripTrailingCommas(text: string): string {
	const parts: string[] = [];
	let from = 0;
	let i = 0;
	while (i < text.length) {
		const c = text[i];
		if (c === '"') {
			i = stringEnd(text, i);
			continue;
		}
		if (c === ",") {
			let j = i + 1;
			while (j < text.length && (text[j] === " " || text[j] === "\t" || text[j] === "\n" || text[j] === "\r")) j++;
			if (text[j] === "}" || text[j] === "]") {
				parts.push(text.slice(from, i));
				from = i + 1;
			}
		}
		i++;
	}
	parts.push(text.slice(from));
	return parts.join("");
}

function isPlainObject(v: unknown): v is Record<string, unknown> {
	return typeof v === "object" && v !== null && !Array.isArray(v);
}

function parseTsconfigOption(value: unknown): TSConfigCompilerOptions {
	let config: unknown = value;
	if (typeof value === "string") {
		try {
			config = parseJSONC(value);
		} catch (err) {
			throw codedError("ERR_INVALID_ARG_VALUE", `Invalid tsconfig: ${(err as Error).message}`, TypeError);
		}
	}
	if (config === undefined || config === null) return {};
	if (!isPlainObject(config)) throw invalidArgType("tsconfig", "a string or an object", value);
	const co = config.compilerOptions;
	if (co === undefined || co === null) return {};
	if (!isPlainObject(co)) throw invalidArgType("tsconfig.compilerOptions", "an object", co);
	return co as TSConfigCompilerOptions;
}

function parseDefineKey(key: string): string[] {
	const parts = key.split(".");
	for (const p of parts) {
		if (!/^[A-Za-z_$][\w$]*$/.test(p))
			throw invalidArgValue("define", key, "must be an identifier or a member expression");
	}
	return parts;
}

/** Validate and normalize `new Bun.Transpiler(options)` options. */
export function normalizeTranspilerOptions(options: TranspilerOptions | undefined): TranspileConfig {
	if (options !== undefined && options !== null && typeof options !== "object") {
		throw invalidArgType("options", "an object", options);
	}
	const opts: TranspilerOptions = options ?? {};
	const loader = opts.loader ?? "tsx";
	if (!LOADERS.includes(loader)) {
		throw invalidArgValue("loader", loader, 'must be one of "js", "jsx", "ts" or "tsx"');
	}
	if (opts.replMode) throw notSupported("Bun.Transpiler replMode");

	const jsx = defaultJsxConfig();
	const co = parseTsconfigOption(opts.tsconfig);
	applyCompilerOptionsJsx(jsx, co);

	const define: DefineEntry[] = [];
	if (opts.define !== undefined && opts.define !== null) {
		if (!isPlainObject(opts.define)) throw invalidArgType("define", "an object", opts.define);
		for (const key in opts.define) {
			const value = opts.define[key];
			if (typeof value !== "string") throw invalidArgType(`define.${key}`, "of type string", value);
			define.push({ parts: parseDefineKey(key), value });
		}
	}

	const eliminate = new Set<string>();
	const replace = new Map<string, { name: string; value: unknown }>();
	const exp = opts.exports;
	if (exp !== undefined && exp !== null) {
		if (!isPlainObject(exp)) throw invalidArgType("exports", "an object", exp);
		if (exp.eliminate !== undefined) {
			if (!Array.isArray(exp.eliminate)) throw invalidArgType("exports.eliminate", "an array", exp.eliminate);
			for (const name of exp.eliminate) {
				if (typeof name !== "string") throw invalidArgType("exports.eliminate", "an array of strings", name);
				eliminate.add(name);
			}
		}
		if (exp.replace !== undefined) {
			if (!isPlainObject(exp.replace)) throw invalidArgType("exports.replace", "an object", exp.replace);
			for (const name in exp.replace) {
				const value: unknown = exp.replace[name];
				if (Array.isArray(value)) {
					if (value.length !== 2 || typeof value[0] !== "string") {
						throw invalidArgValue(`exports.replace.${name}`, value, "must be a value or a [name, value] pair");
					}
					replace.set(name, { name: value[0], value: value[1] });
				} else {
					replace.set(name, { name, value });
				}
			}
		}
	}

	return {
		loader,
		jsx,
		legacyDecorators: co.experimentalDecorators === true,
		keepUnusedImports: co.verbatimModuleSyntax === true || co.preserveValueImports === true,
		trimUnusedImports: opts.trimUnusedImports === true,
		define,
		eliminate,
		replace,
		filePath: "",
	};
}
