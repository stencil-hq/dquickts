// node:path — POSIX path operations (Node's algorithm) and the win32 variant.

import * as core from "qbun:core";

function assertPath(path: unknown, name = "path"): asserts path is string {
	if (typeof path !== "string") {
		const err = new TypeError(`The "${name}" argument must be of type string. Received ${describe(path)}`);
		(err as any).code = "ERR_INVALID_ARG_TYPE";
		throw err;
	}
}

function describe(v: unknown): string {
	if (v === null) return "null";
	if (v === undefined) return "undefined";
	if (typeof v === "function") return `function ${v.name}`;
	if (typeof v === "object") return `an instance of ${(v as object).constructor?.name ?? "Object"}`;
	return `type ${typeof v} (${String(v)})`;
}

// Resolves . and .. elements in a path with directory names
function normalizeString(
	path: string,
	allowAboveRoot: boolean,
	separator: string,
	isSep: (c: number) => boolean,
): string {
	let res = "";
	let lastSegmentLength = 0;
	let lastSlash = -1;
	let dots = 0;
	let code = 0;
	for (let i = 0; i <= path.length; ++i) {
		if (i < path.length) code = path.charCodeAt(i);
		else if (isSep(code)) break;
		else code = 47; /* / */
		if (isSep(code)) {
			if (lastSlash === i - 1 || dots === 1) {
				// NOOP
			} else if (dots === 2) {
				if (
					res.length < 2 ||
					lastSegmentLength !== 2 ||
					res.charCodeAt(res.length - 1) !== 46 ||
					res.charCodeAt(res.length - 2) !== 46
				) {
					if (res.length > 2) {
						const lastSlashIndex = res.lastIndexOf(separator);
						if (lastSlashIndex === -1) {
							res = "";
							lastSegmentLength = 0;
						} else {
							res = res.slice(0, lastSlashIndex);
							lastSegmentLength = res.length - 1 - res.lastIndexOf(separator);
						}
						lastSlash = i;
						dots = 0;
						continue;
					} else if (res.length !== 0) {
						res = "";
						lastSegmentLength = 0;
						lastSlash = i;
						dots = 0;
						continue;
					}
				}
				if (allowAboveRoot) {
					res += res.length > 0 ? `${separator}..` : "..";
					lastSegmentLength = 2;
				}
			} else {
				if (res.length > 0) res += `${separator}${path.slice(lastSlash + 1, i)}`;
				else res = path.slice(lastSlash + 1, i);
				lastSegmentLength = i - lastSlash - 1;
			}
			lastSlash = i;
			dots = 0;
		} else if (code === 46 /* . */ && dots !== -1) {
			++dots;
		} else {
			dots = -1;
		}
	}
	return res;
}

const isPosixSep = (c: number) => c === 47;
const isWinSep = (c: number) => c === 47 || c === 92;
const isWinDeviceRoot = (c: number) => (c >= 65 && c <= 90) || (c >= 97 && c <= 122);

interface ParsedPath {
	root: string;
	dir: string;
	base: string;
	ext: string;
	name: string;
}

function _format(sep: string, p: Partial<ParsedPath>): string {
	if (p === null || typeof p !== "object") {
		const err = new TypeError(`The "pathObject" argument must be of type object. Received ${describe(p)}`);
		(err as any).code = "ERR_INVALID_ARG_TYPE";
		throw err;
	}
	const dir = p.dir || p.root;
	const base = p.base || `${p.name || ""}${p.ext ? (p.ext[0] === "." ? "" : ".") + p.ext : ""}`;
	if (!dir) return base;
	return dir === p.root ? `${dir}${base}` : `${dir}${sep}${base}`;
}

export const posix: any = {
	sep: "/",
	delimiter: ":",

	resolve(...args: string[]): string {
		let resolvedPath = "";
		let resolvedAbsolute = false;
		for (let i = args.length - 1; i >= -1 && !resolvedAbsolute; i--) {
			const path = i >= 0 ? args[i] : core.cwd();
			assertPath(path, `paths[${i}]`);
			if (path.length === 0) continue;
			resolvedPath = `${path}/${resolvedPath}`;
			resolvedAbsolute = path.charCodeAt(0) === 47;
		}
		resolvedPath = normalizeString(resolvedPath, !resolvedAbsolute, "/", isPosixSep);
		if (resolvedAbsolute) return `/${resolvedPath}`;
		return resolvedPath.length > 0 ? resolvedPath : ".";
	},

	normalize(path: string): string {
		assertPath(path);
		if (path.length === 0) return ".";
		const isAbsolute = path.charCodeAt(0) === 47;
		const trailingSeparator = path.charCodeAt(path.length - 1) === 47;
		path = normalizeString(path, !isAbsolute, "/", isPosixSep);
		if (path.length === 0) {
			if (isAbsolute) return "/";
			return trailingSeparator ? "./" : ".";
		}
		if (trailingSeparator) path += "/";
		return isAbsolute ? `/${path}` : path;
	},

	isAbsolute(path: string): boolean {
		assertPath(path);
		return path.length > 0 && path.charCodeAt(0) === 47;
	},

	join(...args: string[]): string {
		if (args.length === 0) return ".";
		let joined: string | undefined;
		for (let i = 0; i < args.length; ++i) {
			const arg = args[i];
			assertPath(arg, `paths[${i}]`);
			if (arg.length > 0) {
				if (joined === undefined) joined = arg;
				else joined += `/${arg}`;
			}
		}
		if (joined === undefined) return ".";
		return posix.normalize(joined);
	},

	relative(from: string, to: string): string {
		assertPath(from, "from");
		assertPath(to, "to");
		if (from === to) return "";
		from = posix.resolve(from);
		to = posix.resolve(to);
		if (from === to) return "";
		const fromStart = 1;
		const fromEnd = from.length;
		const fromLen = fromEnd - fromStart;
		const toStart = 1;
		const toLen = to.length - toStart;
		const length = fromLen < toLen ? fromLen : toLen;
		let lastCommonSep = -1;
		let i = 0;
		for (; i < length; i++) {
			const fromCode = from.charCodeAt(fromStart + i);
			if (fromCode !== to.charCodeAt(toStart + i)) break;
			else if (fromCode === 47) lastCommonSep = i;
		}
		if (i === length) {
			if (toLen > length) {
				if (to.charCodeAt(toStart + i) === 47) return to.slice(toStart + i + 1);
				if (i === 0) return to.slice(toStart + i);
			} else if (fromLen > length) {
				if (from.charCodeAt(fromStart + i) === 47) lastCommonSep = i;
				else if (i === 0) lastCommonSep = 0;
			}
		}
		let out = "";
		for (i = fromStart + lastCommonSep + 1; i <= fromEnd; ++i) {
			if (i === fromEnd || from.charCodeAt(i) === 47) out += out.length === 0 ? ".." : "/..";
		}
		return `${out}${to.slice(toStart + lastCommonSep)}`;
	},

	toNamespacedPath(path: string): string {
		return path;
	},

	dirname(path: string): string {
		assertPath(path);
		if (path.length === 0) return ".";
		const hasRoot = path.charCodeAt(0) === 47;
		let end = -1;
		let matchedSlash = true;
		for (let i = path.length - 1; i >= 1; --i) {
			if (path.charCodeAt(i) === 47) {
				if (!matchedSlash) {
					end = i;
					break;
				}
			} else {
				matchedSlash = false;
			}
		}
		if (end === -1) return hasRoot ? "/" : ".";
		if (hasRoot && end === 1) return "//";
		return path.slice(0, end);
	},

	basename(path: string, suffix?: string): string {
		if (suffix !== undefined) assertPath(suffix, "suffix");
		assertPath(path);
		let start = 0;
		let end = -1;
		let matchedSlash = true;
		if (suffix !== undefined && suffix.length > 0 && suffix.length <= path.length) {
			if (suffix === path) return "";
			let extIdx = suffix.length - 1;
			let firstNonSlashEnd = -1;
			for (let i = path.length - 1; i >= 0; --i) {
				const code = path.charCodeAt(i);
				if (code === 47) {
					if (!matchedSlash) {
						start = i + 1;
						break;
					}
				} else {
					if (firstNonSlashEnd === -1) {
						matchedSlash = false;
						firstNonSlashEnd = i + 1;
					}
					if (extIdx >= 0) {
						if (code === suffix.charCodeAt(extIdx)) {
							if (--extIdx === -1) end = i;
						} else {
							extIdx = -1;
							end = firstNonSlashEnd;
						}
					}
				}
			}
			if (start === end) end = firstNonSlashEnd;
			else if (end === -1) end = path.length;
			return path.slice(start, end);
		}
		for (let i = path.length - 1; i >= 0; --i) {
			if (path.charCodeAt(i) === 47) {
				if (!matchedSlash) {
					start = i + 1;
					break;
				}
			} else if (end === -1) {
				matchedSlash = false;
				end = i + 1;
			}
		}
		if (end === -1) return "";
		return path.slice(start, end);
	},

	extname(path: string): string {
		assertPath(path);
		let startDot = -1;
		let startPart = 0;
		let end = -1;
		let matchedSlash = true;
		let preDotState = 0;
		for (let i = path.length - 1; i >= 0; --i) {
			const code = path.charCodeAt(i);
			if (code === 47) {
				if (!matchedSlash) {
					startPart = i + 1;
					break;
				}
				continue;
			}
			if (end === -1) {
				matchedSlash = false;
				end = i + 1;
			}
			if (code === 46) {
				if (startDot === -1) startDot = i;
				else if (preDotState !== 1) preDotState = 1;
			} else if (startDot !== -1) {
				preDotState = -1;
			}
		}
		if (
			startDot === -1 ||
			end === -1 ||
			preDotState === 0 ||
			(preDotState === 1 && startDot === end - 1 && startDot === startPart + 1)
		) {
			return "";
		}
		return path.slice(startDot, end);
	},

	format: _format.bind(null, "/"),

	parse(path: string): ParsedPath {
		assertPath(path);
		const ret = { root: "", dir: "", base: "", ext: "", name: "" };
		if (path.length === 0) return ret;
		const isAbsolute = path.charCodeAt(0) === 47;
		let start: number;
		if (isAbsolute) {
			ret.root = "/";
			start = 1;
		} else {
			start = 0;
		}
		let startDot = -1;
		let startPart = 0;
		let end = -1;
		let matchedSlash = true;
		let i = path.length - 1;
		let preDotState = 0;
		for (; i >= start; --i) {
			const code = path.charCodeAt(i);
			if (code === 47) {
				if (!matchedSlash) {
					startPart = i + 1;
					break;
				}
				continue;
			}
			if (end === -1) {
				matchedSlash = false;
				end = i + 1;
			}
			if (code === 46) {
				if (startDot === -1) startDot = i;
				else if (preDotState !== 1) preDotState = 1;
			} else if (startDot !== -1) {
				preDotState = -1;
			}
		}
		if (end !== -1) {
			const start2 = startPart === 0 && isAbsolute ? 1 : startPart;
			if (
				startDot === -1 ||
				preDotState === 0 ||
				(preDotState === 1 && startDot === end - 1 && startDot === startPart + 1)
			) {
				ret.base = ret.name = path.slice(start2, end);
			} else {
				ret.name = path.slice(start2, startDot);
				ret.base = path.slice(start2, end);
				ret.ext = path.slice(startDot, end);
			}
		}
		if (startPart > 0) ret.dir = path.slice(0, startPart - 1);
		else if (isAbsolute) ret.dir = "/";
		return ret;
	},

	matchesGlob(path: string, pattern: string): boolean {
		return globToRegExp(pattern).test(path);
	},
};

function globToRegExp(glob: string): RegExp {
	let re = "^";
	for (let i = 0; i < glob.length; i++) {
		const c = glob[i];
		if (c === "*") {
			if (glob[i + 1] === "*") {
				i++;
				if (glob[i + 1] === "/") {
					i++;
					re += "(?:.*/)?";
				} else re += ".*";
			} else re += "[^/]*";
		} else if (c === "?") re += "[^/]";
		else if (c === "{") {
			const close = glob.indexOf("}", i);
			if (close < 0) re += "\\{";
			else {
				re += `(?:${glob
					.slice(i + 1, close)
					.split(",")
					.map(escapeRe)
					.join("|")})`;
				i = close;
			}
		} else if (c === "[") {
			const close = glob.indexOf("]", i);
			if (close < 0) re += "\\[";
			else {
				re += `[${glob.slice(i + 1, close).replace(/^!/, "^")}]`;
				i = close;
			}
		} else re += escapeRe(c);
	}
	return new RegExp(`${re}$`);
}

function escapeRe(s: string): string {
	return s.replace(/[.*+?^${}()|[\]\\/]/g, "\\$&");
}

// win32: Node's algorithm for drive letters, UNC roots and both separators.
export const win32: any = {
	sep: "\\",
	delimiter: ";",

	resolve(...args: string[]): string {
		let resolvedDevice = "";
		let resolvedTail = "";
		let resolvedAbsolute = false;
		for (let i = args.length - 1; i >= -1; i--) {
			let path: string;
			if (i >= 0) {
				path = args[i];
				assertPath(path, `paths[${i}]`);
				if (path.length === 0) continue;
			} else if (resolvedDevice.length === 0) {
				path = core.cwd();
			} else {
				path = `${resolvedDevice}\\`;
			}
			const len = path.length;
			let rootEnd = 0;
			let device = "";
			let isAbsolute = false;
			const code = path.charCodeAt(0);
			if (len === 1) {
				if (isWinSep(code)) {
					rootEnd = 1;
					isAbsolute = true;
				}
			} else if (isWinSep(code)) {
				isAbsolute = true;
				if (isWinSep(path.charCodeAt(1))) {
					let j = 2;
					let last = j;
					while (j < len && !isWinSep(path.charCodeAt(j))) j++;
					if (j < len && j !== last) {
						const firstPart = path.slice(last, j);
						last = j;
						while (j < len && isWinSep(path.charCodeAt(j))) j++;
						if (j < len && j !== last) {
							last = j;
							while (j < len && !isWinSep(path.charCodeAt(j))) j++;
							if (j === len || j !== last) {
								device = `\\\\${firstPart}\\${path.slice(last, j)}`;
								rootEnd = j;
							}
						}
					}
				} else {
					rootEnd = 1;
				}
			} else if (isWinDeviceRoot(code) && path.charCodeAt(1) === 58) {
				device = path.slice(0, 2);
				rootEnd = 2;
				if (len > 2 && isWinSep(path.charCodeAt(2))) {
					isAbsolute = true;
					rootEnd = 3;
				}
			}
			if (device.length > 0) {
				if (resolvedDevice.length > 0) {
					if (device.toLowerCase() !== resolvedDevice.toLowerCase()) continue;
				} else {
					resolvedDevice = device;
				}
			}
			if (resolvedAbsolute) {
				if (resolvedDevice.length > 0) break;
			} else {
				resolvedTail = `${path.slice(rootEnd)}\\${resolvedTail}`;
				resolvedAbsolute = isAbsolute;
				if (isAbsolute && resolvedDevice.length > 0) break;
			}
		}
		resolvedTail = normalizeString(resolvedTail, !resolvedAbsolute, "\\", isWinSep);
		return resolvedAbsolute ? `${resolvedDevice}\\${resolvedTail}` : `${resolvedDevice}${resolvedTail}` || ".";
	},

	normalize(path: string): string {
		assertPath(path);
		const len = path.length;
		if (len === 0) return ".";
		let rootEnd = 0;
		let device: string | undefined;
		let isAbsolute = false;
		const code = path.charCodeAt(0);
		if (len === 1) return isPosixSep(code) || code === 92 ? "\\" : path;
		if (isWinSep(code)) {
			isAbsolute = true;
			if (isWinSep(path.charCodeAt(1))) {
				let j = 2;
				let last = j;
				while (j < len && !isWinSep(path.charCodeAt(j))) j++;
				if (j < len && j !== last) {
					const firstPart = path.slice(last, j);
					last = j;
					while (j < len && isWinSep(path.charCodeAt(j))) j++;
					if (j < len && j !== last) {
						last = j;
						while (j < len && !isWinSep(path.charCodeAt(j))) j++;
						if (j === len) return `\\\\${firstPart}\\${path.slice(last)}\\`;
						if (j !== last) {
							device = `\\\\${firstPart}\\${path.slice(last, j)}`;
							rootEnd = j;
						}
					}
				}
			} else {
				rootEnd = 1;
			}
		} else if (isWinDeviceRoot(code) && path.charCodeAt(1) === 58) {
			device = path.slice(0, 2);
			rootEnd = 2;
			if (len > 2 && isWinSep(path.charCodeAt(2))) {
				isAbsolute = true;
				rootEnd = 3;
			}
		}
		let tail = rootEnd < len ? normalizeString(path.slice(rootEnd), !isAbsolute, "\\", isWinSep) : "";
		if (tail.length === 0 && !isAbsolute) tail = ".";
		if (tail.length > 0 && isWinSep(path.charCodeAt(len - 1))) tail += "\\";
		if (device === undefined) return isAbsolute ? `\\${tail}` : tail;
		return isAbsolute ? `${device}\\${tail}` : `${device}${tail}`;
	},

	isAbsolute(path: string): boolean {
		assertPath(path);
		const len = path.length;
		if (len === 0) return false;
		const code = path.charCodeAt(0);
		return (
			isWinSep(code) || (len > 2 && isWinDeviceRoot(code) && path.charCodeAt(1) === 58 && isWinSep(path.charCodeAt(2)))
		);
	},

	join(...args: string[]): string {
		if (args.length === 0) return ".";
		let joined: string | undefined;
		let firstPart = "";
		for (let i = 0; i < args.length; ++i) {
			const arg = args[i];
			assertPath(arg, `paths[${i}]`);
			if (arg.length > 0) {
				if (joined === undefined) joined = firstPart = arg;
				else joined += `\\${arg}`;
			}
		}
		if (joined === undefined) return ".";
		let needsReplace = true;
		let slashCount = 0;
		if (isWinSep(firstPart.charCodeAt(0))) {
			++slashCount;
			const firstLen = firstPart.length;
			if (firstLen > 1 && isWinSep(firstPart.charCodeAt(1))) {
				++slashCount;
				if (firstLen > 2) {
					if (isWinSep(firstPart.charCodeAt(2))) ++slashCount;
					else needsReplace = false;
				}
			}
		}
		if (needsReplace) {
			while (slashCount < joined.length && isWinSep(joined.charCodeAt(slashCount))) slashCount++;
			if (slashCount >= 2) joined = `\\${joined.slice(slashCount)}`;
		}
		return win32.normalize(joined);
	},

	relative(from: string, to: string): string {
		assertPath(from, "from");
		assertPath(to, "to");
		if (from === to) return "";
		const fromOrig = win32.resolve(from);
		const toOrig = win32.resolve(to);
		if (fromOrig === toOrig) return "";
		from = fromOrig.toLowerCase();
		to = toOrig.toLowerCase();
		if (from === to) return "";
		const fromParts = fromOrig.split("\\").filter(Boolean);
		const toParts = toOrig.split("\\").filter(Boolean);
		let i = 0;
		while (i < fromParts.length && i < toParts.length && fromParts[i].toLowerCase() === toParts[i].toLowerCase()) i++;
		if (i === 0) return toOrig;
		const up = fromParts.slice(i).map(() => "..");
		return [...up, ...toParts.slice(i)].join("\\");
	},

	toNamespacedPath(path: string): string {
		if (typeof path !== "string" || path.length === 0) return path;
		const resolvedPath = win32.resolve(path);
		if (resolvedPath.length <= 2) return path;
		if (resolvedPath.charCodeAt(0) === 92) {
			if (resolvedPath.charCodeAt(1) === 92) {
				const code = resolvedPath.charCodeAt(2);
				if (code !== 63 && code !== 46) return `\\\\?\\UNC\\${resolvedPath.slice(2)}`;
			}
		} else if (
			isWinDeviceRoot(resolvedPath.charCodeAt(0)) &&
			resolvedPath.charCodeAt(1) === 58 &&
			resolvedPath.charCodeAt(2) === 92
		) {
			return `\\\\?\\${resolvedPath}`;
		}
		return path;
	},

	dirname(path: string): string {
		assertPath(path);
		const parsed = win32.parse(path);
		if (!parsed.dir) return parsed.root || ".";
		return parsed.dir;
	},

	basename(path: string, suffix?: string): string {
		assertPath(path);
		let start = 0;
		if (path.length >= 2 && isWinDeviceRoot(path.charCodeAt(0)) && path.charCodeAt(1) === 58) start = 2;
		const p = path.slice(start).replace(/\\/g, "/");
		const base = posix.basename(p, suffix);
		return base;
	},

	extname(path: string): string {
		assertPath(path);
		return posix.extname(path.replace(/\\/g, "/"));
	},

	format: _format.bind(null, "\\"),

	parse(path: string): ParsedPath {
		assertPath(path);
		const ret = { root: "", dir: "", base: "", ext: "", name: "" };
		if (path.length === 0) return ret;
		let root = "";
		const m = /^([a-zA-Z]:)?([\\/])?/.exec(path)!;
		const unc = /^[\\/]{2}[^\\/]+[\\/][^\\/]+[\\/]?/.exec(path);
		if (unc) root = unc[0];
		else root = (m[1] ?? "") + (m[2] ?? "");
		ret.root = root;
		const rest = path.slice(root.length);
		const p = posix.parse(rest.replace(/\\/g, "/"));
		ret.base = p.base;
		ret.ext = p.ext;
		ret.name = p.name;
		const restDir = p.dir.replace(/\//g, "\\");
		const rawDir = rest.slice(0, rest.length - p.base.length).replace(/[\\/]+$/, "");
		ret.dir = rawDir ? root + rawDir : root ? root : restDir;
		if (ret.dir === root && root.length > 1 && /[\\/]$/.test(root) && !unc) ret.dir = root;
		return ret;
	},

	matchesGlob(path: string, pattern: string): boolean {
		return globToRegExp(pattern.replace(/\\/g, "/")).test(path.replace(/\\/g, "/"));
	},
};

posix.posix = win32.posix = posix;
posix.win32 = win32.win32 = win32;

export const {
	sep,
	delimiter,
	resolve,
	normalize,
	isAbsolute,
	join,
	relative,
	toNamespacedPath,
	dirname,
	basename,
	extname,
	format,
	parse,
	matchesGlob,
} = posix;

export default posix;
