// Node-API addons: process.dlopen() and require() of .node files.
//
// The native side (qbun:napi, node/napi.c) opens the shared library, finds its
// registration (napi_register_module_v1 or napi_module_register), creates the
// module's napi_env and calls the addon's init function with module.exports;
// a different value returned by init becomes module.exports.

import { fileURLToPath } from "node:url";
import * as native from "qbun:napi";
import { codedError } from "internal:errors";

interface NapiNative {
	dlopen(module: { exports: unknown }, filename: string, flags?: number): void;
	readonly version: number;
}

// qbun:napi exports exactly these
const binding = native as unknown as NapiNative;

/** A path from a filename argument: file: URLs (string or URL) are accepted like Bun. */
function toPath(filename: unknown): string {
	if (typeof filename === "string") {
		if (!filename.startsWith("file:")) return filename;
	} else if (!(filename instanceof URL)) {
		return String(filename);
	}
	return fileURLToPath(filename as string | URL);
}

/** Load the Node-API addon `filename` into `module` (Node's process.dlopen). */
export function dlopen(module: { exports: unknown }, filename: string, flags?: number): void {
	if (arguments.length < 2)
		throw codedError("ERR_MISSING_ARGS", "process.dlopen needs at least 2 arguments", TypeError);
	// Access the lazy global so napi_create_buffer() sees Buffer.prototype.
	void Buffer;
	let mode: number | undefined;
	if (flags !== undefined) {
		try {
			mode = Number(flags) | 0;
		} catch {
			throw codedError("ERR_INVALID_ARG_TYPE", "flag argument must be an integer.", TypeError);
		}
	}
	const path = toPath(filename);
	if (mode === undefined) binding.dlopen(module, path);
	else binding.dlopen(module, path, mode);
}

/** The Node-API version implemented (process.versions.napi). */
export const version: number = binding.version;

export default { dlopen, version };
