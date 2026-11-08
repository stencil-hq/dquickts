// internal:fs/node-glob — fs.glob / fs.globSync / fs.promises.glob on the
// shared glob engine (internal:fs/glob).

import * as pathMod from "node:path";
import process from "node:process";
import { invalidArgType } from "internal:errors";
import { compileGlob, globWalk, globWalkSync, type WalkOptions } from "internal:fs/glob";
import { Dirent, direntTypeFromMode, getValidatedPath, pathString } from "internal:fs/utils";
import { sync, call } from "internal:fs/ops";

export interface NodeGlobOptions {
	cwd?: string | URL;
	exclude?: ((entry: string | Dirent) => boolean) | readonly string[];
	withFileTypes?: boolean;
}

function prepare(
	pattern: unknown,
	options: NodeGlobOptions | undefined,
): { patterns: string[]; walk: WalkOptions; withFileTypes: boolean } {
	const patterns = typeof pattern === "string" ? [pattern] : pattern;
	if (!Array.isArray(patterns) || !patterns.every((p) => typeof p === "string")) {
		throw invalidArgType("pattern", "of type string or an instance of Array", pattern);
	}
	const opts = options ?? {};
	const cwd = opts.cwd === undefined ? process.cwd() : pathString(getValidatedPath(opts.cwd, "options.cwd"));
	const withFileTypes = !!opts.withFileTypes;
	let exclude: WalkOptions["exclude"];
	const ex = opts.exclude;
	if (typeof ex === "function") {
		exclude = (rel, isDir) => {
			if (!withFileTypes) return !!ex(rel);
			const abs = pathMod.resolve(cwd, rel);
			return !!ex(
				new Dirent(pathMod.basename(abs), isDir ? direntType(abs, true) : direntType(abs, false), pathMod.dirname(abs)),
			);
		};
	} else if (Array.isArray(ex)) {
		const matchers = ex.map((p) => compileGlob(p));
		exclude = (rel) => matchers.some((m) => m.match(rel));
	} else if (ex != null) {
		throw invalidArgType("options.exclude", "of type function or an instance of Array", ex);
	}
	return {
		patterns: patterns as string[],
		walk: { cwd, dot: false, absolute: false, followSymlinks: false, onlyFiles: false, exclude },
		withFileTypes,
	};
}

function direntType(abs: string, fallbackDir: boolean): number {
	try {
		return direntTypeFromMode(sync<number[]>("lstat", abs, false)[1]);
	} catch {
		return fallbackDir ? 2 : 1;
	}
}

function toDirent(cwd: string, rel: string): Dirent {
	const abs = pathMod.resolve(cwd, rel);
	return new Dirent(pathMod.basename(abs), direntType(abs, false), pathMod.dirname(abs));
}

function nativeSep(p: string): string {
	return pathMod.sep === "/" ? p : p.split("/").join(pathMod.sep);
}

export function globSyncImpl(pattern: unknown, options?: NodeGlobOptions): (string | Dirent)[] {
	const { patterns, walk, withFileTypes } = prepare(pattern, options);
	const out = globWalkSync(patterns, walk);
	return withFileTypes ? out.map((rel) => toDirent(walk.cwd, rel)) : out.map(nativeSep);
}

export async function* globImpl(pattern: unknown, options?: NodeGlobOptions): AsyncGenerator<string | Dirent> {
	const { patterns, walk, withFileTypes } = prepare(pattern, options);
	for await (const rel of globWalk(patterns, walk)) {
		if (!withFileTypes) {
			yield nativeSep(rel);
			continue;
		}
		const abs = pathMod.resolve(walk.cwd, rel);
		let type = 0;
		try {
			type = direntTypeFromMode((await call<number[]>("lstat", abs, false))[1]);
		} catch {
			type = 0;
		}
		yield new Dirent(pathMod.basename(abs), type, pathMod.dirname(abs));
	}
}
