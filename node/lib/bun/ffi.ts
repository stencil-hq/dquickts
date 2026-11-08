import * as native from "qbun:ffi";
import { Buffer } from "node:buffer";
import { codedError, invalidArgType } from "internal:errors";

export enum FFIType {
	char = 0,
	int8_t = 1,
	i8 = 1,
	uint8_t = 2,
	u8 = 2,
	int16_t = 3,
	i16 = 3,
	uint16_t = 4,
	u16 = 4,
	int32_t = 5,
	i32 = 5,
	int = 5,
	uint32_t = 6,
	u32 = 6,
	int64_t = 7,
	i64 = 7,
	uint64_t = 8,
	u64 = 8,
	double = 9,
	f64 = 9,
	float = 10,
	f32 = 10,
	bool = 11,
	ptr = 12,
	pointer = 12,
	void = 13,
	cstring = 14,
	i64_fast = 15,
	u64_fast = 16,
	function = 17,
	napi_env = 18,
	napi_value = 19,
	buffer = 20,
	buffer_length = 21,
}
export type Pointer = number;
type Type = FFIType | keyof typeof FFIType;
export interface FFIFunction {
	args?: readonly Type[];
	returns?: Type;
	ptr?: number | bigint;
	threadsafe?: boolean;
}
type Symbols = Record<string, FFIFunction>;
type Callable = ((...args: unknown[]) => unknown) & {
	ptr: number;
	native: (...args: unknown[]) => unknown;
	close(): void;
};
export const suffix: string = native.suffix;
function code(type: Type | undefined): number {
	if (type === undefined) return FFIType.void;
	const value = typeof type === "string" ? FFIType[type] : type;
	if (typeof value !== "number" || !Number.isInteger(value) || value < 0 || value > 21)
		throw codedError("ERR_INVALID_ARG_VALUE", `Unknown FFI type: ${String(type)}`, TypeError);
	if (value === 18 || value === 19)
		throw codedError("ERR_NOT_SUPPORTED", "napi_env and napi_value cannot be used with bun:ffi");
	return value;
}
function argument(value: unknown): unknown {
	if (value instanceof CString) return value.ptr + value.byteOffset;
	if (value instanceof JSCallback) return value.ptr;
	return value;
}
function create(definition: FFIFunction, pointer: number | bigint, library?: ArrayBuffer): Callable {
	const args = (definition.args ?? []).map(code);
	const returns = code(definition.returns);
	const raw = native.function(pointer, args, returns, library) as (...args: unknown[]) => unknown;
	let closed = false;
	const fn = function (...values: unknown[]): unknown {
		if (closed) throw codedError("ERR_INVALID_STATE", "FFI function has been closed");
		const result = raw(...values.map(argument));
		return returns === FFIType.cstring ? new CString(result as number) : result;
	} as Callable;
	fn.ptr = Number(pointer);
	fn.native = raw;
	fn.close = () => {
		closed = true;
	};
	return fn;
}
export function dlopen(path: string | URL, definitions: Symbols): { symbols: Record<string, Callable>; close(): void } {
	let filename: string;
	if (typeof path === "string") filename = path;
	else if (path instanceof URL && path.protocol === "file:") filename = decodeURIComponent(path.pathname);
	else throw invalidArgType("path", "a string or file URL", path);
	const library = native.open(filename) as ArrayBuffer;
	const symbols: Record<string, Callable> = Object.create(null);
	try {
		for (const name in definitions) {
			const definition = definitions[name];
			symbols[name] = create(definition, definition.ptr ?? native.symbol(library, name), library);
		}
	} catch (error) {
		native.close(library);
		throw error;
	}
	return {
		symbols,
		close() {
			native.close(library);
		},
	};
}
export function linkSymbols(definitions: Symbols): { symbols: Record<string, Callable>; close(): void } {
	const symbols: Record<string, Callable> = Object.create(null);
	for (const name in definitions) {
		const def = definitions[name];
		if (def.ptr === undefined)
			throw codedError("ERR_INVALID_ARG_VALUE", `Missing pointer for symbol ${name}`, TypeError);
		symbols[name] = create(def, def.ptr);
	}
	return {
		symbols,
		close() {
			for (const name in symbols) symbols[name].close();
		},
	};
}
export function CFunction(definition: FFIFunction & { ptr: number | bigint }): Callable {
	return create(definition, definition.ptr);
}
export function ptr(view: ArrayBufferLike | ArrayBufferView, byteOffset = 0): number {
	return native.ptr(view, byteOffset);
}
// With five arguments Bun places the context before the deallocator function.
export function toArrayBuffer(
	pointer: number | bigint,
	byteOffset = 0,
	byteLength?: number,
	contextOrDeallocator?: number | bigint,
	deallocator?: number | bigint,
): ArrayBuffer {
	return native.toArrayBuffer(pointer, byteOffset, byteLength, contextOrDeallocator, deallocator);
}
export function toBuffer(
	pointer: number | bigint,
	byteOffset = 0,
	byteLength?: number,
	contextOrDeallocator?: number | bigint,
	deallocator?: number | bigint,
): Buffer {
	return Buffer.from(toArrayBuffer(pointer, byteOffset, byteLength, contextOrDeallocator, deallocator));
}
class CStringValue extends String {
	readonly ptr: number;
	readonly byteOffset: number;
	readonly byteLength: number;
	constructor(pointer: number | bigint | null, byteOffset = 0, byteLength?: number) {
		const value = native.cstring(pointer, byteOffset, byteLength) as string;
		super(value);
		this.ptr = Number(pointer ?? 0);
		this.byteOffset = byteOffset;
		this.byteLength = byteLength ?? (this.ptr ? native.toArrayBuffer(pointer, byteOffset).byteLength : 0);
	}
	get arrayBuffer(): ArrayBuffer {
		return toArrayBuffer(this.ptr, this.byteOffset, this.byteLength);
	}
}
export type CString = CStringValue;
interface CStringConstructor {
	new (pointer: number | bigint | null, byteOffset?: number, byteLength?: number): CStringValue;
	(pointer: number | bigint | null, byteOffset?: number, byteLength?: number): CStringValue;
	prototype: CStringValue;
}
export const CString = function CString(
	pointer: number | bigint | null,
	byteOffset = 0,
	byteLength?: number,
): CStringValue {
	return new CStringValue(pointer, byteOffset, byteLength);
} as unknown as CStringConstructor; // The factory deliberately supports both call and construct.
CString.prototype = CStringValue.prototype;
export class JSCallback {
	ptr: number | null;
	readonly threadsafe: boolean;
	private handle: ArrayBuffer | null;
	constructor(callback: (...args: unknown[]) => unknown, definition: FFIFunction = {}) {
		if (typeof callback !== "function") throw invalidArgType("callback", "a function", callback);
		const args = (definition.args ?? []).map(code),
			returns = code(definition.returns);
		const wrapped = (...values: unknown[]) =>
			argument(
				callback(
					...values.map((value, index) => (args[index] === FFIType.cstring ? new CString(value as number) : value)),
				),
			);
		const result = native.callback(wrapped, args, returns, !!definition.threadsafe) as {
			ptr: number;
			handle: ArrayBuffer;
		};
		this.ptr = result.ptr;
		this.handle = result.handle;
		this.threadsafe = !!definition.threadsafe;
	}
	close(): void {
		if (this.handle) {
			native.closeCallback(this.handle);
			this.handle = null;
			this.ptr = null;
		}
	}
}
type Reader = (pointer: number | bigint, offset?: number) => number;
type BigReader = (pointer: number | bigint, offset?: number) => bigint;
const reader =
	(type: number): Reader =>
	(pointer, offset = 0) =>
		native.read(pointer, offset, type);
export const read = {
	u8: reader(2),
	i8: reader(1),
	u16: reader(4),
	i16: reader(3),
	u32: reader(6),
	i32: reader(5),
	u64: reader(8) as unknown as BigReader,
	i64: reader(7) as unknown as BigReader,
	f32: reader(10),
	f64: reader(9),
	ptr: reader(12),
	intptr: reader(12),
};
function source(definition: FFIFunction, name: string): string {
	const names = [
		"char",
		"int8_t",
		"uint8_t",
		"int16_t",
		"uint16_t",
		"int32_t",
		"uint32_t",
		"int64_t",
		"uint64_t",
		"double",
		"float",
		"bool",
		"void *",
		"void",
		"char *",
		"int64_t",
		"uint64_t",
		"void *",
		"napi_env",
		"napi_value",
		"void *",
		"size_t",
	];
	return `/* qbun uses libffi (FFI_DEFAULT_ABI); no C wrapper is compiled. */\n${names[code(definition.returns)]} ${name}(${(definition.args ?? []).map((t, i) => `${names[code(t)]} arg${i}`).join(", ") || "void"});`;
}
export function viewSource(definitions: Symbols | FFIFunction, isCallback = false): string | string[] {
	if (isCallback) return source(definitions as FFIFunction, "callback");
	const result: string[] = [];
	for (const name in definitions) result.push(source((definitions as Symbols)[name], name));
	return result;
}
export function cc(_options: unknown): never {
	throw codedError("ERR_NOT_SUPPORTED", "bun:ffi cc is not supported: qbun does not include TinyCC");
}
export default {
	dlopen,
	FFIType,
	suffix,
	ptr,
	toArrayBuffer,
	toBuffer,
	read,
	CString,
	JSCallback,
	linkSymbols,
	CFunction,
	viewSource,
	cc,
};
