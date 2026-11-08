// "bun" — the Bun object and its named exports (import { $ } from "bun").
// Each API group lives in its own internal module; a group module keeps its
// heavy dependencies lazy, since importing "bun" loads every group:
//   internal:bun/file     file, write, stdin, stdout, stderr, FileSink, mmap
//   internal:bun/glob     Glob
//   internal:bun/spawn    spawn, spawnSync, which
//   internal:bun/shell    $ (Bun Shell)
//   internal:bun/hash     hash, CryptoHasher, SHA1..SHA512_256, MD4, MD5, sha, password
//   internal:bun/compress gzipSync, gunzipSync, deflateSync, inflateSync, zstd*
//   internal:bun/misc     stringWidth, stripANSI, wrapAnsi, inspect, deepEquals, deepMatch,
//                         escapeHTML, color, randomUUIDv7, peek, ArrayBufferSink,
//                         readableStreamTo*, sleep, sleepSync, nanoseconds, gc, Cookie...
//   internal:bun/semver   semver
//   internal:bun/parsers  TOML, YAML, JSONC, JSONL, JSON5
//   internal:bun/net      serve, connect, listen, udpSocket, dns
//   internal:bun/sql      SQL, sql, RedisClient, redis, S3Client, s3 (unsupported)
//   internal:bun/image    Image
//   internal:bun/archive  Archive
//   internal:bun/transpiler Transpiler

import * as core from "qbun:core";
import process, { BUN_VERSION } from "node:process";
import { resolve as resolveSpecifier } from "internal:resolve";
import { fileURLToPath as urlToPath, pathToFileURL as pathToURL } from "node:url";
import { type BunPlugin, clearAll as clearAllPlugins, plugin as registerPlugin } from "internal:plugins";
import * as file from "internal:bun/file";
import * as glob from "internal:bun/glob";
import * as spawn from "internal:bun/spawn";
import * as shell from "internal:bun/shell";
import * as hash from "internal:bun/hash";
import * as compress from "internal:bun/compress";
import * as misc from "internal:bun/misc";
import * as semverNs from "internal:bun/semver";
import * as parsers from "internal:bun/parsers";
import * as net from "internal:bun/net";
import * as sql from "internal:bun/sql";
import * as image from "internal:bun/image";
import * as archive from "internal:bun/archive";
import * as transpiler from "internal:bun/transpiler";

export * from "internal:bun/file";
export * from "internal:bun/glob";
export * from "internal:bun/spawn";
export * from "internal:bun/shell";
export * from "internal:bun/hash";
export * from "internal:bun/compress";
export * from "internal:bun/misc";
export * from "internal:bun/semver";
export * from "internal:bun/parsers";
export * from "internal:bun/net";
export * from "internal:bun/sql";
export * from "internal:bun/image";
export * from "internal:bun/archive";
export * from "internal:bun/transpiler";

const info = core.info() as { mainPath?: string; isMainThread: boolean };

export const version = BUN_VERSION;
export const revision = "qbun";
export const env = process.env;
export const argv = process.argv;
export const main = info.mainPath ?? process.argv[1] ?? "";
export const isMainThread = info.isMainThread;
export const origin = "";
export const enableANSIColors = true;
export const embeddedFiles: Blob[] = [];

export const pathToFileURL: (path: string) => URL = pathToURL;
export const fileURLToPath: (url: string | URL) => string = urlToPath;

/** Resolve `specifier` from the file or directory `from`. */
export function resolveSync(specifier: string, from: string): string {
	const parent = from.endsWith("/") || core.statKind(from) === 2 ? `${from.replace(/\/$/, "")}/` : from;
	return resolveSpecifier(specifier, parent, "import");
}

export function resolve(specifier: string, from: string): Promise<string> {
	try {
		return Promise.resolve(resolveSync(specifier, from));
	} catch (e) {
		return Promise.reject(e);
	}
}

/** Bun.plugin(): register a runtime plugin (onResolve, onLoad, virtual modules). */
export const plugin = Object.assign((p: BunPlugin): unknown => registerPlugin(p), { clearAll: clearAllPlugins });

const Bun = {
	...file,
	...glob,
	...spawn,
	...shell,
	...hash,
	...compress,
	...misc,
	...semverNs,
	...parsers,
	...net,
	...sql,
	...image,
	...archive,
	...transpiler,
	version,
	revision,
	env,
	argv,
	main,
	isMainThread,
	origin,
	enableANSIColors,
	embeddedFiles,
	pathToFileURL,
	fileURLToPath,
	resolveSync,
	resolve,
	plugin,
};

export default Bun;
