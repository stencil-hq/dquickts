// CommonJS: Node's Module with require(), require.resolve(), the module
// cache, JSON and N-API (.node) files, and require() of ES modules and
// builtins (synchronous, like Bun).

import * as core from "qbun:core";
import { codedError } from "internal:errors";
import { builtinModules, isBuiltin, resolve, type ResolveKind } from "internal:resolve";
import { transpileForLoad } from "internal:transpile/index";
import { type Loader, type LoadDescriptor, loadModuleFromPlugin, pluginLoadSync, pluginState } from "internal:plugins";

type RequireFn = ((id: string) => unknown) & {
	resolve: ((id: string, options?: { paths?: string[] }) => string) & { paths(id: string): string[] | null };
	cache: Record<string, Module>;
	main: Module | undefined;
	extensions: Record<string, (m: Module, filename: string) => void>;
};

const WRAPPER_PARAMS = "exports, require, module, __filename, __dirname";

/** The namespace or module object returned by require() of a builtin. */
export function requireBuiltin(name: string): unknown {
	const ns = core.loadModuleSync(name, "") as Record<string, unknown>;
	const def = ns.default;
	return def !== undefined && (typeof def === "object" || typeof def === "function") ? def : ns;
}

function dirnameOf(p: string): string {
	const i = p.lastIndexOf("/");
	return i <= 0 ? "/" : p.slice(0, i);
}

/** The file of a module name: "/a/b.js?mtime=1" → "/a/b.js". */
function stripQuery(name: string): string {
	if (!name.startsWith("/")) return name;
	const q = name.indexOf("?");
	return q < 0 ? name : name.slice(0, q);
}

/** The ES module name of `filename` loaded by `m` (its id keeps the ?query). */
function esmName(m: Module, filename: string): string {
	return m.id.length > filename.length && m.id.startsWith(filename) && m.id[filename.length] === "?" ? m.id : filename;
}

/** Run the CommonJS source `src` of `m` (TypeScript when `ts`). */
function runWrapped(m: Module, src: string, filename: string, ts: boolean): unknown {
	const fn = core.compileFunction(src, filename, WRAPPER_PARAMS, ts) as (this: unknown, ...args: unknown[]) => unknown;
	return fn.call(m.exports, m.exports, m.require, m, filename, dirnameOf(filename));
}

export class Module {
	id: string;
	filename: string;
	path: string;
	exports: unknown = {};
	loaded = false;
	children: Module[] = [];
	paths: string[];
	parent: Module | null | undefined;
	require: RequireFn;

	static _cache: Record<string, Module> = Object.create(null);
	static _pathCache: Record<string, string> = Object.create(null);
	static _extensions: Record<string, (m: Module, filename: string) => void> = Object.create(null);
	static builtinModules = builtinModules;
	static globalPaths: string[] = [];
	static Module = Module;
	static wrapper = [`(function (${WRAPPER_PARAMS}) { `, "\n});"];

	constructor(id = "", parent?: Module | null) {
		this.id = id;
		this.filename = id;
		this.path = dirnameOf(id);
		this.parent = parent;
		this.paths = Module._nodeModulePaths(this.path);
		this.require = makeRequire(this);
		if (parent) parent.children.push(this);
	}

	static isBuiltin(name: string): boolean {
		return isBuiltin(name) || name === "bun" || name.startsWith("bun:");
	}

	static createRequire(filename: string | URL): RequireFn {
		let path = typeof filename === "string" ? filename : filename.href;
		if (path.startsWith("file:")) path = decodeURIComponent(new URL(path).pathname);
		const m = new Module(path, null);
		m.filename = path;
		m.loaded = true;
		return m.require;
	}

	static wrap(script: string): string {
		return Module.wrapper[0] + script + Module.wrapper[1];
	}

	static _nodeModulePaths(from: string): string[] {
		const out: string[] = [];
		let dir = from;
		for (;;) {
			if (!dir.endsWith("/node_modules")) out.push(`${dir === "/" ? "" : dir}/node_modules`);
			const parent = dirnameOf(dir);
			if (parent === dir) break;
			dir = parent;
		}
		return out;
	}

	static _resolveFilename(request: string, parent: Module | null | undefined, _isMain?: boolean): string {
		return resolve(request, parent?.filename || undefined, "require");
	}

	static _load(request: string, parent: Module | null | undefined, isMain = false): unknown {
		const filename = Module._resolveFilename(request, parent, isMain);
		return loadResolved(filename, parent, isMain);
	}

	static runMain(main?: string): void {
		loadResolved(main ?? (core.info() as { mainPath: string }).mainPath, null, true);
	}

	static syncBuiltinESMExports(): void {}
	static findSourceMap(): undefined {
		return undefined;
	}
	static register(): void {}
	static enableCompileCache(): { status: number } {
		return { status: 3 };
	}
	static getCompileCacheDir(): undefined {
		return undefined;
	}

	_compile(content: string, filename: string): unknown {
		return runWrapped(this, content, filename, false);
	}

	load(filename: string): void {
		// a ?query (cache busting) names another module instance of the same file
		const file = stripQuery(filename);
		this.filename = file;
		this.paths = Module._nodeModulePaths(dirnameOf(file));
		const ext = extensionOf(file);
		const handler = Module._extensions[ext] ?? Module._extensions[".js"];
		handler(this, file);
		this.loaded = true;
	}
}

function extensionOf(filename: string): string {
	const base = filename.slice(filename.lastIndexOf("/") + 1);
	const dot = base.lastIndexOf(".");
	return dot > 0 ? base.slice(dot) : "";
}

Module._extensions[".js"] = (m, filename) => {
	const src = core.readText(filename) as string | undefined;
	if (src === undefined) throw codedError("MODULE_NOT_FOUND", `Cannot find module '${filename}'`);
	if (core.detectModule(src, filename)) {
		// an ES module: require() returns its namespace
		m.exports = core.loadModuleSync(esmName(m, filename), "");
		return;
	}
	m._compile(src, filename);
};
for (const ext of [".ts", ".cjs", ".cts", ".mjs", ".mts"]) Module._extensions[ext] = Module._extensions[".js"];
// JSX: transpiled by Bun.Transpiler; an ES module result goes through the module loader
Module._extensions[".jsx"] = Module._extensions[".tsx"] = (m, filename) => {
	const src = core.readText(filename) as string | undefined;
	if (src === undefined) throw codedError("MODULE_NOT_FOUND", `Cannot find module '${filename}'`);
	const js = transpileForLoad(src, filename);
	if (core.detectModule(js, filename)) m.exports = core.loadModuleSync(esmName(m, filename), "");
	else m._compile(js, filename);
};
Module._extensions[".json"] = (m, filename) => {
	const src = core.readText(filename) as string | undefined;
	if (src === undefined) throw codedError("MODULE_NOT_FOUND", `Cannot find module '${filename}'`);
	try {
		m.exports = JSON.parse(src);
	} catch (e) {
		(e as Error).message = `${filename}: ${(e as Error).message}`;
		throw e;
	}
};
Module._extensions[".node"] = (m, filename) => {
	const napi = requireBuiltin("internal:napi") as { dlopen(m: Module, filename: string): void };
	napi.dlopen(m, filename);
};

/**
 * Load the module `key` (a module name: a path, maybe with a ?query, or a
 * plugin module) from the Bun.plugin() onLoad result `desc`.
 */
function loadFromPlugin(m: Module, key: string, desc: LoadDescriptor): void {
	const filename = stripQuery(key);
	m.filename = filename;
	m.paths = Module._nodeModulePaths(dirnameOf(filename));
	if ("exports" in desc) {
		m.exports = desc.exports;
		return;
	}
	const { contents, loader } = desc;
	let src = typeof contents === "string" ? contents : new TextDecoder().decode(contents);
	switch (loader) {
		case "js":
		case "ts":
		case "jsx":
		case "tsx": {
			let ts = loader === "ts";
			if (loader === "jsx" || loader === "tsx") {
				// the transpiler picks its loader from the extension
				src = transpileForLoad(src, filename.endsWith(`.${loader}`) ? filename : `${filename}.${loader}`);
				ts = false;
			}
			if (core.detectModule(src, filename)) {
				m.exports = loadModuleFromPlugin(key, { contents: src, loader: ts ? "ts" : "js" });
			} else {
				runWrapped(m, src, filename, ts);
			}
			return;
		}
		case "napi":
			(requireBuiltin("internal:napi") as { dlopen(m: Module, filename: string): void }).dlopen(m, filename);
			return;
		default:
			// data loaders (json, toml, text...): the default export of the ES module loader
			m.exports = (loadModuleFromPlugin(key, desc) as { default: unknown }).default;
	}
}

/**
 * Load the resolved `filename` (a path or a builtin name) through the cache.
 * `desc`: the onLoad result to use; null: load the file without plugins.
 */
function loadResolved(
	filename: string,
	parent: Module | null | undefined,
	isMain: boolean,
	desc?: LoadDescriptor | null,
): unknown {
	if (
		filename.startsWith("node:") ||
		filename === "bun" ||
		filename.startsWith("bun:") ||
		filename.startsWith("internal:")
	) {
		return requireBuiltin(filename);
	}
	const cached = Module._cache[filename];
	if (cached) {
		if (parent && !parent.children.includes(cached)) parent.children.push(cached);
		return cached.exports;
	}
	const m = new Module(filename, parent);
	if (isMain) {
		m.id = ".";
		mainModule = m;
		const process = globalThis.process as unknown as { mainModule?: Module };
		if (process) process.mainModule = m;
	}
	Module._cache[filename] = m;
	let ok = false;
	try {
		const plugin = desc === undefined && pluginState.active ? pluginLoadSync(filename) : desc;
		if (plugin) {
			loadFromPlugin(m, filename, plugin);
			m.loaded = true;
		} else {
			m.load(filename);
		}
		ok = true;
	} finally {
		if (!ok) delete Module._cache[filename];
	}
	return m.exports;
}

let mainModule: Module | undefined;

function makeRequire(m: Module): RequireFn {
	const require = ((id: string) => {
		if (typeof id !== "string") {
			throw codedError(
				"ERR_INVALID_ARG_TYPE",
				`The "id" argument must be of type string. Received ${typeof id}`,
				TypeError,
			);
		}
		if (id === "") throw codedError("ERR_INVALID_ARG_VALUE", "The argument 'id' must be a non-empty string", TypeError);
		return Module._load(id, m, false);
	}) as RequireFn;
	const resolveFn = ((id: string, options?: { paths?: string[] }) => {
		if (options?.paths?.length) {
			let lastErr: unknown;
			for (const p of options.paths) {
				try {
					return resolve(id, `${p.endsWith("/") ? p : `${p}/`}`, "require");
				} catch (e) {
					lastErr = e;
				}
			}
			throw lastErr;
		}
		return Module._resolveFilename(id, m);
	}) as RequireFn["resolve"];
	resolveFn.paths = (id: string) => (isBuiltin(id) ? null : Module._nodeModulePaths(m.path));
	require.resolve = resolveFn;
	require.cache = Module._cache;
	require.extensions = Module._extensions;
	Object.defineProperty(require, "main", { get: () => mainModule, enumerable: true, configurable: true });
	return require;
}

const fileModules = new Map<string, Module>();
/** A Module standing for an ES module file, used as the parent of its require() calls. */
function moduleFor(filename: string | undefined): Module {
	if (!filename || !filename.startsWith("/")) {
		const cwd = core.cwd() as string;
		filename = `${cwd}/[eval]`;
	}
	const cached = Module._cache[filename] ?? fileModules.get(filename);
	if (cached) return cached;
	const m = new Module(filename, null);
	m.filename = stripQuery(filename);
	m.path = dirnameOf(m.filename);
	m.loaded = true;
	fileModules.set(filename, m);
	return m;
}

/** require() from the file `parent` (import.meta.require, the global require). */
export function requireFrom(parent: string | undefined, id: string): unknown {
	return moduleFor(parent).require(id);
}

export function requireResolveFrom(parent: string | undefined, id: string, options?: { paths?: string[] }): string {
	return moduleFor(parent).require.resolve(id, options);
}

/**
 * The exports of the CommonJS (or .node) module `filename`, for import (the
 * C loader). `source`: the CommonJS contents an onLoad callback gave (with
 * its `loader`); null: the file itself, the plugins were consulted already.
 */
export function loadCommonJS(filename: string, source?: string | null, loader?: Loader): unknown {
	if (typeof source === "string")
		return loadResolved(filename, null, false, { contents: source, loader: loader ?? "js" });
	return loadResolved(filename, null, false, source);
}

export function runMain(filename: string): void {
	loadResolved(filename, null, true);
}

/** import.meta.resolve(): a file: URL, or the builtin name. */
export function metaResolve(parent: string, spec: string): string {
	const r = resolve(spec, parent, "import" satisfies ResolveKind);
	if (!r.startsWith("/")) return r;
	const file = stripQuery(r);
	return `file://${file.replace(/[%#?\s"<>\\^`{|}]/g, (c) => `%${c.charCodeAt(0).toString(16).toUpperCase().padStart(2, "0")}`)}${r.slice(file.length)}`;
}

export default Module;
