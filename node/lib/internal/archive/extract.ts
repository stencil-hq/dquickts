// Extraction never deliberately follows absolute or escaping symlinks. Resolve
// each component beneath the canonical destination and open file leaves NOFOLLOW.
import * as fs from "node:fs";
import { codedError, invalidArgType } from "internal:errors";
import { readError, type TarEntry } from "internal:archive/tar";

interface Stat {
	isSymbolicLink(): boolean;
	isDirectory(): boolean;
}
interface ResolvedPath {
	parts: string[];
	followed: boolean;
	stat?: Stat;
}

function errorCode(error: unknown): string | undefined {
	return error !== null && typeof error === "object" && "code" in error ? String(error.code) : undefined;
}

function unsafe(path: string): Error {
	const error = codedError("EXDEV", `Archive entry would escape the extraction directory: ${path}`) as Error & {
		path: string;
	};
	error.path = path;
	return error;
}

function isAbsolute(path: string): boolean {
	return path.startsWith("/") || /^[a-zA-Z]:/.test(path);
}

function entryPath(path: string): string {
	const normalized = path.replace(/\\/g, "/");
	if (normalized.includes("\0") || isAbsolute(normalized)) throw unsafe(path);
	const parts: string[] = [];
	for (const part of normalized.split("/")) {
		if (part === "" || part === ".") continue;
		if (part === "..") parts.pop();
		else parts.push(part);
	}
	return parts.join("/");
}

function linkTarget(target: string, parent: readonly string[], entry: string): string[] {
	const normalized = target.replace(/\\/g, "/");
	if (!normalized || normalized.includes("\0") || isAbsolute(normalized)) throw unsafe(entry);
	const result = parent.slice();
	for (const part of normalized.split("/")) {
		if (part === "" || part === ".") continue;
		if (part === "..") {
			if (result.length === 0) throw unsafe(entry);
			result.pop();
		} else result.push(part);
	}
	return result;
}

export function extractTar(
	entries: readonly TarEntry[],
	directory: string,
	matches: (path: string) => boolean,
	filtered: boolean,
): number {
	if (typeof directory !== "string") throw invalidArgType("path", "a string", directory);
	if (directory.includes("\0"))
		throw codedError("ERR_INVALID_ARG_VALUE", "Extraction path must not contain null bytes", TypeError);
	fs.mkdirSync(directory, { recursive: true });
	const root = fs.realpathSync(directory).replace(/\/$/, "");
	const absolute = (parts: readonly string[]): string => `${root}/${parts.join("/")}`;
	const stat = (path: string): Stat | undefined => {
		try {
			return fs.lstatSync(path);
		} catch (error) {
			if (errorCode(error) === "ENOENT") return undefined;
			throw error;
		}
	};

	// Resolve one component at a time, checking the original link text as well
	// as its eventual destination. Absolute links are forbidden even within root.
	const resolve = (relative: string, followLeaf: boolean, createParents: boolean): ResolvedPath => {
		let pending = relative === "" ? [] : relative.split("/");
		let parts: string[] = [];
		let followed = false;
		let links = 0;
		while (pending.length) {
			const part = pending.shift()!;
			const leaf = pending.length === 0;
			const candidate = [...parts, part];
			let current = stat(absolute(candidate));
			if (current?.isSymbolicLink() && (!leaf || followLeaf)) {
				if (++links > 40) {
					const error = new Error(`Cannot resolve archive entry symlink: ${relative}`);
					error.name = "ReadError";
					throw error;
				}
				const target = linkTarget(fs.readlinkSync(absolute(candidate)), parts, relative);
				pending = [...target, ...pending];
				parts = [];
				followed = true;
				continue;
			}
			if (!leaf) {
				if (!current && createParents) {
					try {
						fs.mkdirSync(absolute(candidate), { mode: 0o777 });
					} catch (error) {
						if (errorCode(error) !== "EEXIST") throw error;
					}
					current = stat(absolute(candidate));
					if (current?.isSymbolicLink()) {
						pending.unshift(part);
						continue;
					}
				}
				if (!current?.isDirectory()) {
					const error = codedError(
						current ? "ENOTDIR" : "ENOENT",
						`Cannot resolve archive directory: ${relative}`,
					) as Error & { path: string };
					error.path = relative;
					throw error;
				}
			}
			parts = candidate;
			if (leaf) return { parts, followed, stat: current };
		}
		return { parts, followed, stat: stat(absolute(parts)) };
	};

	let count = 0;
	const directories = new Map<string, TarEntry>();
	for (const entry of entries) {
		if (!matches(entry.path)) continue;
		const relative = entryPath(entry.path);
		if (relative === "" && entry.type !== "directory") throw unsafe(entry.path);
		let resolved: ResolvedPath;
		try {
			resolved = resolve(relative, entry.type !== "symlink" && entry.type !== "hardlink", true);
		} catch (error) {
			if (
				entry.type === "directory" &&
				(errorCode(error) === "EXDEV" || (error instanceof Error && error.name === "ReadError"))
			)
				continue;
			if (filtered && errorCode(error) === "EXDEV") continue;
			throw error;
		}
		const target = absolute(resolved.parts);
		if (entry.type === "directory") {
			if (!resolved.stat) fs.mkdirSync(target, { mode: 0o777 });
			else if (!resolved.stat.isDirectory())
				throw codedError("ENOTDIR", `Archive directory is not a directory: ${relative}`);
			directories.set(relative, entry);
			count++;
			continue;
		}
		if (entry.type === "symlink") {
			// libarchive does not create symlinks through symlinked parents.
			if (resolved.followed) continue;
			try {
				linkTarget(entry.linkpath, resolved.parts.slice(0, -1), relative);
			} catch (error) {
				if (filtered && errorCode(error) === "EXDEV") continue;
				throw error;
			}
			if (resolved.stat) fs.unlinkSync(target);
			fs.symlinkSync(entry.linkpath, target);
			fs.lutimesSync(target, entry.mtime, entry.mtime);
			count++;
			continue;
		}
		if (entry.type === "hardlink") {
			const link = entryPath(entry.linkpath);
			if (!link) throw unsafe(entry.linkpath);
			let source: ResolvedPath;
			try {
				source = resolve(link, true, false);
			} catch (error) {
				if (filtered && errorCode(error) === "EXDEV") continue;
				throw error;
			}
			if (!source.stat || source.stat.isDirectory()) throw readError(`Invalid tar hardlink target: ${entry.linkpath}`);
			if (absolute(source.parts) === target) {
				count++;
				continue;
			}
			if (resolved.stat) fs.unlinkSync(target);
			fs.linkSync(absolute(source.parts), target);
			count++;
			continue;
		}
		const flags = fs.constants.O_WRONLY | fs.constants.O_CREAT | fs.constants.O_TRUNC | fs.constants.O_NOFOLLOW;
		const fd = fs.openSync(target, flags, entry.mode);
		try {
			fs.writeFileSync(fd, entry.data);
			fs.fchmodSync(fd, entry.mode);
			fs.futimesSync(fd, entry.mtime, entry.mtime);
		} finally {
			fs.closeSync(fd);
		}
		count++;
	}
	// Defer directory metadata until children no longer need write permission.
	const directoryEntries = [...directories].sort(
		(left, right) => right[0].split("/").length - left[0].split("/").length,
	);
	for (const [relative, entry] of directoryEntries) {
		const resolved = resolve(relative, true, false);
		const target = absolute(resolved.parts);
		fs.chmodSync(target, entry.mode);
		fs.utimesSync(target, entry.mtime, entry.mtime);
	}
	return count;
}
