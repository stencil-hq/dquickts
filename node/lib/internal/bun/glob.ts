// internal:bun/glob — Bun.Glob on the shared engine (internal:fs/glob).

import * as core from "qbun:core";
import { invalidArgType } from "internal:errors";
import { compileGlob, globWalk, globWalkSync } from "internal:fs/glob";

interface GlobMatcher {
	match(path: string): boolean;
}

interface WalkOptions {
	cwd: string;
	dot?: boolean;
	absolute?: boolean;
	followSymlinks?: boolean;
	onlyFiles?: boolean;
	throwErrorOnBrokenSymlink?: boolean;
}

interface GlobScanOptions {
	cwd?: string;
	dot?: boolean;
	absolute?: boolean;
	followSymlinks?: boolean;
	throwErrorOnBrokenSymlink?: boolean;
	onlyFiles?: boolean;
}

function scanOptions(opts: string | GlobScanOptions | undefined): WalkOptions {
	if (opts === undefined || opts === null) return { cwd: core.cwd() as string };
	if (typeof opts === "string") return { cwd: opts };
	if (typeof opts !== "object") throw invalidArgType("options", "string or object", opts);
	const cwd = opts.cwd;
	if (cwd !== undefined && typeof cwd !== "string") throw invalidArgType("options.cwd", "string", cwd);
	return {
		cwd: cwd ?? (core.cwd() as string),
		dot: !!opts.dot,
		absolute: !!opts.absolute,
		followSymlinks: !!opts.followSymlinks,
		throwErrorOnBrokenSymlink: !!opts.throwErrorOnBrokenSymlink,
		onlyFiles: opts.onlyFiles === undefined ? true : !!opts.onlyFiles,
	};
}

export class Glob {
	#pattern: string;
	#matcher: GlobMatcher | null = null;

	constructor(pattern: string) {
		if (typeof pattern !== "string") throw invalidArgType("pattern", "string", pattern);
		this.#pattern = pattern;
	}

	/** Match `path` against the pattern as-is (wildcards also match dotfiles). */
	match(path: string): boolean {
		if (typeof path !== "string") throw invalidArgType("path", "string", path);
		this.#matcher ??= compileGlob(this.#pattern, { dot: true });
		return this.#matcher.match(path);
	}

	scan(optionsOrCwd?: string | GlobScanOptions): AsyncIterableIterator<string> {
		return globWalk([this.#pattern], scanOptions(optionsOrCwd));
	}

	scanSync(optionsOrCwd?: string | GlobScanOptions): IterableIterator<string> {
		return globWalkSync([this.#pattern], scanOptions(optionsOrCwd))[Symbol.iterator]();
	}
}
