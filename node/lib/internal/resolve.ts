// Module resolution of Bun: builtins, relative/absolute paths with the
// TypeScript extension fallbacks, file: URLs, package "imports" (#x),
// node_modules packages with "exports" conditions, "main" and index files.
// Results are realpaths (symlinks resolved), or builtin names ("node:fs").

import * as core from "qbun:core";
import { codedError } from "internal:errors";
import { hasFileLoader, pluginResolve, pluginState } from "internal:plugins";

export type ResolveKind = "import" | "require";

/** Node builtin module names (without the node: prefix). */
export const builtinModules: readonly string[] = [
	"_http_agent",
	"_http_client",
	"_http_common",
	"_http_incoming",
	"_http_outgoing",
	"_http_server",
	"_stream_duplex",
	"_stream_passthrough",
	"_stream_readable",
	"_stream_transform",
	"_stream_writable",
	"_tls_common",
	"_tls_wrap",
	"assert",
	"assert/strict",
	"async_hooks",
	"buffer",
	"child_process",
	"cluster",
	"console",
	"constants",
	"crypto",
	"dgram",
	"diagnostics_channel",
	"dns",
	"dns/promises",
	"domain",
	"events",
	"fs",
	"fs/promises",
	"http",
	"http2",
	"https",
	"inspector",
	"inspector/promises",
	"module",
	"net",
	"os",
	"path",
	"path/posix",
	"path/win32",
	"perf_hooks",
	"process",
	"punycode",
	"querystring",
	"readline",
	"readline/promises",
	"repl",
	"stream",
	"stream/consumers",
	"stream/promises",
	"stream/web",
	"string_decoder",
	"sys",
	"timers",
	"timers/promises",
	"tls",
	"trace_events",
	"tty",
	"url",
	"util",
	"util/types",
	"v8",
	"vm",
	"wasi",
	"worker_threads",
	"zlib",
];
const builtinSet = new Set(builtinModules);
/** Builtins that only exist with the node: prefix. */
const prefixOnly = new Set(["test", "sqlite", "sea", "test/reporters"]);

const IMPORT_CONDITIONS = new Set(["bun", "node", "import", "default"]);
const REQUIRE_CONDITIONS = new Set(["bun", "node", "require", "default"]);
const EXTENSIONS = [".tsx", ".ts", ".jsx", ".js", ".mjs", ".cjs", ".mts", ".cts", ".json", ".node"];
const TS_REPLACEMENTS: Record<string, string[]> = {
	".js": [".ts", ".tsx"],
	".jsx": [".tsx", ".ts"],
	".mjs": [".mts"],
	".cjs": [".cts"],
};

export function isBuiltin(name: string): boolean {
	if (name.startsWith("node:")) {
		const bare = name.slice(5);
		return builtinSet.has(bare) || prefixOnly.has(bare);
	}
	return builtinSet.has(name);
}

const kindCache = new Map<string, number>();
function statKind(path: string): number {
	let k = kindCache.get(path);
	if (k === undefined) {
		k = core.statKind(path) as number;
		kindCache.set(path, k);
	}
	return k;
}
const isFile = (p: string) => statKind(p) === 1;
const isDir = (p: string) => statKind(p) === 2;

/** Forget the cached file-system facts (after files appear or vanish). */
export function clearResolveCache(): void {
	kindCache.clear();
	pkgCache.clear();
	realCache.clear();
}

const realCache = new Map<string, string>();
function realpath(p: string): string {
	let r = realCache.get(p);
	if (r === undefined) {
		r = (core.realpath(p) as string | null) ?? p;
		realCache.set(p, r);
	}
	return r;
}

interface PackageJson {
	name?: string;
	main?: string;
	type?: string;
	exports?: unknown;
	imports?: Record<string, unknown>;
	[key: string]: unknown;
}

const pkgCache = new Map<string, PackageJson | null>();
/** The parsed package.json of directory `dir`, null if there is none. */
export function readPackageJson(dir: string): PackageJson | null {
	let pkg = pkgCache.get(dir);
	if (pkg !== undefined) return pkg;
	const text = core.readText(`${dir}/package.json`) as string | undefined;
	pkg = null;
	if (text !== undefined) {
		try {
			pkg = JSON.parse(text);
		} catch (e) {
			throw codedError(
				"ERR_INVALID_PACKAGE_CONFIG",
				`Invalid package config ${dir}/package.json: ${(e as Error).message}`,
			);
		}
	}
	pkgCache.set(dir, pkg);
	return pkg;
}

/** The directory of the nearest package.json above `dir` (inclusive). */
export function findPackageScope(dir: string): string | null {
	for (;;) {
		if (readPackageJson(dir) !== null) return dir;
		const parent = dirname(dir);
		if (parent === dir) return null;
		dir = parent;
	}
}

function dirname(p: string): string {
	const i = p.lastIndexOf("/");
	if (i <= 0) return "/";
	return p.slice(0, i);
}

function normalize(p: string): string {
	const abs = p.startsWith("/");
	const out: string[] = [];
	for (const part of p.split("/")) {
		if (part === "" || part === ".") continue;
		if (part === "..") {
			if (out.length && out[out.length - 1] !== "..") out.pop();
			else if (!abs) out.push("..");
		} else out.push(part);
	}
	return (abs ? "/" : "") + out.join("/");
}

function join(dir: string, rel: string): string {
	return normalize(`${dir}/${rel}`);
}

/** [path, ?query] of a file specifier (Bun): the query is kept, a #hash dropped. */
function splitQuery(spec: string): [string, string] {
	const q = spec.indexOf("?");
	const h = spec.indexOf("#");
	if (q >= 0 && (h < 0 || q < h)) return [spec.slice(0, q), spec.slice(q, h < 0 ? undefined : h)];
	if (h >= 0) return [spec.slice(0, h), ""];
	return [spec, ""];
}

function tryFile(p: string): string | null {
	if (isFile(p)) return p;
	const dot = p.lastIndexOf(".");
	if (dot > p.lastIndexOf("/")) {
		const repl = TS_REPLACEMENTS[p.slice(dot)];
		if (repl) {
			const stem = p.slice(0, dot);
			for (const ext of repl) if (isFile(stem + ext)) return stem + ext;
		}
	}
	for (const ext of EXTENSIONS) if (isFile(p + ext)) return p + ext;
	return null;
}

function tryDirectory(p: string, conditions: Set<string>): string | null {
	if (!isDir(p)) return null;
	const pkg = readPackageJson(p);
	if (pkg) {
		const main = pickMain(pkg, conditions);
		if (main) {
			const m = join(p, main);
			const r = tryFile(m) ?? tryIndex(m);
			if (r) return r;
		}
	}
	return tryIndex(p);
}

function tryIndex(p: string): string | null {
	for (const ext of EXTENSIONS) if (isFile(`${p}/index${ext}`)) return `${p}/index${ext}`;
	return null;
}

function pickMain(pkg: PackageJson, conditions: Set<string>): string | undefined {
	if (conditions.has("import") && typeof pkg.module === "string") return pkg.module;
	return typeof pkg.main === "string" ? pkg.main : undefined;
}

function resolvePath(p: string, conditions: Set<string>): string | null {
	if (p.endsWith("/")) return tryDirectory(p.slice(0, -1) || "/", conditions);
	return tryFile(p) ?? tryDirectory(p, conditions);
}

/** Resolve the target of an "exports"/"imports" entry. undefined: no match. */
function resolveTarget(
	pkgDir: string,
	target: unknown,
	patternMatch: string | null,
	conditions: Set<string>,
): string | null | undefined {
	if (typeof target === "string") {
		if (!target.startsWith("./")) {
			if (patternMatch !== null && !target.startsWith("../") && !target.startsWith("/")) {
				return `bare:${target.replace(/\*/g, patternMatch)}`;
			}
			if (!target.startsWith("../") && !target.startsWith("/")) return `bare:${target}`;
			return null;
		}
		const sub = patternMatch === null ? target : target.replace(/\*/g, patternMatch);
		return join(pkgDir, sub);
	}
	if (Array.isArray(target)) {
		for (const t of target) {
			const r = resolveTarget(pkgDir, t, patternMatch, conditions);
			if (r) return r;
		}
		return target.length ? null : undefined;
	}
	if (target && typeof target === "object") {
		const map = target as Record<string, unknown>;
		for (const key in map) {
			if (key === "default" || conditions.has(key)) {
				const r = resolveTarget(pkgDir, map[key], patternMatch, conditions);
				if (r !== undefined) return r;
			}
		}
		return undefined;
	}
	if (target === null) return null;
	return undefined;
}

function resolveMap(
	pkgDir: string,
	map: Record<string, unknown>,
	key: string,
	conditions: Set<string>,
): string | null | undefined {
	if (Object.hasOwn(map, key) && !key.includes("*")) return resolveTarget(pkgDir, map[key], null, conditions);
	let bestKey = "";
	let bestMatch = "";
	for (const k in map) {
		const star = k.indexOf("*");
		if (star >= 0) {
			const prefix = k.slice(0, star);
			const suffix = k.slice(star + 1);
			if (
				key.startsWith(prefix) &&
				key !== prefix &&
				key.length >= k.length &&
				key.endsWith(suffix) &&
				patternKeyCompare(bestKey, k) > 0
			) {
				bestKey = k;
				bestMatch = key.slice(prefix.length, key.length - suffix.length);
			}
		} else if (k.endsWith("/") && key.startsWith(k) && patternKeyCompare(bestKey, k) > 0) {
			bestKey = k;
			bestMatch = key.slice(k.length);
		}
	}
	if (!bestKey) return undefined;
	if (bestKey.endsWith("/") && !bestKey.includes("*")) {
		const t = resolveTarget(pkgDir, map[bestKey], null, conditions);
		return typeof t === "string" ? `${t}/${bestMatch}` : t;
	}
	return resolveTarget(pkgDir, map[bestKey], bestMatch, conditions);
}

function patternKeyCompare(a: string, b: string): number {
	const aStar = a.indexOf("*");
	const bStar = b.indexOf("*");
	const baseA = aStar === -1 ? a.length : aStar + 1;
	const baseB = bStar === -1 ? b.length : bStar + 1;
	if (baseA > baseB) return -1;
	if (baseB > baseA) return 1;
	if (aStar === -1) return 1;
	if (bStar === -1) return -1;
	if (a.length > b.length) return -1;
	if (b.length > a.length) return 1;
	return 0;
}

function exportsMap(exports: unknown): Record<string, unknown> {
	if (typeof exports === "string" || Array.isArray(exports)) return { ".": exports };
	if (exports && typeof exports === "object") {
		const keys = Object.keys(exports);
		if (keys.length && !keys[0].startsWith(".")) return { ".": exports };
		return exports as Record<string, unknown>;
	}
	return {};
}

function finishTarget(r: string, from: string, kind: ResolveKind, conditions: Set<string>): string | null {
	if (r.startsWith("bare:")) return resolveBare(r.slice(5), from, kind, conditions);
	return isFile(r) ? r : resolvePath(r, conditions);
}

function resolvePackageImports(
	spec: string,
	fromDir: string,
	kind: ResolveKind,
	conditions: Set<string>,
): string | null {
	const scope = findPackageScope(fromDir);
	if (!scope) return null;
	const pkg = readPackageJson(scope)!;
	if (!pkg.imports || typeof pkg.imports !== "object") return null;
	const r = resolveMap(scope, pkg.imports, spec, conditions);
	if (!r) return null;
	return finishTarget(r, scope, kind, conditions);
}

function splitPackage(spec: string): [string, string] {
	let i = spec.indexOf("/");
	if (spec.startsWith("@")) i = i < 0 ? -1 : spec.indexOf("/", i + 1);
	if (i < 0) return [spec, "."];
	return [spec.slice(0, i), `.${spec.slice(i)}`];
}

function resolveInPackage(pkgDir: string, subpath: string, kind: ResolveKind, conditions: Set<string>): string | null {
	const pkg = readPackageJson(pkgDir);
	if (pkg && pkg.exports !== undefined && pkg.exports !== null) {
		const r = resolveMap(pkgDir, exportsMap(pkg.exports), subpath, conditions);
		if (r) return finishTarget(r, pkgDir, kind, conditions);
		// Bun falls back to the files when "exports" has no match
	}
	if (subpath === ".") return tryDirectory(pkgDir, conditions);
	return resolvePath(join(pkgDir, subpath), conditions);
}

function resolveBare(spec: string, fromDir: string, kind: ResolveKind, conditions: Set<string>): string | null {
	const [name, subpath] = splitPackage(spec);
	// self reference: the package of fromDir has this name and "exports"
	const scope = findPackageScope(fromDir);
	if (scope) {
		const pkg = readPackageJson(scope)!;
		if (pkg.name === name && pkg.exports !== undefined) {
			const r = resolveInPackage(scope, subpath, kind, conditions);
			if (r) return r;
		}
	}
	let dir = fromDir;
	for (;;) {
		if (!dir.endsWith("/node_modules")) {
			const pkgDir = `${dir === "/" ? "" : dir}/node_modules/${name}`;
			if (isDir(pkgDir)) {
				const r = resolveInPackage(pkgDir, subpath, kind, conditions);
				if (r) return r;
			}
		}
		const parent = dirname(dir);
		if (parent === dir) break;
		dir = parent;
	}
	for (const extra of globalPaths()) {
		const pkgDir = `${extra}/${name}`;
		if (isDir(pkgDir)) {
			const r = resolveInPackage(pkgDir, subpath, kind, conditions);
			if (r) return r;
		}
	}
	return null;
}

let globalPathList: string[] | undefined;
function globalPaths(): string[] {
	if (!globalPathList) {
		const env = core.environ() as Record<string, string>;
		globalPathList = (env.NODE_PATH ?? "").split(":").filter(Boolean);
	}
	return globalPathList;
}

function parentDir(parent: string | undefined): string {
	if (parent && parent.startsWith("/")) {
		const q = parent.indexOf("?");
		if (q >= 0) parent = parent.slice(0, q);
		if (parent.endsWith("/")) return parent.slice(0, -1) || "/";
		return dirname(parent);
	}
	return core.cwd() as string;
}

/** The builtin module named by `spec`, undefined if it is not one. */
function resolveBuiltin(spec: string): string | undefined {
	if (spec.startsWith("node:")) {
		if (!isBuiltin(spec)) {
			throw codedError("ERR_UNKNOWN_BUILTIN_MODULE", `No such built-in module: ${spec}`);
		}
		return spec;
	}
	if (spec === "bun" || spec.startsWith("bun:") || spec.startsWith("qbun:") || spec.startsWith("internal:"))
		return spec;
	if (builtinSet.has(spec)) return `node:${spec}`;
	return undefined;
}

/** The resolution without plugins (the fallback of an onResolve answer). */
function resolveDefault(spec: string, parent: string | undefined, kind: ResolveKind): string {
	return resolveBuiltin(spec) ?? resolveFile(spec, parent, kind);
}

/**
 * Resolve `spec` imported or required from the file `parent`. Returns a
 * builtin name ("node:fs", "bun:sqlite", "bun"), the realpath of a file
 * (followed by the ?query of a path specifier: "/a/b.ts?mtime=1", a distinct
 * module), or a module name given by a Bun.plugin() onResolve callback.
 * Throws an error with code ERR_MODULE_NOT_FOUND / MODULE_NOT_FOUND.
 */
export function resolve(spec: string, parent: string | undefined, kind: ResolveKind = "import"): string {
	const builtin = resolveBuiltin(spec);
	if (builtin !== undefined) return builtin;
	if (pluginState.active) {
		const r = pluginResolve(spec, parent, kind, resolveDefault);
		if (r !== undefined) return r;
	}
	return resolveFile(spec, parent, kind);
}

function resolveFile(spec: string, parent: string | undefined, kind: ResolveKind): string {
	const conditions = kind === "require" ? REQUIRE_CONDITIONS : IMPORT_CONDITIONS;
	const fromDir = parentDir(parent);
	let r: string | null;
	let query = "";
	if (spec.startsWith("file:")) {
		const u = new URL(spec);
		if (u.protocol !== "file:") throw codedError("ERR_INVALID_URL_SCHEME", "The URL must be of scheme file");
		r = resolvePath(decodeURIComponent(u.pathname), conditions);
		if (r !== null) query = u.search;
	} else if (spec.startsWith("/") || spec === "." || spec === ".." || spec.startsWith("./") || spec.startsWith("../")) {
		const local = (s: string): string | null =>
			s.startsWith("/")
				? resolvePath(normalize(s), conditions)
				: resolvePath(join(fromDir, s) + (s.endsWith("/") ? "/" : ""), conditions);
		const [path, q] = splitQuery(spec);
		r = local(path);
		if (r !== null) query = q;
		// a file name with a literal "?" or "#"
		else if (path !== spec) r = local(spec);
		if (r === null && pluginState.active) {
			// a file that does not exist, loaded by a Bun.plugin() onLoad callback
			const name = (path.startsWith("/") ? normalize(path) : join(fromDir, path)) + q;
			if (hasFileLoader(name)) return name;
		}
	} else if (spec.startsWith("#")) {
		r = resolvePackageImports(spec, fromDir, kind, conditions);
	} else {
		r = resolveBare(spec, fromDir, kind, conditions);
	}
	if (r === null) {
		const err = codedError(
			kind === "require" ? "MODULE_NOT_FOUND" : "ERR_MODULE_NOT_FOUND",
			`Cannot find module '${spec}' from '${parent || fromDir}'`,
		);
		Object.assign(err, { specifier: spec, referrer: parent });
		throw err;
	}
	if (r.startsWith("node:") || r === "bun" || r.startsWith("bun:")) return r;
	return realpath(r) + query;
}
