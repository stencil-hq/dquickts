// node:vm — scripts and contexts. A context is a new realm (JSContext) of the
// same runtime with the JS builtins only (node/worker.c createContext). The
// object given to createContext() (the "sandbox") is tied to the realm's
// global object: its properties appear on the global as accessors reading and
// writing the sandbox (live), and the globals a script creates are copied
// back to the sandbox after each run.

import * as native from "qbun:worker";
import { codedError, invalidArgType, notSupported, validateObject, validateString } from "internal:errors";

interface ContextHandle {
	global(): Record<PropertyKey, unknown>;
}

interface ScriptHandle {
	run(): unknown;
}

interface VMNative {
	createContext(): ContextHandle;
	compileScript(context: ContextHandle | null, source: string, filename: string): ScriptHandle;
	evalInContext(context: ContextHandle | null, source: string, filename: string): unknown;
}
const vmNative = native as unknown as VMNative;

export const constants = Object.freeze({
	USE_MAIN_CONTEXT_DEFAULT_LOADER: Symbol("vm_dynamic_import_main_context_default"),
	DONT_CONTEXTIFY: Symbol("vm_context_no_contextify"),
});

interface ContextRecord {
	handle: ContextHandle;
	global: Record<PropertyKey, unknown>;
	sandbox: Record<PropertyKey, unknown>;
	/** own keys of the fresh global (the builtins) */
	builtins: Set<PropertyKey>;
	/** sandbox keys forwarded by accessors on the global */
	forwarded: Set<PropertyKey>;
	/** script globals copied to the sandbox */
	copied: Set<PropertyKey>;
}

/** the ContextRecord of a contextified sandbox and of its realm's global */
const kContext = Symbol("vm.context");
type Tagged = { [kContext]?: ContextRecord };

export interface RunningScriptOptions {
	filename?: string;
	lineOffset?: number;
	columnOffset?: number;
	displayErrors?: boolean;
	timeout?: number;
	breakOnSigint?: boolean;
}

export interface CreateContextOptions {
	name?: string;
	origin?: string;
	codeGeneration?: { strings?: boolean; wasm?: boolean };
	microtaskMode?: "afterEvaluate";
	importModuleDynamically?: unknown;
}

const DEFAULT_FILENAME = "evalmachine.<anonymous>";

function scriptOptions(options: unknown): RunningScriptOptions {
	if (options === undefined || options === null) return {};
	if (typeof options === "string") return { filename: options };
	validateObject(options, "options");
	const opts = options as RunningScriptOptions;
	if (opts.filename !== undefined) validateString(opts.filename, "options.filename");
	if (opts.timeout !== undefined && (typeof opts.timeout !== "number" || !(opts.timeout > 0))) {
		throw codedError(
			"ERR_OUT_OF_RANGE",
			`The value of "options.timeout" is out of range. It must be a positive integer. Received ${String(opts.timeout)}`,
			RangeError,
		);
	}
	return opts;
}

function withLineOffset(code: string, options: RunningScriptOptions): string {
	const offset = options.lineOffset ?? 0;
	return offset > 0 ? "\n".repeat(offset) + code : code;
}

function syncIn(record: ContextRecord): void {
	const { global, sandbox, forwarded } = record;
	if (global === sandbox) return;
	for (const key of Reflect.ownKeys(sandbox)) {
		if (key === kContext || forwarded.has(key) || record.copied.has(key)) continue;
		try {
			Object.defineProperty(global, key, {
				configurable: true,
				enumerable: Object.prototype.propertyIsEnumerable.call(sandbox, key),
				get: () => sandbox[key],
				set: (value: unknown) => {
					sandbox[key] = value;
				},
			});
			forwarded.add(key);
		} catch {
			// a non-configurable builtin (undefined, NaN, Infinity)
		}
	}
	for (const key of forwarded) {
		if (!Object.hasOwn(sandbox, key)) {
			delete global[key];
			forwarded.delete(key);
		}
	}
}

function syncOut(record: ContextRecord): void {
	const { global, sandbox, forwarded, builtins, copied } = record;
	if (global === sandbox) return;
	const present = new Set<PropertyKey>();
	for (const key of Reflect.ownKeys(global)) {
		if (key === kContext || forwarded.has(key) || builtins.has(key)) continue;
		present.add(key);
		const desc = Object.getOwnPropertyDescriptor(global, key);
		if (!desc) continue;
		const value = "value" in desc ? desc.value : global[key];
		try {
			Object.defineProperty(sandbox, key, {
				value,
				writable: true,
				configurable: true,
				enumerable: desc.enumerable ?? true,
			});
			copied.add(key);
		} catch {
			// the sandbox refuses the property
		}
	}
	for (const key of copied) {
		if (!present.has(key)) {
			delete sandbox[key];
			copied.delete(key);
		}
	}
}

function contextOf(contextifiedObject: unknown, name = "contextifiedObject"): ContextRecord {
	const record =
		typeof contextifiedObject === "object" && contextifiedObject !== null
			? (contextifiedObject as Tagged)[kContext]
			: undefined;
	if (!record) throw invalidArgType(name, "an vm.Context", contextifiedObject);
	return record;
}

function runIn(record: ContextRecord, run: () => unknown): unknown {
	syncIn(record);
	try {
		return run();
	} finally {
		syncOut(record);
	}
}

export function isContext(object: unknown): boolean {
	validateObject(object, "object");
	return Object.hasOwn(object as object, kContext);
}

export function createContext(contextObject: unknown = {}, options: CreateContextOptions = {}): object {
	if (contextObject !== constants.DONT_CONTEXTIFY) {
		validateObject(contextObject, "contextObject");
		if (Object.hasOwn(contextObject as object, kContext)) return contextObject as object;
	}
	if (options !== undefined) validateObject(options, "options");
	if (options?.name !== undefined) validateString(options.name, "options.name");
	const handle = vmNative.createContext();
	const global = handle.global();
	const sandbox =
		contextObject === constants.DONT_CONTEXTIFY ? global : (contextObject as Record<PropertyKey, unknown>);
	const record: ContextRecord = {
		handle,
		global,
		sandbox,
		builtins: new Set(Reflect.ownKeys(global)),
		forwarded: new Set(),
		copied: new Set(),
	};
	const tag = { value: record, configurable: true, enumerable: false, writable: false };
	Object.defineProperty(sandbox, kContext, tag);
	if (global !== sandbox) Object.defineProperty(global, kContext, tag);
	syncIn(record);
	return sandbox;
}

export class Script {
	readonly cachedDataRejected: boolean | undefined;
	readonly sourceMapURL: string | undefined;
	cachedData: Buffer | undefined;
	#source: string;
	#filename: string;
	#compiled: ScriptHandle;

	constructor(code: unknown, options?: unknown) {
		const source = `${code}`;
		const opts = scriptOptions(options) as RunningScriptOptions & { cachedData?: unknown; produceCachedData?: boolean };
		this.#filename = opts.filename ?? DEFAULT_FILENAME;
		this.#source = withLineOffset(source, opts);
		this.#compiled = vmNative.compileScript(null, this.#source, this.#filename);
		this.cachedDataRejected = opts.cachedData === undefined ? undefined : false;
		const map = /\/\/[#@] sourceMappingURL=([^\s'"]+)\s*$/.exec(source);
		this.sourceMapURL = map ? map[1] : undefined;
		if (opts.produceCachedData) this.cachedData = Buffer.alloc(0);
	}

	runInThisContext(options?: unknown): unknown {
		scriptOptions(options);
		return this.#compiled.run();
	}

	runInContext(contextifiedObject: unknown, options?: unknown): unknown {
		const record = contextOf(contextifiedObject);
		scriptOptions(options);
		return runIn(record, () => vmNative.evalInContext(record.handle, this.#source, this.#filename));
	}

	runInNewContext(contextObject?: unknown, options?: unknown): unknown {
		const context = createContext(contextObject ?? {}, contextOptions(options));
		return this.runInContext(context, options);
	}

	/** V8 code cache: qbun has none, an empty Buffer. */
	createCachedData(): Buffer {
		return Buffer.alloc(0);
	}
}

function contextOptions(options: unknown): CreateContextOptions {
	if (options === null || typeof options !== "object") return {};
	const opts = options as {
		contextName?: string;
		contextOrigin?: string;
		contextCodeGeneration?: { strings?: boolean; wasm?: boolean };
		microtaskMode?: "afterEvaluate";
	};
	return {
		name: opts.contextName,
		origin: opts.contextOrigin,
		codeGeneration: opts.contextCodeGeneration,
		microtaskMode: opts.microtaskMode,
	};
}

export function createScript(code: unknown, options?: unknown): Script {
	return new Script(code, options);
}

export function runInThisContext(code: unknown, options?: unknown): unknown {
	return new Script(code, options).runInThisContext(options);
}

export function runInContext(code: unknown, contextifiedObject: unknown, options?: unknown): unknown {
	const record = contextOf(contextifiedObject);
	const opts = scriptOptions(options);
	const source = withLineOffset(`${code}`, opts);
	return runIn(record, () => vmNative.evalInContext(record.handle, source, opts.filename ?? DEFAULT_FILENAME));
}

export function runInNewContext(code: unknown, contextObject?: unknown, options?: unknown): unknown {
	const context = createContext(contextObject ?? {}, contextOptions(options));
	return runInContext(code, context, options);
}

export interface CompileFunctionOptions {
	filename?: string;
	lineOffset?: number;
	columnOffset?: number;
	cachedData?: unknown;
	produceCachedData?: boolean;
	parsingContext?: object;
	contextExtensions?: object[];
	importModuleDynamically?: unknown;
}

export function compileFunction(
	code: unknown,
	params: unknown = [],
	options: CompileFunctionOptions = {},
): (...args: unknown[]) => unknown {
	validateString(code, "code");
	if (!Array.isArray(params)) throw invalidArgType("params", "an instance of Array", params);
	params.forEach((p: unknown, i: number) => validateString(p, `params[${i}]`));
	validateObject(options, "options");
	if (options.filename !== undefined) validateString(options.filename, "options.filename");
	const extensions = options.contextExtensions ?? [];
	if (!Array.isArray(extensions)) throw invalidArgType("options.contextExtensions", "an instance of Array", extensions);
	extensions.forEach((e: unknown, i: number) => validateObject(e, `options.contextExtensions[${i}]`));
	const record =
		options.parsingContext === undefined ? null : contextOf(options.parsingContext, "options.parsingContext");
	const body = code.startsWith("#!") ? `//${code.slice(2)}` : code;
	const offset = "\n".repeat(Math.max(0, options.lineOffset ?? 0));
	const filename = options.filename ?? "";
	const handle = record ? record.handle : null;
	let fn: (...args: unknown[]) => unknown;
	if (extensions.length === 0) {
		fn = vmNative.evalInContext(handle, `${offset}(function (${params.join(", ")}) {${body}\n})`, filename) as (
			...args: unknown[]
		) => unknown;
	} else {
		let open = "";
		let close = "";
		for (let i = 0; i < extensions.length; i++) {
			open += `with (__qbun_vm_ext[${i}]) { `;
			close += " }";
		}
		const factory = vmNative.evalInContext(
			handle,
			`${offset}(function (__qbun_vm_ext) { ${open}return function (${params.join(", ")}) {${body}\n};${close} })`,
			filename,
		) as (ext: object[]) => (...args: unknown[]) => unknown;
		fn = factory(extensions);
	}
	if (options.produceCachedData)
		Object.defineProperty(fn, "cachedData", { value: Buffer.alloc(0), configurable: true, writable: true });
	if (options.cachedData !== undefined)
		Object.defineProperty(fn, "cachedDataRejected", { value: false, configurable: true, writable: true });
	return fn;
}

export function measureMemory(
	options: { mode?: "summary" | "detailed"; execution?: "default" | "eager" } = {},
): Promise<unknown> {
	validateObject(options, "options");
	const mode = options.mode ?? "summary";
	if (mode !== "summary" && mode !== "detailed")
		return Promise.reject(
			codedError(
				"ERR_INVALID_ARG_VALUE",
				`The argument 'options.mode' must be one of: 'summary', 'detailed'. Received '${String(mode)}'`,
				TypeError,
			),
		);
	const mem = process.memoryUsage();
	const total = { jsMemoryEstimate: mem.heapUsed, jsMemoryRange: [mem.heapUsed, mem.heapTotal] };
	return Promise.resolve(mode === "summary" ? { total } : { total, current: total, other: [] });
}

/** vm.Module and its subclasses need V8's module records: not supported. */
export class Module {
	constructor() {
		throw notSupported("vm.Module");
	}
}

export class SourceTextModule extends Module {
	constructor() {
		super();
		throw notSupported("vm.SourceTextModule");
	}
}

export class SyntheticModule extends Module {
	constructor() {
		super();
		throw notSupported("vm.SyntheticModule");
	}
}

export default {
	Script,
	createContext,
	createScript,
	runInContext,
	runInNewContext,
	runInThisContext,
	isContext,
	compileFunction,
	measureMemory,
	Module,
	SourceTextModule,
	SyntheticModule,
	constants,
};
