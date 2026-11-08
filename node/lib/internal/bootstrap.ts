// Runtime bootstrap: installs the globals (process, timers, Buffer, fetch,
// URL, Bun...) and gives the C loader its hooks (qbun:core setHooks).
//
// Globals backed by a library module are lazy: the module is loaded on
// first access (see LAZY_GLOBALS), so a program pays only for what it uses.

import * as core from "qbun:core";
import process, { emitBeforeExit, emitExit, emitUncaught, emitUnhandledRejection } from "node:process";
import * as timers from "node:timers";
import { resolve } from "internal:resolve";
import { setupChildChannel } from "internal:child/ipc";
import { loadCommonJS, metaResolve, requireFrom, requireResolveFrom, runMain, Module } from "internal:cjs";
import { bindLoaderHooks } from "internal:plugins";
import { transpileForLoad } from "internal:transpile/index";
import { inspect } from "node:util";

const g = globalThis as unknown as Record<string, unknown>;

function defineValue(name: string, value: unknown): void {
	Object.defineProperty(globalThis, name, { value, writable: true, configurable: true, enumerable: false });
}

/** Define `name` on globalThis as `exportName` of `spec`, loaded on first access. */
function lazyGlobal(name: string, spec: string, exportName: string = name): void {
	Object.defineProperty(globalThis, name, {
		configurable: true,
		enumerable: false,
		get() {
			const ns = core.loadModuleSync(spec, "") as Record<string, unknown>;
			const value = ns[exportName];
			defineValue(name, value);
			return value;
		},
		set(value: unknown) {
			defineValue(name, value);
		},
	});
}

// global name → [module, export]
const LAZY_GLOBALS: Record<string, [string, string?]> = {
	console: ["node:console", "default"],
	Buffer: ["node:buffer"],
	Blob: ["node:buffer"],
	File: ["node:buffer"],
	TextEncoder: ["internal:encoding"],
	TextDecoder: ["internal:encoding"],
	URL: ["node:url"],
	URLSearchParams: ["node:url"],
	AbortController: ["internal:event-target"],
	AbortSignal: ["internal:event-target"],
	Event: ["internal:event-target"],
	EventTarget: ["internal:event-target"],
	CustomEvent: ["internal:event-target"],
	MessageEvent: ["internal:event-target"],
	ErrorEvent: ["internal:event-target"],
	CloseEvent: ["internal:event-target"],
	ReadableStream: ["node:stream/web"],
	ReadableStreamDefaultReader: ["node:stream/web"],
	ReadableStreamBYOBReader: ["node:stream/web"],
	ReadableStreamBYOBRequest: ["node:stream/web"],
	ReadableByteStreamController: ["node:stream/web"],
	ReadableStreamDefaultController: ["node:stream/web"],
	WritableStream: ["node:stream/web"],
	WritableStreamDefaultWriter: ["node:stream/web"],
	WritableStreamDefaultController: ["node:stream/web"],
	TransformStream: ["node:stream/web"],
	TransformStreamDefaultController: ["node:stream/web"],
	ByteLengthQueuingStrategy: ["node:stream/web"],
	CountQueuingStrategy: ["node:stream/web"],
	TextEncoderStream: ["node:stream/web"],
	TextDecoderStream: ["node:stream/web"],
	CompressionStream: ["node:stream/web"],
	DecompressionStream: ["node:stream/web"],
	fetch: ["internal:fetch"],
	Headers: ["internal:fetch"],
	Request: ["internal:fetch"],
	Response: ["internal:fetch"],
	FormData: ["internal:fetch"],
	WebSocket: ["internal:websocket"],
	crypto: ["internal:webcrypto"],
	Crypto: ["internal:webcrypto"],
	CryptoKey: ["internal:webcrypto"],
	SubtleCrypto: ["internal:webcrypto"],
	MessageChannel: ["internal:worker"],
	MessagePort: ["internal:worker"],
	BroadcastChannel: ["internal:worker"],
	Worker: ["internal:worker"],
	PerformanceEntry: ["node:perf_hooks"],
	PerformanceMark: ["node:perf_hooks"],
	PerformanceMeasure: ["node:perf_hooks"],
	PerformanceObserver: ["node:perf_hooks"],
	Bun: ["bun", "default"],
	BuildMessage: ["internal:transpile/errors"],
};

for (const name in LAZY_GLOBALS) {
	const [spec, exportName] = LAZY_GLOBALS[name];
	lazyGlobal(name, spec, exportName);
}

defineValue("global", globalThis);
defineValue("self", globalThis);
defineValue("process", process);
defineValue("setTimeout", timers.setTimeout);
defineValue("setInterval", timers.setInterval);
defineValue("setImmediate", timers.setImmediate);
defineValue("clearTimeout", timers.clearTimeout);
defineValue("clearInterval", timers.clearInterval);
defineValue("clearImmediate", timers.clearImmediate);
defineValue("navigator", {
	userAgent: `Bun/${process.versions.bun}`,
	language: "en-US",
	languages: ["en-US"],
	platform: process.platform,
	hardwareConcurrency: 8,
	onLine: true,
});
defineValue("reportError", (err: unknown) => {
	queueMicrotask(() => {
		throw err;
	});
});

// require() in ES modules (Bun's import.meta.require), relative to the caller
const globalRequire = core.globalRequire as ((id: string) => unknown) & Record<string, unknown>;
globalRequire.resolve = core.globalRequireResolve;
globalRequire.cache = Module._cache;
globalRequire.extensions = Module._extensions;
defineValue("require", globalRequire);

// the engine's REPL helpers are not part of Bun
for (const name of ["print", "scriptArgs", "execArgv", "argv0", "gc"]) delete g[name];

function parseData(type: string, text: string, filename: string): unknown {
	const Bun = g.Bun as Record<string, { parse(text: string): unknown }>;
	switch (type) {
		case "toml":
			return Bun.TOML.parse(text);
		case "yaml":
			return Bun.YAML.parse(text);
		case "jsonl":
			return Bun.JSONL.parse(text);
	}
	throw new TypeError(`${filename}: unsupported import type '${type}'`);
}

function formatError(err: unknown): string {
	try {
		return `${inspect(err, { colors: false })}\n\nqbun ${process.versions.bun} (${process.platform} ${process.arch})`;
	} catch {
		if (err instanceof Error) {
			const stack = err.stack ?? "";
			return stack.startsWith(`${err.name}`) ? stack : `${err.name}: ${err.message}\n${stack}`;
		}
		return `error: ${String(err)}`;
	}
}

// Bun.plugin() adds "pluginLoad" to this object while plugins are registered
const hooks: Record<string, unknown> = {
	resolve,
	metaResolve,
	requireFrom,
	requireResolveFrom,
	loadCommonJS,
	runMain,
	parseData,
	transpileForLoad,
	formatError,
	runImmediates: timers.runImmediates,
	onUncaughtException: emitUncaught,
	onUnhandledRejection: emitUnhandledRejection,
	beforeExit: emitBeforeExit,
	emitExit(code?: number): number {
		if (code !== undefined) process.exitCode = code;
		return emitExit();
	},
};
bindLoaderHooks(hooks);
core.setHooks(hooks);

// a child spawned with an IPC channel (fork()); workers inherit
// NODE_CHANNEL_FD but only the main thread owns the fd
if (process.env.NODE_CHANNEL_FD !== undefined && (core.info() as { isMainThread: boolean }).isMainThread) {
	setupChildChannel(process);
}
