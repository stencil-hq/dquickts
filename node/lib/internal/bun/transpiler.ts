// internal:bun/transpiler — Bun.Transpiler.
// Construction only validates and normalizes the options: the transformer
// (internal:transpile/index, built on the vendored sucrase) loads on the first transform or scan.

import { invalidArgType, invalidArgValue } from "internal:errors";
import { LOADERS, normalizeTranspilerOptions } from "internal:transpile/options";
import type { Loader, TranspileConfig, TranspilerOptions } from "internal:transpile/options";
import type { Import } from "internal:transpile/index";
import type * as TranspileEngine from "internal:transpile/index";

type StringOrBuffer = string | ArrayBufferView | ArrayBuffer | SharedArrayBuffer;

function toSource(code: StringOrBuffer): string {
	if (typeof code === "string") return code;
	let bytes: Uint8Array;
	if (ArrayBuffer.isView(code)) {
		bytes = new Uint8Array(code.buffer, code.byteOffset, code.byteLength);
	} else if (code instanceof ArrayBuffer) {
		bytes = new Uint8Array(code);
	} else if (typeof SharedArrayBuffer !== "undefined" && code instanceof SharedArrayBuffer) {
		// TextDecoder does not accept shared memory.
		bytes = new Uint8Array(code).slice();
	} else {
		throw invalidArgType("code", "a string, a TypedArray, a DataView or an ArrayBuffer", code);
	}
	return new TextDecoder().decode(bytes);
}

function isLoader(value: unknown): value is Loader {
	return typeof value === "string" && (LOADERS as readonly string[]).includes(value);
}

/** The loader argument of transformSync/transform; an object there is the macro context. */
function loaderArgument(value: unknown, fallback: Loader): Loader {
	if (value === undefined || value === null || typeof value === "object") return fallback;
	if (!isLoader(value)) throw invalidArgValue("loader", value, 'must be one of "js", "jsx", "ts" or "tsx"');
	return value;
}

export class Transpiler {
	#config: TranspileConfig;

	constructor(options?: TranspilerOptions) {
		this.#config = normalizeTranspilerOptions(options);
	}

	/** Transpile TypeScript/JSX to JavaScript. `ctx` (macro context) is accepted and unused. */
	transformSync(code: StringOrBuffer, loader?: Loader | object, ctx?: object): string {
		void ctx;
		// Defer sucrase's top-level 65,536-entry identifier-table initialization until transformation.
		const engine: typeof TranspileEngine = require("internal:transpile/index");
		return engine.transform(toSource(code), loaderArgument(loader, this.#config.loader), this.#config);
	}

	transform(code: StringOrBuffer, loader?: Loader): Promise<string> {
		const { promise, resolve, reject } = Promise.withResolvers<string>();
		try {
			resolve(this.transformSync(code, loader));
		} catch (err) {
			reject(err);
		}
		return promise;
	}

	scan(code: StringOrBuffer): { exports: string[]; imports: Import[] } {
		// Defer sucrase's top-level 65,536-entry identifier-table initialization until scanning.
		const engine: typeof TranspileEngine = require("internal:transpile/index");
		return engine.scan(toSource(code), this.#config.loader, this.#config);
	}

	scanImports(code: StringOrBuffer): Import[] {
		// Defer sucrase's top-level 65,536-entry identifier-table initialization until scanning.
		const engine: typeof TranspileEngine = require("internal:transpile/index");
		return engine.scanImports(toSource(code), this.#config.loader, this.#config);
	}
}
