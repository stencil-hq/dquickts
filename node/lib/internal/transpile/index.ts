// internal:transpile — TypeScript/JSX to JavaScript for Bun.Transpiler and the module loader.
//
// Built on the vendored sucrase parser and token transformer (internal:thirdparty/sucrase), run
// with ES transforms disabled (the output keeps modern syntax and ES modules) plus qbun
// extensions in the vendored code: namespaces with values, TypeScript legacy decorators
// (experimentalDecorators) and parameter decorators. Standard decorators pass through (the
// engine implements them).

import { dirname, isAbsolute, join, resolve } from "node:path";
import * as core from "qbun:core";
import { parse } from "internal:thirdparty/sucrase/parser/index";
import NameManager from "internal:thirdparty/sucrase/NameManager";
import { HelperManager } from "internal:thirdparty/sucrase/HelperManager";
import TokenProcessor from "internal:thirdparty/sucrase/TokenProcessor";
import RootTransformer from "internal:thirdparty/sucrase/transformers/RootTransformer";
import ReactDisplayNameTransformer from "internal:thirdparty/sucrase/transformers/ReactDisplayNameTransformer";
import identifyShadowedGlobals from "internal:thirdparty/sucrase/identifyShadowedGlobals";
import getTSImportedNames from "internal:thirdparty/sucrase/util/getTSImportedNames";
import { TokenType as tt } from "internal:thirdparty/sucrase/parser/tokenizer/types";
import { ContextualKeyword } from "internal:thirdparty/sucrase/parser/tokenizer/keywords";
import { IdentifierRole, isDeclaration } from "internal:thirdparty/sucrase/parser/tokenizer/index";
import { DefineTransformer, ExportsTransformer, exportedDeclarationNames } from "internal:transpile/transformers";
import type { SucraseToken, SucraseTokenProcessor } from "internal:transpile/transformers";
import { applyCompilerOptionsJsx, defaultJsxConfig, parseJSONC } from "internal:transpile/options";
import type { JsxConfig, Loader, TranspileConfig, TSConfig, TSConfigCompilerOptions } from "internal:transpile/options";
import { BuildMessage } from "internal:transpile/errors";

export { BuildMessage };

export type ImportKind =
	| "import-statement"
	| "require-call"
	| "require-resolve"
	| "dynamic-import"
	| "import-rule"
	| "url-token"
	| "internal"
	| "entry-point-run"
	| "entry-point-build";

export interface Import {
	path: string;
	kind: ImportKind;
}

interface SucraseFile {
	tokens: SucraseToken[];
	scopes: unknown[];
}

interface ResolvedJsx {
	runtime: "automatic" | "classic" | "preserve";
	development: boolean;
	factory: string;
	fragment: string;
	importSource: string;
}

interface TransformResult {
	code: string;
	/** The source has `import`/`export` statements. */
	hasESM: boolean;
	/** The source references module, exports or require, or uses `export =`/`import x = require()`. */
	usesCommonJS: boolean;
}

function nodeEnv(): string | undefined {
	return typeof process === "undefined" ? undefined : process.env.NODE_ENV;
}

/** The JSX settings for one source: config, then `@jsx*` pragma comments. */
function resolveJsx(base: JsxConfig, code: string, tokens: SucraseToken[]): ResolvedJsx {
	const jsx: JsxConfig = { ...base };
	if (code.includes("@jsx")) {
		let sawRuntime = false;
		let sawFactory = false;
		let sawImportSource = false;
		const apply = (gap: string): void => {
			for (const m of gap.matchAll(/@(jsxRuntime|jsxImportSource|jsxFrag|jsx)\s+([^\s*]+)/g)) {
				const value = m[2];
				switch (m[1]) {
					case "jsxRuntime":
						if (value === "classic" || value === "automatic") {
							jsx.runtime = value;
							sawRuntime = true;
						}
						break;
					case "jsxImportSource":
						jsx.importSource = value;
						sawImportSource = true;
						break;
					case "jsxFrag":
						jsx.fragment = value;
						break;
					default:
						jsx.factory = value;
						sawFactory = true;
						break;
				}
			}
		};
		// Comments only live between tokens (strings, templates and JSX text are tokens).
		let prev = 0;
		for (const token of tokens) {
			if (token.start > prev) {
				const gap = code.slice(prev, token.start);
				if (gap.includes("@jsx")) apply(gap);
			}
			prev = token.end;
		}
		if (prev < code.length) {
			const gap = code.slice(prev);
			if (gap.includes("@jsx")) apply(gap);
		}
		if (!sawRuntime && jsx.runtime !== "preserve") {
			if (sawFactory) jsx.runtime = "classic";
			else if (sawImportSource) jsx.runtime = "automatic";
		}
	}
	return {
		runtime: jsx.runtime,
		development: jsx.development ?? nodeEnv() !== "production",
		factory: jsx.factory,
		fragment: jsx.fragment,
		importSource: jsx.importSource,
	};
}

/** Module syntax used by the source, from its tokens. */
function moduleKind(code: string, tokens: SucraseToken[]): { hasESM: boolean; usesCommonJS: boolean } {
	let hasESM = false;
	let usesCommonJS = false;
	for (let i = 0; i < tokens.length; i++) {
		const token = tokens[i];
		if (token.scopeDepth !== 0 && token.type !== tt.name) continue;
		if (token.type === tt._import && token.scopeDepth === 0) {
			const next = tokens[i + 1];
			if (next === undefined || next.type === tt.parenL || next.type === tt.dot) continue;
			if (tokens[i + 2]?.type === tt.eq) usesCommonJS = true;
			else hasESM = true;
		} else if (token.type === tt._export && token.scopeDepth === 0) {
			if (tokens[i + 1]?.type === tt.eq) usesCommonJS = true;
			else hasESM = true;
		} else if (token.type === tt.name && token.identifierRole === IdentifierRole.Access) {
			const name = code.slice(token.start, token.end);
			if (name === "module" || name === "exports" || name === "require") usesCommonJS = true;
		}
	}
	return { hasESM, usesCommonJS };
}

interface RunOptions {
	loader: Loader;
	config: TranspileConfig;
	/** Keep JSX as is (scanning: no injected JSX runtime imports). */
	preserveJsx: boolean;
	detectModuleKind: boolean;
}

function runSucrase(code: string, run: RunOptions): TransformResult {
	const { loader, config } = run;
	// trimUnusedImports for JavaScript: TypeScript's import elision does exactly that.
	const typescript = loader === "ts" || loader === "tsx" || config.trimUnusedImports;
	// Like Bun, the js loader accepts JSX.
	const jsxEnabled = loader !== "ts";
	const keepUnusedImports = config.keepUnusedImports && !config.trimUnusedImports;
	const transforms: string[] = [];
	if (typescript) transforms.push("typescript");
	if (jsxEnabled) transforms.push("jsx");

	// The vendored sucrase is untyped.
	const file = parse(code, jsxEnabled, typescript, false) as SucraseFile;
	const tokens = file.tokens;
	const jsx = resolveJsx(config.jsx, code, tokens);
	const kind = run.detectModuleKind ? moduleKind(code, tokens) : { hasESM: false, usesCommonJS: false };

	const nameManager = new NameManager(code, tokens);
	const helperManager = new HelperManager(nameManager);
	const tokenProcessor = new TokenProcessor(code, tokens, false, true, helperManager);
	if (typescript && !keepUnusedImports) {
		// Shadowed global detection is needed for TS implicit elision of imported names.
		identifyShadowedGlobals(tokenProcessor, file.scopes, getTSImportedNames(tokenProcessor));
	}
	const options = {
		transforms,
		disableESTransforms: true,
		jsxRuntime: run.preserveJsx ? "preserve" : jsx.runtime,
		production: !jsx.development,
		jsxImportSource: jsx.importSource,
		jsxPragma: jsx.factory,
		jsxFragmentPragma: jsx.fragment,
		keepUnusedImports,
		filePath: config.filePath,
		preserveDynamicImport: true,
		injectCreateRequireForImportRequire: false,
		legacyDecorators: config.legacyDecorators,
	};
	const root = new RootTransformer(
		{ tokenProcessor, scopes: file.scopes, nameManager, importProcessor: null, helperManager },
		transforms,
		false,
		options,
	) as { transformers: object[]; transform(): { code: string } };
	// Bun does not add displayName to createReactClass() calls.
	root.transformers = root.transformers.filter((t) => !(t instanceof ReactDisplayNameTransformer));
	const tp = tokenProcessor as SucraseTokenProcessor;
	if (config.define.length > 0) root.transformers.unshift(new DefineTransformer(tp, config.define));
	if (config.eliminate.size > 0 || config.replace.size > 0) {
		root.transformers.unshift(new ExportsTransformer(tp, config.eliminate, config.replace));
	}
	return { code: root.transform().code, hasESM: kind.hasESM, usesCommonJS: kind.usesCommonJS };
}

/** Turn a sucrase syntax error into a BuildMessage carrying the position. */
function toBuildMessage(err: unknown, code: string, file: string): unknown {
	if (!(err instanceof SyntaxError) || !("pos" in err) || typeof err.pos !== "number") return err;
	const pos = err.pos;
	const offset = Math.min(Math.max(pos, 0), code.length);
	const lineStart = offset === 0 ? 0 : code.lastIndexOf("\n", offset - 1) + 1;
	let lineEnd = code.indexOf("\n", offset);
	if (lineEnd === -1) lineEnd = code.length;
	let line = 1;
	for (let i = code.indexOf("\n"); i !== -1 && i < lineStart; i = code.indexOf("\n", i + 1)) line++;
	const lineText = code.slice(lineStart, lineEnd).replace(/\r$/, "");
	const message = err.message.replace(/ \(\d+:\d+\)$/, "");
	return new BuildMessage(message, {
		file,
		namespace: "file",
		line,
		column: offset - lineStart,
		length: offset < lineEnd ? 1 : 0,
		lineText,
		offset,
	});
}

function transformSource(code: string, run: RunOptions): TransformResult {
	try {
		return runSucrase(code, run);
	} catch (err) {
		throw toBuildMessage(err, code, run.config.filePath);
	}
}

/** Transpile `code` (TypeScript and/or JSX per `loader`) to JavaScript. */
export function transform(code: string, loader: Loader, config: TranspileConfig): string {
	return transformSource(code, { loader, config, preserveJsx: false, detectModuleKind: false }).code;
}

/** Decode the source text of a string literal (or a template without substitutions). */
function decodeStringLiteral(raw: string): string {
	const body = raw.slice(1, -1);
	if (!body.includes("\\")) return body;
	return body.replace(/\\(u\{[0-9a-fA-F]+\}|u[0-9a-fA-F]{4}|x[0-9a-fA-F]{2}|\r\n|[\s\S])/g, (_m, e: string) => {
		switch (e[0]) {
			case "u":
				if (e.length === 1) return e;
				return String.fromCodePoint(Number.parseInt(e[1] === "{" ? e.slice(2, -1) : e.slice(1), 16));
			case "x":
				return e.length === 1 ? e : String.fromCharCode(Number.parseInt(e.slice(1), 16));
			case "n":
				return "\n";
			case "t":
				return "\t";
			case "r":
				return e.length === 1 ? "\r" : "";
			case "b":
				return "\b";
			case "f":
				return "\f";
			case "v":
				return "\v";
			case "0":
				return "\0";
			case "\n":
			case "\u2028":
			case "\u2029":
				return "";
			default:
				return e;
		}
	});
}

interface ScanResult {
	imports: Import[];
	exports: string[];
	hasESM: boolean;
}

/** Collect the imports and exports of transpiled JavaScript. */
function scanOutput(js: string, jsxEnabled: boolean): ScanResult {
	// The vendored sucrase is untyped.
	const file = parse(js, jsxEnabled, false, false) as SucraseFile;
	const tokens = file.tokens;
	const text = (token: SucraseToken): string => js.slice(token.start, token.end);
	const imports: Import[] = [];
	const exportNames: string[] = [];
	let hasESM = false;

	// A static string argument at tokens[j] followed by `)` or `,`.
	const staticArgument = (j: number): string | null => {
		const token = tokens[j];
		if (token === undefined) return null;
		let after: number;
		let raw: string;
		if (token.type === tt.string) {
			after = j + 1;
			raw = text(token);
		} else if (
			token.type === tt.backQuote &&
			tokens[j + 1]?.type === tt.template &&
			tokens[j + 2]?.type === tt.backQuote
		) {
			after = j + 3;
			raw = js.slice(token.start, tokens[j + 2].end);
		} else {
			return null;
		}
		const next = tokens[after];
		if (next === undefined || (next.type !== tt.parenR && next.type !== tt.comma)) return null;
		return decodeStringLiteral(raw);
	};
	// The module specifier of `from "x"` at tokens[k].
	const fromSpecifier = (k: number): string | null => {
		const token = tokens[k];
		const next = tokens[k + 1];
		if (token === undefined || next === undefined) return null;
		if (token.type !== tt.name || token.contextualKeyword !== ContextualKeyword._from || next.type !== tt.string)
			return null;
		return decodeStringLiteral(text(next));
	};
	const nameOf = (token: SucraseToken): string =>
		token.type === tt.string ? decodeStringLiteral(text(token)) : text(token);

	for (let i = 0; i < tokens.length; i++) {
		const token = tokens[i];
		switch (token.type) {
			case tt._import: {
				const next = tokens[i + 1];
				if (next === undefined || next.type === tt.dot) break;
				if (next.type === tt.parenL) {
					const path = staticArgument(i + 2);
					if (path !== null) imports.push({ kind: "dynamic-import", path });
					break;
				}
				hasESM = true;
				if (next.type === tt.string) {
					imports.push({ kind: "import-statement", path: decodeStringLiteral(text(next)) });
					break;
				}
				for (let k = i + 1; k < tokens.length && tokens[k].type !== tt.semi; k++) {
					const path = fromSpecifier(k);
					if (path !== null) {
						imports.push({ kind: "import-statement", path });
						i = k + 1;
						break;
					}
				}
				break;
			}
			case tt._export: {
				if (token.scopeDepth !== 0) break;
				hasESM = true;
				const next = tokens[i + 1];
				if (next === undefined) break;
				if (next.type === tt._default) {
					exportNames.push("default");
				} else if (next.type === tt.braceL) {
					let k = i + 2;
					let last: SucraseToken | null = null;
					for (; k < tokens.length && tokens[k].type !== tt.braceR; k++) {
						if (tokens[k].type === tt.comma) {
							if (last !== null) exportNames.push(nameOf(last));
							last = null;
						} else {
							last = tokens[k];
						}
					}
					if (last !== null) exportNames.push(nameOf(last));
					const path = fromSpecifier(k + 1);
					if (path !== null) imports.push({ kind: "import-statement", path });
					i = k;
				} else if (next.type === tt.star) {
					let k = i + 2;
					const as = tokens[k];
					if (as !== undefined && (as.type === tt._as || as.contextualKeyword === ContextualKeyword._as)) {
						if (tokens[k + 1] !== undefined) exportNames.push(nameOf(tokens[k + 1]));
						k += 2;
					}
					const path = fromSpecifier(k);
					if (path !== null) imports.push({ kind: "import-statement", path });
				} else {
					const end = token.rhsEndIndex ?? tokens.length;
					exportNames.push(...exportedDeclarationNames({ tokens, identifierNameForToken: text }, i, end));
				}
				break;
			}
			case tt.name: {
				if (token.identifierRole !== IdentifierRole.Access || text(token) !== "require") break;
				if (i > 0 && (tokens[i - 1].type === tt.dot || tokens[i - 1].type === tt.questionDot)) break;
				const next = tokens[i + 1];
				if (next === undefined) break;
				if (next.type === tt.parenL) {
					const path = staticArgument(i + 2);
					if (path !== null) imports.push({ kind: "require-call", path });
				} else if (
					next.type === tt.dot &&
					tokens[i + 2] !== undefined &&
					text(tokens[i + 2]) === "resolve" &&
					tokens[i + 3]?.type === tt.parenL
				) {
					const path = staticArgument(i + 4);
					if (path !== null) imports.push({ kind: "require-resolve", path });
				}
				break;
			}
			default:
				break;
		}
	}
	return { imports, exports: exportNames, hasESM };
}

function scanSource(code: string, loader: Loader, config: TranspileConfig): ScanResult {
	// Transpile first (types and type-only imports gone, like Bun's scanner, which keeps unused
	// value imports unless trimUnusedImports) but keep JSX so the JSX runtime imports, which Bun
	// marks internal, are not reported.
	const scanConfig: TranspileConfig = { ...config, keepUnusedImports: !config.trimUnusedImports };
	const js = transformSource(code, { loader, config: scanConfig, preserveJsx: true, detectModuleKind: false }).code;
	try {
		return scanOutput(js, loader !== "ts");
	} catch (err) {
		throw toBuildMessage(err, js, config.filePath);
	}
}

/** Bun.Transpiler#scanImports: the imports of `code`, in source order. */
export function scanImports(code: string, loader: Loader, config: TranspileConfig): Import[] {
	return scanSource(code, loader, config).imports;
}

/**
 * Bun.Transpiler#scan: exports and imports. As in Bun, require() calls of ES modules are not
 * listed (they are for CommonJS sources).
 */
export function scan(code: string, loader: Loader, config: TranspileConfig): { exports: string[]; imports: Import[] } {
	const result = scanSource(code, loader, config);
	const imports = result.hasESM
		? result.imports.filter((i) => i.kind !== "require-call" && i.kind !== "require-resolve")
		: result.imports;
	return { exports: result.exports, imports };
}

// --- module loader ---------------------------------------------------------------------------

const compilerOptionsCache = new Map<string, TSConfigCompilerOptions | null>();

function resolveExtends(spec: string, dir: string): string | null {
	const candidates: string[] = [];
	if (spec.startsWith("./") || spec.startsWith("../") || isAbsolute(spec)) {
		const p = resolve(dir, spec);
		candidates.push(p, `${p}.json`, join(p, "tsconfig.json"));
	} else {
		for (let d = dir; ; d = dirname(d)) {
			const p = join(d, "node_modules", spec);
			candidates.push(p, `${p}.json`, join(p, "tsconfig.json"));
			if (dirname(d) === d) break;
		}
	}
	for (const candidate of candidates) {
		if ((core.statKind(candidate) as number) === 1) return candidate;
	}
	return null;
}

/** compilerOptions of a tsconfig file, `extends` applied. */
function loadCompilerOptions(file: string, depth: number): TSConfigCompilerOptions {
	const text = core.readText(file) as string | undefined;
	if (text === undefined || depth > 16) return {};
	let config: unknown;
	try {
		config = parseJSONC(text);
	} catch {
		return {};
	}
	if (typeof config !== "object" || config === null) return {};
	const { extends: ext, compilerOptions } = config as TSConfig;
	let merged: TSConfigCompilerOptions = {};
	const bases = ext === undefined ? [] : Array.isArray(ext) ? ext : [ext];
	for (const base of bases) {
		if (typeof base !== "string") continue;
		const basePath = resolveExtends(base, dirname(file));
		if (basePath !== null) merged = { ...merged, ...loadCompilerOptions(basePath, depth + 1) };
	}
	if (typeof compilerOptions === "object" && compilerOptions !== null) merged = { ...merged, ...compilerOptions };
	return merged;
}

/** compilerOptions of the nearest tsconfig.json/jsconfig.json at or above `dir`. */
function findCompilerOptions(dir: string): TSConfigCompilerOptions | null {
	const cached = compilerOptionsCache.get(dir);
	if (cached !== undefined) return cached;
	let result: TSConfigCompilerOptions | null = null;
	let found = false;
	for (const name of ["tsconfig.json", "jsconfig.json"]) {
		const file = join(dir, name);
		if ((core.statKind(file) as number) === 1) {
			result = loadCompilerOptions(file, 0);
			found = true;
			break;
		}
	}
	if (!found) {
		const parent = dirname(dir);
		if (parent !== dir) result = findCompilerOptions(parent);
	}
	compilerOptionsCache.set(dir, result);
	return result;
}

function loaderForPath(filename: string): Loader {
	const dot = filename.lastIndexOf(".");
	const ext = dot === -1 ? "" : filename.slice(dot).toLowerCase();
	if (ext === ".ts" || ext === ".mts" || ext === ".cts") return "ts";
	if (ext === ".tsx") return "tsx";
	if (ext === ".jsx") return "jsx";
	return "js";
}

/**
 * In a CommonJS source, turn the JSX runtime imports sucrase puts at the start of the output into
 * require() calls so the module stays CommonJS.
 */
function jsxImportsToRequire(code: string): string {
	let pos = 0;
	if (code.startsWith("#!")) {
		const nl = code.indexOf("\n");
		pos = nl === -1 ? code.length : nl + 1;
	}
	const head = code.slice(0, pos);
	const re = /(const [\w$]+ = "(?:[^"\\\n]|\\.)*";)|import \{([^}]*)\} from "([^"]*)";/y;
	let out = "";
	re.lastIndex = pos;
	for (let m = re.exec(code); m !== null; m = re.exec(code)) {
		if (m[1] !== undefined) {
			out += m[1];
		} else {
			const bindings = m[2]
				.split(",")
				.map((s) => s.trim().replace(/^([\w$]+) as ([\w$]+)$/, "$1: $2"))
				.join(", ");
			out += `const {${bindings}} = require(${JSON.stringify(m[3])});`;
		}
		pos = re.lastIndex;
	}
	return head + out + code.slice(pos);
}

/**
 * Transpile a module source for the module loader, the loader picked from the file extension
 * (.ts/.mts/.cts, .tsx, .jsx, else JavaScript with JSX). JSX and decorator settings come from the
 * nearest tsconfig.json/jsconfig.json and `@jsx*` pragmas. CommonJS sources stay CommonJS.
 */
export function transpileForLoad(source: string, filename: string): string {
	const loader = loaderForPath(filename);
	const co = findCompilerOptions(dirname(filename)) ?? {};
	const jsx = defaultJsxConfig();
	applyCompilerOptionsJsx(jsx, co);
	const config: TranspileConfig = {
		loader,
		jsx,
		legacyDecorators: co.experimentalDecorators === true,
		keepUnusedImports: co.verbatimModuleSyntax === true || co.preserveValueImports === true,
		trimUnusedImports: false,
		define: [],
		eliminate: new Set(),
		replace: new Map(),
		filePath: filename,
	};
	const result = transformSource(source, { loader, config, preserveJsx: false, detectModuleKind: true });
	if (result.usesCommonJS && !result.hasESM) return jsxImportsToRequire(result.code);
	return result.code;
}
