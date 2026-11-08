// Bun.plugin(): runtime loader plugins (onResolve, onLoad, virtual modules).
//
// Nothing here runs until a plugin is registered: the resolver checks
// `pluginState.active` and the C module loader calls the "pluginLoad" hook,
// which is only installed in the loader hooks object while plugins exist.
//
// Module names produced by plugins: files keep their path (with a ?query),
// a result in another namespace becomes "namespace:path", a virtual module
// (build.module()) keeps its specifier.

import * as core from "qbun:core";

export type Loader =
	| "js"
	| "jsx"
	| "ts"
	| "tsx"
	| "json"
	| "jsonc"
	| "json5"
	| "toml"
	| "yaml"
	| "text"
	| "file"
	| "napi"
	| "jsonl";

/** A normalized onLoad result, read by the C loader and the CommonJS loader. */
export type LoadDescriptor =
	| { exports: Record<string, unknown>; esDefault: boolean }
	| { contents: string | Uint8Array | ArrayBuffer; loader: Loader };

type ResolveKind = "import" | "require";

interface OnResolveArgs {
	path: string;
	importer: string;
	namespace: string;
	resolveDir: string;
	kind: "import-statement" | "require-call";
}
interface OnResolveResult {
	path?: unknown;
	namespace?: unknown;
}
interface OnLoadArgs {
	path: string;
	namespace: string;
	loader: string;
	defer: () => Promise<void>;
}
interface OnLoadResult {
	contents?: unknown;
	loader?: unknown;
	exports?: unknown;
}
type OnResolveCallback = (args: OnResolveArgs) => OnResolveResult | null | undefined | void;
type OnLoadCallback = (args: OnLoadArgs) => OnLoadResult | null | undefined | void | PromiseLike<unknown>;
type VirtualCallback = () => OnLoadResult | null | undefined | void | PromiseLike<unknown>;

interface Constraints {
	filter: RegExp;
	namespace?: string;
}
interface Entry<F> {
	filter: RegExp;
	namespace: string;
	callback: F;
}

export interface BunPlugin {
	name?: string;
	target?: string;
	setup(build: PluginBuilder): unknown;
}

export interface PluginBuilder {
	onLoad(constraints: Constraints, callback: OnLoadCallback): PluginBuilder;
	onResolve(constraints: Constraints, callback: OnResolveCallback): PluginBuilder;
	onStart(callback: () => unknown): PluginBuilder;
	onEnd(callback: (result: unknown) => unknown): PluginBuilder;
	onBeforeParse(constraints: Constraints, callback: unknown): PluginBuilder;
	module(specifier: string, callback: VirtualCallback): PluginBuilder;
	config: Record<string, unknown>;
	target: "bun";
}

const resolvers: Entry<OnResolveCallback>[] = [];
const loaders: Entry<OnLoadCallback>[] = [];
const virtualModules = new Map<string, VirtualCallback>();
/** Module names in a plugin namespace ("ns:path") handed out by resolution. */
const namespacedNames = new Set<string>();
/** onResolve paths found nowhere, used as module names because an onLoad callback matches them. */
const asIsNames = new Set<string>();
/** onLoad results computed by the CommonJS loader for the ES module loader. */
const pendingLoads = new Map<string, LoadDescriptor>();

/** Read by internal:resolve: false while no plugin is registered. */
export const pluginState = { active: false };

let loaderHooks: Record<string, unknown> | undefined;

/** The object given to core.setHooks(): "pluginLoad" is set in it while plugins exist. */
export function bindLoaderHooks(hooks: Record<string, unknown>): void {
	loaderHooks = hooks;
	if (pluginState.active) hooks.pluginLoad = pluginLoad;
}

function activate(): void {
	if (pluginState.active) return;
	pluginState.active = true;
	if (loaderHooks) loaderHooks.pluginLoad = pluginLoad;
}

function testFilter(filter: RegExp, s: string): boolean {
	filter.lastIndex = 0;
	return filter.test(s);
}

function isThenable(v: unknown): v is PromiseLike<unknown> {
	return (
		v !== null && (typeof v === "object" || typeof v === "function") && "then" in v && typeof v.then === "function"
	);
}

function checkConstraints(constraints: Constraints, method: string): { filter: RegExp; namespace: string } {
	if (constraints === null || typeof constraints !== "object") {
		throw new TypeError(`${method}() expects an object as its first argument`);
	}
	const { filter, namespace } = constraints;
	if (!(filter instanceof RegExp))
		throw new TypeError(`${method}() expects first argument to be an object with a filter RegExp`);
	if (namespace !== undefined && typeof namespace !== "string") {
		throw new TypeError(`${method}() expects namespace to be a string`);
	}
	return { filter, namespace: namespace || "file" };
}

function checkCallback(callback: unknown, method: string): void {
	if (typeof callback !== "function") throw new TypeError(`${method}() expects callback to be a function`);
}

/** Bun.plugin(plugin): register a runtime plugin; returns what setup() returns. */
export function plugin(p: BunPlugin): unknown {
	if (p === null || typeof p !== "object") throw new TypeError("plugin needs to be an object");
	if (typeof p.setup !== "function") throw new TypeError("plugin needs a setup() function");
	const build: PluginBuilder = {
		onLoad(constraints, callback) {
			const c = checkConstraints(constraints, "onLoad");
			checkCallback(callback, "onLoad");
			loaders.push({ ...c, callback });
			activate();
			return build;
		},
		onResolve(constraints, callback) {
			const c = checkConstraints(constraints, "onResolve");
			checkCallback(callback, "onResolve");
			resolvers.push({ ...c, callback });
			activate();
			return build;
		},
		onStart() {
			return build;
		},
		onEnd() {
			return build;
		},
		onBeforeParse() {
			return build;
		},
		module(specifier, callback) {
			if (typeof specifier !== "string")
				throw new TypeError("module() expects first argument to be a string for the module ID");
			checkCallback(callback, "module");
			virtualModules.set(specifier, callback);
			activate();
			return build;
		},
		config: {},
		target: "bun",
	};
	return p.setup(build);
}

/** Bun.plugin.clearAll(): unregister every runtime plugin. */
export function clearAll(): void {
	resolvers.length = 0;
	loaders.length = 0;
	virtualModules.clear();
	namespacedNames.clear();
	asIsNames.clear();
	pendingLoads.clear();
	pluginState.active = false;
	if (loaderHooks) delete loaderHooks.pluginLoad;
}

function stripQuery(path: string): string {
	if (!path.startsWith("/")) return path;
	const q = path.indexOf("?");
	return q < 0 ? path : path.slice(0, q);
}

function dirnameOf(p: string): string {
	const i = p.lastIndexOf("/");
	return i <= 0 ? "/" : p.slice(0, i);
}

/** [namespace, path] of a module name or importer. */
function splitName(name: string): [string, string] {
	if (name.startsWith("/") || namespacedNames.size === 0) return ["file", name];
	if (namespacedNames.has(name)) {
		const i = name.indexOf(":");
		return [name.slice(0, i), name.slice(i + 1)];
	}
	return ["file", name];
}

/** The module name of a resolution in `namespace`. */
function namespacedName(namespace: string, path: string): string {
	const name = `${namespace}:${path}`;
	namespacedNames.add(name);
	return name;
}

/** An onLoad callback of the "file" namespace matches the module name `path`. */
export function hasFileLoader(path: string): boolean {
	for (const e of loaders) if (e.namespace === "file" && testFilter(e.filter, path)) return true;
	return false;
}

/**
 * The resolution of `spec` from `parent` by the onResolve callbacks, or
 * undefined when none of them answers. `fallback` is the default resolution
 * (no plugins), used for an answer without a namespace.
 */
export function pluginResolve(
	spec: string,
	parent: string | undefined,
	kind: ResolveKind,
	fallback: (spec: string, parent: string | undefined, kind: ResolveKind) => string,
): string | undefined {
	// a module name that a plugin produced (require() of a plugin module re-resolves it)
	if (virtualModules.has(spec) || namespacedNames.has(spec) || asIsNames.has(spec)) return spec;
	// no parent: the loader itself loading an already resolved name
	if (resolvers.length === 0 || !parent) return undefined;
	const [importerNamespace, importerName] = splitName(parent);
	let importer = importerNamespace === "file" ? stripQuery(importerName) : importerName;
	let resolveDir: string;
	if (importer.endsWith("/")) {
		// Bun.resolveSync(spec, dir): the importer is the directory
		importer = importer.slice(0, -1) || "/";
		resolveDir = importer;
	} else if (importer.startsWith("/")) resolveDir = dirnameOf(importer);
	else resolveDir = core.cwd() as string;
	const kindName = kind === "require" ? "require-call" : "import-statement";

	const run = (namespace: string, path: string): string | undefined => {
		for (let i = 0; i < resolvers.length; i++) {
			const e = resolvers[i];
			if (e.namespace !== namespace || !testFilter(e.filter, path)) continue;
			const r = e.callback({ path, importer, namespace: importerNamespace, resolveDir, kind: kindName });
			if (isThenable(r)) {
				throw new TypeError("onResolve() doesn't support returning a Promise at runtime");
			}
			if (r === null || r === undefined) continue;
			if (typeof r !== "object") throw new TypeError("onResolve() expects an object returned");
			const { path: newPath, namespace: newNamespace } = r as OnResolveResult;
			if (typeof newPath !== "string") throw new TypeError('onResolve() expects a "path" string');
			if (newNamespace !== undefined && typeof newNamespace !== "string") {
				throw new TypeError('onResolve() expects "namespace" to be a string');
			}
			if (newNamespace && newNamespace !== "file") return namespacedName(newNamespace, newPath);
			try {
				return fallback(newPath, parent, kind);
			} catch (err) {
				// a path that does not exist is still loadable by an onLoad callback
				if (hasFileLoader(newPath)) {
					asIsNames.add(newPath);
					return newPath;
				}
				throw err;
			}
		}
		return undefined;
	};

	// "virtual:thing": the callbacks of namespace "virtual" run on "thing" first
	const colon = spec.indexOf(":");
	if (colon > 1 && !spec.startsWith("file:")) {
		const r = run(spec.slice(0, colon), spec.slice(colon + 1));
		if (r !== undefined) return r;
	}
	// Bun's runtime onResolve sees only paths (./x, ../x, /x, file:): bare
	// package names and other prefixed strings resolve without the callbacks
	// (verified against bun 1.4.4), so a plugin that maps a package to a shim
	// does not catch the shim's own imports of that package
	if (importerNamespace === "file" && !isPathSpecifier(spec)) return undefined;
	return run(importerNamespace, spec);
}

function isPathSpecifier(spec: string): boolean {
	return spec.startsWith("./") || spec.startsWith("../") || spec === "." || spec === ".." || spec.startsWith("/") || spec.startsWith("file:");
}

const LOADERS: Record<string, true> = {
	js: true,
	jsx: true,
	ts: true,
	tsx: true,
	json: true,
	jsonc: true,
	json5: true,
	toml: true,
	yaml: true,
	text: true,
	file: true,
	napi: true,
	jsonl: true,
};

const EXT_LOADERS: Record<string, Loader> = {
	".js": "js",
	".mjs": "js",
	".cjs": "js",
	".ts": "ts",
	".mts": "ts",
	".cts": "ts",
	".jsx": "jsx",
	".tsx": "tsx",
	".json": "json",
	".jsonc": "jsonc",
	".json5": "json5",
	".toml": "toml",
	".yaml": "yaml",
	".yml": "yaml",
	".txt": "text",
	".jsonl": "jsonl",
	".node": "napi",
};

function defaultLoader(path: string, type: string | undefined): Loader {
	if (type !== undefined) {
		if (type === "javascript") return "js";
		if (Object.hasOwn(LOADERS, type)) return type as Loader;
	}
	const base = path.slice(path.lastIndexOf("/") + 1);
	const dot = base.lastIndexOf(".");
	if (dot <= 0) return "js";
	return EXT_LOADERS[base.slice(dot)] ?? "file";
}

interface LoadContext {
	path: string;
	namespace: string;
	loader: Loader;
	sync: boolean;
	name: string;
}

function describe(v: unknown, ctx: LoadContext): LoadDescriptor {
	if (typeof v !== "object") throw new TypeError("onLoad() expects an object returned");
	const r = v as OnLoadResult;
	if (r.loader === "object" || (r.exports !== undefined && r.contents === undefined)) {
		const exports = r.exports;
		if (exports === null || (typeof exports !== "object" && typeof exports !== "function")) {
			throw new TypeError('"object" loader must return an "exports" object');
		}
		const ex = exports as Record<string, unknown>;
		return { exports: ex, esDefault: ex.default !== undefined };
	}
	const contents = r.contents;
	if (typeof contents !== "string" && !ArrayBuffer.isView(contents) && !(contents instanceof ArrayBuffer)) {
		throw new TypeError('onLoad() expects "contents" to be a string or Uint8Array');
	}
	let loader = ctx.loader;
	if (r.loader !== undefined && r.loader !== null) {
		if (typeof r.loader !== "string" || !Object.hasOwn(LOADERS, r.loader)) {
			throw new TypeError(`onLoad() returned an unsupported loader "${String(r.loader)}"`);
		}
		loader = r.loader as Loader;
	}
	return { contents: contents as string | Uint8Array | ArrayBuffer, loader };
}

function asyncError(ctx: LoadContext): Error {
	return new TypeError(`require() async module "${ctx.name}" is unsupported. use "await import()" instead.`);
}

const resolvedDefer = (): Promise<void> => Promise.resolve();

/** Run the onLoad callbacks from index `i`: the first result, undefined if none answers. */
function chain(ctx: LoadContext, i: number): LoadDescriptor | Promise<LoadDescriptor | undefined> | undefined {
	for (; i < loaders.length; i++) {
		const e = loaders[i];
		if (e.namespace !== ctx.namespace || !testFilter(e.filter, ctx.path)) continue;
		const r = e.callback({ path: ctx.path, namespace: ctx.namespace, loader: ctx.loader, defer: resolvedDefer });
		if (isThenable(r)) {
			if (ctx.sync) throw asyncError(ctx);
			const next = i + 1;
			return Promise.resolve(r).then((v) => (v === null || v === undefined ? chain(ctx, next) : describe(v, ctx)));
		}
		if (r !== null && r !== undefined) return describe(r, ctx);
	}
	return undefined;
}

function runLoad(
	name: string,
	type: string | undefined,
	sync: boolean,
): LoadDescriptor | Promise<LoadDescriptor | undefined> | undefined {
	const [namespace, path] = splitName(name);
	const ctx: LoadContext = { path, namespace, loader: defaultLoader(stripQuery(path), type), sync, name };
	const virtual = virtualModules.get(name);
	if (virtual !== undefined) {
		const r = virtual();
		if (isThenable(r)) {
			if (sync) throw asyncError(ctx);
			return Promise.resolve(r).then((v) => (v === null || v === undefined ? chain(ctx, 0) : describe(v, ctx)));
		}
		if (r !== null && r !== undefined) return describe(r, ctx);
	}
	return chain(ctx, 0);
}

/**
 * The "pluginLoad" loader hook (C, ES modules): the onLoad result of the
 * module `name` imported with the type attribute `type`, maybe a promise,
 * undefined to load it as usual.
 */
export function pluginLoad(
	name: string,
	type: string | undefined,
): LoadDescriptor | Promise<LoadDescriptor | undefined> | undefined {
	const pending = pendingLoads.get(name);
	if (pending !== undefined) {
		pendingLoads.delete(name);
		return pending;
	}
	return runLoad(name, type, false);
}

/** The onLoad result of `name` for require(): a promise result throws. */
export function pluginLoadSync(name: string): LoadDescriptor | undefined {
	return runLoad(name, undefined, true) as LoadDescriptor | undefined;
}

/** Load `name` as an ES module from the onLoad result `desc` (for require()). */
export function loadModuleFromPlugin(name: string, desc: LoadDescriptor): unknown {
	pendingLoads.set(name, desc);
	try {
		return core.loadModuleSync(name, "");
	} finally {
		pendingLoads.delete(name);
	}
}
