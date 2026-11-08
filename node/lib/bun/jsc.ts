import * as core from "qbun:core";
import * as clone from "qbun:worker";
import { notSupported } from "internal:errors";

interface CloneNative {
	serialize(value: unknown): Buffer;
	deserialize(bytes: ArrayBufferLike | ArrayBufferView): unknown;
}
const native = clone as unknown as CloneNative;

/**
 * Structured clone serialization (qbun's format, readable by qbun only): a
 * SharedArrayBuffer, or a Buffer with `binaryType: "nodebuffer"`.
 */
export function serialize(value: unknown, options?: { binaryType?: "arraybuffer" | "nodebuffer" }): SharedArrayBuffer | Buffer {
	const bytes = native.serialize(value);
	if (options?.binaryType === "nodebuffer") return bytes;
	const out = new SharedArrayBuffer(bytes.length);
	new Uint8Array(out).set(bytes);
	return out;
}

export function deserialize(value: ArrayBufferLike | ArrayBufferView): unknown {
	if (!ArrayBuffer.isView(value) && !(value instanceof ArrayBuffer) && !(value instanceof SharedArrayBuffer))
		throw new TypeError("First argument must be an ArrayBuffer");
	return native.deserialize(value);
}
export const memoryUsage = core.memoryUsage;
export function heapStats(): object {
	const m = core.memoryUsage();
	return {
		heapSize: m.heapUsed,
		heapCapacity: m.heapTotal,
		extraMemorySize: m.external,
		objectCount: 0,
		protectedObjectCount: 0,
		globalObjectCount: 1,
		protectedGlobalObjectCount: 1,
		objectTypeCounts: {},
		protectedObjectTypeCounts: {},
	};
}
export function startRemoteDebugger(): void {}
export function setTimeZone(zone: string): void {
	core.setenv("TZ", zone);
}
export function describe(value: unknown): string {
	return Object.prototype.toString.call(value);
}
export const describeArray = describe,
	jscDescribe = describe;
export const gcAndSweep = core.gc,
	fullGC = core.gc,
	edenGC = core.gc;
let seed = 0;
export function getRandomSeed(): number {
	return seed;
}
export function setRandomSeed(value: number): void {
	seed = value;
}
export const drainMicrotasks = core.drainMicrotasks;
export function numberOfDFGCompiles(): number {
	return 0;
}
export const numberOfDFGCompilesFor = numberOfDFGCompiles,
	numberOfFTLCompiles = numberOfDFGCompiles,
	totalCompileTime = numberOfDFGCompiles;
export function estimateShallowMemoryUsageOf(value: unknown): number {
	return typeof value === "string"
		? value.length * 2
		: ArrayBuffer.isView(value) || value instanceof ArrayBuffer
			? value.byteLength
			: typeof value === "object" && value !== null
				? 32
				: 8;
}
export function profile(): never {
	throw notSupported("JavaScriptCore profiling");
}
export function callerSourceOrigin(): string {
	return core.callerFilename(1) ?? "";
}
export default {
	serialize,
	deserialize,
	memoryUsage,
	heapStats,
	startRemoteDebugger,
	setTimeZone,
	describe,
	describeArray,
	jscDescribe,
	gcAndSweep,
	fullGC,
	edenGC,
	getRandomSeed,
	setRandomSeed,
	drainMicrotasks,
	numberOfDFGCompiles,
	numberOfDFGCompilesFor,
	numberOfFTLCompiles,
	totalCompileTime,
	estimateShallowMemoryUsageOf,
	profile,
	callerSourceOrigin,
};
