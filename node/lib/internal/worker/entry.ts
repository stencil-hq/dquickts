// The entry of a new worker: a module path, eval code, a data: URL or (Web
// Worker) a blob: URL or module specifier.

import * as core from "qbun:core";
import { codedError, invalidArgType } from "internal:errors";
import { resolve } from "internal:resolve";
import { getBlobBytes, resolveObjectURL } from "node:buffer";
import { fileURLToPath } from "node:url";

export interface WorkerEntry {
	filename?: string;
	evalSource?: string;
	evalTs?: boolean;
	/** process.argv[1] of the worker */
	displayName: string;
}

const TS_EXT = /\.(?:[cm]?ts|tsx)$/;

function isTypeScriptMime(mime: string): boolean {
	return /typescript|\/tsx?\b/.test(mime.toLowerCase());
}

/** The code of a data: URL (RFC 2397). */
function fromDataURL(href: string): WorkerEntry {
	const comma = href.indexOf(",");
	if (comma < 0) throw codedError("ERR_INVALID_URL", `Invalid URL: ${href}`, TypeError);
	const meta = href.slice(5, comma);
	const payload = href.slice(comma + 1);
	const base64 = /;base64$/i.test(meta);
	const mime = (base64 ? meta.slice(0, -7) : meta).split(";")[0] || "text/plain";
	let code: string;
	if (base64) {
		const bin = atob(decodeURIComponent(payload));
		const bytes = new Uint8Array(bin.length);
		for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
		code = new TextDecoder().decode(bytes);
	} else {
		code = decodeURIComponent(payload);
	}
	return { evalSource: code, evalTs: isTypeScriptMime(mime), displayName: href };
}

type BlobLike = Blob & { name?: string };

/** The code of a blob: URL made by URL.createObjectURL(). */
function fromBlobURL(href: string): WorkerEntry {
	const blob = resolveObjectURL(href) as BlobLike | undefined;
	if (!blob) throw codedError("ERR_WORKER_PATH", `Blob URL not found: ${href}`, TypeError);
	const code = new TextDecoder().decode(getBlobBytes(blob));
	const ts = isTypeScriptMime(blob.type ?? "") || (typeof blob.name === "string" && TS_EXT.test(blob.name));
	return { evalSource: code, evalTs: ts, displayName: href };
}

function absolute(path: string): string {
	if (path.startsWith("/")) return path;
	const cwd = core.cwd() as string;
	const parts = `${cwd}/${path}`.split("/");
	const out: string[] = [];
	for (const part of parts) {
		if (part === "" || part === ".") continue;
		if (part === "..") out.pop();
		else out.push(part);
	}
	return `/${out.join("/")}`;
}

/** Resolve like a module import from the current directory, else the plain path. */
function resolveFile(spec: string, bare: boolean): string {
	try {
		const resolved = resolve(spec, `${core.cwd()}/`, "import");
		if (resolved.startsWith("/")) return resolved;
	} catch (err) {
		if (bare) throw err;
	}
	return absolute(spec);
}

function fromURL(href: string): WorkerEntry | null {
	if (href.startsWith("file:")) {
		const filename = fileURLToPath(href);
		return { filename, displayName: filename };
	}
	if (href.startsWith("data:")) return fromDataURL(href);
	if (href.startsWith("blob:")) return fromBlobURL(href);
	if (/^[a-z][a-z0-9+.-]*:\/\//i.test(href))
		throw codedError("ERR_WORKER_UNSUPPORTED_OPERATION", `Worker URL protocol not supported: ${href}`, TypeError);
	return null;
}

/** node:worker_threads: an absolute path, ./ or ../ relative path, file: or data: URL, or eval code. */
export function nodeEntry(filename: unknown, isEval: boolean): WorkerEntry {
	if (isEval) {
		if (typeof filename !== "string")
			throw codedError("ERR_WORKER_UNSUPPORTED_OPERATION", "Eval code must be a string", TypeError);
		return { evalSource: filename, evalTs: false, displayName: "[worker eval]" };
	}
	if (filename instanceof URL) {
		const entry = fromURL(filename.href);
		if (!entry) throw codedError("ERR_INVALID_URL_SCHEME", "The URL must be of scheme file or data", TypeError);
		return entry;
	}
	if (typeof filename !== "string") throw invalidArgType("filename", "of type string or an instance of URL", filename);
	const entry = fromURL(filename);
	if (entry) return entry;
	const relative = filename.startsWith("./") || filename.startsWith("../");
	if (!filename.startsWith("/") && !relative) {
		try {
			const resolved = resolveFile(filename, true);
			return { filename: resolved, displayName: resolved };
		} catch {
			throw codedError(
				"ERR_WORKER_PATH",
				`The worker script or module filename must be an absolute path or a relative path starting with './' or '../'. Received "${filename}"`,
				TypeError,
			);
		}
	}
	const resolved = resolveFile(filename, false);
	return { filename: resolved, displayName: resolved };
}

/** Web Worker: a path or module specifier (from the current directory), or a file:, data: or blob: URL. */
export function webEntry(specifier: unknown): WorkerEntry {
	const href = specifier instanceof URL ? specifier.href : `${specifier}`;
	const entry = fromURL(href);
	if (entry) return entry;
	const bare = !href.startsWith("/") && !href.startsWith("./") && !href.startsWith("../");
	const resolved = resolveFile(href, false);
	if (bare && core.statKind(resolved) !== 1) {
		const fallback = resolveFile(`./${href}`, false);
		return { filename: fallback, displayName: fallback };
	}
	return { filename: resolved, displayName: resolved };
}
