// node:crypto randomness (port of Node's lib/internal/crypto/random.js): randomBytes, randomFill(Sync),
// randomInt, randomUUID, randomUUIDv7, getRandomValues, generatePrime(Sync), checkPrime(Sync).
import { native, type Bytes } from "internal:crypto/native";
import {
		errRange, errType, isAnyArrayBuffer, isArrayBufferView, nextTick, toArrayBuffer, validateBoolean,
	validateFunction,
	validateInt32,
	validateNumber,
	validateObject,
} from "internal:crypto/util";

const kMaxPossibleLength = 2147483647;

function assertSize(size: unknown, elementSize: number, offset: number, length: number): number {
	validateNumber(size, "size");
	const bytes = size * elementSize;
	if (Number.isNaN(bytes) || bytes > kMaxPossibleLength || bytes < 0) {
		throw errRange("size", `>= 0 && <= ${kMaxPossibleLength}`, size);
	}
	if (bytes + offset > length) throw errRange("size + offset", `<= ${length}`, bytes + offset);
	return bytes >>> 0;
}

function assertOffset(offset: unknown, elementSize: number, length: number): number {
	validateNumber(offset, "offset");
	const bytes = offset * elementSize;
	const maxLength = Math.min(length, kMaxPossibleLength);
	if (Number.isNaN(bytes) || bytes > maxLength || bytes < 0) throw errRange("offset", `>= 0 && <= ${maxLength}`, bytes);
	return bytes >>> 0;
}

function bufArg(buf: unknown): ArrayBuffer | SharedArrayBuffer | ArrayBufferView {
	if (!isAnyArrayBuffer(buf) && !isArrayBufferView(buf)) {
		throw errType("buf", "an instance of ArrayBuffer or ArrayBufferView", buf);
	}
	return buf;
}

function elementSizeOf(buf: ArrayBuffer | SharedArrayBuffer | ArrayBufferView): number {
	const per = (buf as { BYTES_PER_ELEMENT?: number }).BYTES_PER_ELEMENT;
	return typeof per === "number" ? per : 1;
}

export function randomBytes(size: unknown, callback?: unknown): Buffer | undefined {
	validateNumber(size, "size");
	if (Number.isNaN(size) || size > kMaxPossibleLength || size < 0)
		throw errRange("size", `>= 0 && <= ${kMaxPossibleLength}`, size);
	if (callback === undefined) return native.randomBytes(size >>> 0);
	validateFunction(callback, "callback");
	const cb = callback as (err: Error | null, buf?: Buffer) => void;
	native.randomBytes(size >>> 0, (err, buf) => {
		if (err) cb(err);
		else cb(null, buf);
	});
	return undefined;
}

export const pseudoRandomBytes = randomBytes;
export const prng = randomBytes;
export const rng = randomBytes;

export function randomFillSync<T>(buf: T, offset: unknown = 0, size?: unknown): T {
	const b = bufArg(buf);
	const elementSize = elementSizeOf(b);
	const off = assertOffset(offset, elementSize, b.byteLength);
	const n = size === undefined ? b.byteLength - off : assertSize(size, elementSize, off, b.byteLength);
	if (n > 0) native.randomFill(b, off, n);
	return buf;
}

export function randomFill(buf: unknown, offset?: unknown, size?: unknown, callback?: unknown): void {
	const b = bufArg(buf);
	if (typeof offset === "function") {
		callback = offset;
		offset = 0;
		size = undefined;
	} else if (typeof size === "function") {
		callback = size;
		size = undefined;
	}
	validateFunction(callback, "callback");
	const cb = callback as (err: Error | null, buf?: unknown) => void;
	const elementSize = elementSizeOf(b);
	const off = assertOffset(offset ?? 0, elementSize, b.byteLength);
	const n = size === undefined ? b.byteLength - off : assertSize(size, elementSize, off, b.byteLength);
	if (n === 0) {
		nextTick(() => cb(null, buf));
		return;
	}
	native.randomBytes(n, (err, bytes) => {
		if (err) {
			cb(err);
			return;
		}
		const target = isAnyArrayBuffer(b) ? new Uint8Array(b) : new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
		target.set(bytes, off);
		cb(null, buf);
	});
}

// randomInt: 6-byte samples with rejection like Node
const RAND_MAX = 0xffffffffffff;
const randomCache = Buffer.alloc(6 * 1024);
let randomCacheOffset = randomCache.length;

export function randomInt(min: unknown, max?: unknown, callback?: unknown): number | undefined {
	const minNotSpecified = typeof max === "undefined" || typeof max === "function";
	if (minNotSpecified) {
		callback = max;
		max = min;
		min = 0;
	}
	const isSync = typeof callback === "undefined";
	if (!isSync) validateFunction(callback, "callback");
	if (!Number.isSafeInteger(min)) throw errType("min", "a safe integer", min);
	if (!Number.isSafeInteger(max)) throw errType("max", "a safe integer", max);
	const lo = min as number;
	const hi = max as number;
	if (hi <= lo) throw errRange("max", `greater than the value of "min" (${lo})`, hi);
	const range = hi - lo;
	if (!(range <= RAND_MAX)) throw errRange(`max${minNotSpecified ? "" : " - min"}`, `<= ${RAND_MAX}`, range);
	const randLimit = RAND_MAX - (RAND_MAX % range);
	for (;;) {
		if (randomCacheOffset === randomCache.length) {
			native.randomFill(randomCache, 0, randomCache.length);
			randomCacheOffset = 0;
		}
		const x = randomCache.readUIntBE(randomCacheOffset, 6);
		randomCacheOffset += 6;
		if (x < randLimit) {
			const n = (x % range) + lo;
			if (isSync) return n;
			const cb = callback as (err: Error | undefined, n: number) => void;
			nextTick(() => cb(undefined, n));
			return undefined;
		}
	}
}

// randomUUID: a pool of 128 UUIDs worth of entropy
const kUuidBatch = 128;
let uuidPool: Uint8Array | undefined;
let uuidOffset = kUuidBatch * 16;
const kHex: string[] = [];
for (let i = 0; i < 256; i++) kHex.push(i.toString(16).padStart(2, "0"));

function formatUuid(b: Uint8Array, o: number): string {
	return (
		kHex[b[o]] +
		kHex[b[o + 1]] +
		kHex[b[o + 2]] +
		kHex[b[o + 3]] +
		"-" +
		kHex[b[o + 4]] +
		kHex[b[o + 5]] +
		"-" +
		kHex[b[o + 6]] +
		kHex[b[o + 7]] +
		"-" +
		kHex[b[o + 8]] +
		kHex[b[o + 9]] +
		"-" +
		kHex[b[o + 10]] +
		kHex[b[o + 11]] +
		kHex[b[o + 12]] +
		kHex[b[o + 13]] +
		kHex[b[o + 14]] +
		kHex[b[o + 15]]
	);
}

function uuidBytes(disableEntropyCache: boolean): [Uint8Array, number] {
	if (disableEntropyCache) {
		const b = new Uint8Array(16);
		native.randomFill(b, 0, 16);
		return [b, 0];
	}
	if (!uuidPool) uuidPool = new Uint8Array(kUuidBatch * 16);
	if (uuidOffset === kUuidBatch * 16) {
		native.randomFill(uuidPool, 0, uuidPool.length);
		uuidOffset = 0;
	}
	const o = uuidOffset;
	uuidOffset += 16;
	return [uuidPool, o];
}

function entropyOption(options: unknown): boolean {
	if (options === undefined) return false;
	validateObject(options, "options");
	const { disableEntropyCache = false } = options;
	validateBoolean(disableEntropyCache, "options.disableEntropyCache");
	return disableEntropyCache;
}

export function randomUUID(options?: unknown): string {
	const [b, o] = uuidBytes(entropyOption(options));
	b[o + 6] = (b[o + 6] & 0x0f) | 0x40;
	b[o + 8] = (b[o + 8] & 0x3f) | 0x80;
	return formatUuid(b, o);
}

export function randomUUIDv7(options?: unknown): string {
	const [b, o] = uuidBytes(entropyOption(options));
	let ms = Date.now();
	for (let i = 5; i >= 0; i--) {
		b[o + i] = ms % 256;
		ms = Math.floor(ms / 256);
	}
	b[o + 6] = (b[o + 6] & 0x0f) | 0x70;
	b[o + 8] = (b[o + 8] & 0x3f) | 0x80;
	return formatUuid(b, o);
}

export function getRandomValues<T>(data: T): T {
	return crypto.getRandomValues(data);
}

// ------------------------------------------------------------------ primes

function bigintToBytes(v: bigint, name: string): Uint8Array {
	if (v < 0n) throw errRange(name, ">= 0", v);
	let hex = v.toString(16);
	if (hex.length % 2) hex = `0${hex}`;
	return Buffer.from(hex, "hex");
}

function primeOperand(v: unknown, name: string): Bytes {
	if (typeof v === "bigint") return bigintToBytes(v, name);
	if (isAnyArrayBuffer(v) || isArrayBufferView(v)) return v;
	throw errType(name, "of type bigint or an instance of ArrayBuffer, Buffer, TypedArray, or DataView", v);
}

function bytesToBigInt(b: Uint8Array): bigint {
	return b.length ? BigInt(`0x${Buffer.from(b.buffer, b.byteOffset, b.byteLength).toString("hex")}`) : 0n;
}

function bitLength(b: Bytes): number {
	const u = asU8(b);
	let i = 0;
	while (i < u.length && u[i] === 0) i++;
	if (i === u.length) return 0;
	return (u.length - i - 1) * 8 + (32 - Math.clz32(u[i]));
}

function primeOptions(size: unknown, options: unknown) {
	validateInt32(size, "size", 1);
	validateObject(options, "options");
	const { safe = false, bigint = false, add, rem } = options;
	validateBoolean(safe, "options.safe");
	validateBoolean(bigint, "options.bigint");
	let addB: Bytes | null = null;
	let remB: Bytes | null = null;
	if (add !== undefined) {
		addB = primeOperand(add, "options.add");
		if (bitLength(addB) > size) throw errRange("options.add", `<= ${size} bits`, add);
	}
	if (rem !== undefined) {
		remB = primeOperand(rem, "options.rem");
		if (addB && bytesToBigInt(asU8(remB)) >= bytesToBigInt(asU8(addB))) {
			throw errRange("options.rem", "< options.add", rem);
		}
	}
	return { size, safe, bigint, addB, remB };
}

function asU8(b: Bytes): Uint8Array {
	return isAnyArrayBuffer(b) ? new Uint8Array(b) : new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
}

export function generatePrimeSync(size: unknown, options: unknown = {}): ArrayBuffer | bigint {
	const o = primeOptions(size, options);
	const prime = native.generatePrime(o.size, o.safe, o.addB, o.remB);
	return o.bigint ? bytesToBigInt(prime) : toArrayBuffer(prime);
}

export function generatePrime(size: unknown, options: unknown, callback?: unknown): void {
	if (typeof options === "function") {
		callback = options;
		options = {};
	}
	validateFunction(callback, "callback");
	const cb = callback as (err: Error | null, prime?: ArrayBuffer | bigint) => void;
	const o = primeOptions(size, options ?? {});
	native.generatePrime(o.size, o.safe, o.addB, o.remB, (err, prime) => {
		if (err) cb(err);
		else cb(null, o.bigint ? bytesToBigInt(prime) : toArrayBuffer(prime));
	});
}

function checkPrimeArgs(candidate: unknown, options: unknown): { cand: Bytes; checks: number } {
	let cand: Bytes;
	if (typeof candidate === "bigint") cand = bigintToBytes(candidate, "candidate");
	else if (isAnyArrayBuffer(candidate) || isArrayBufferView(candidate)) cand = candidate;
	else
		throw errType(
			"candidate",
			"of type bigint or an instance of ArrayBuffer, Buffer, TypedArray, or DataView",
			candidate,
		);
	validateObject(options, "options");
	const { checks = 0 } = options;
	validateInt32(checks, "options.checks", 0);
	return { cand, checks };
}

export function checkPrimeSync(candidate: unknown, options: unknown = {}): boolean {
	const { cand, checks } = checkPrimeArgs(candidate, options);
	return native.checkPrime(cand, checks);
}

export function checkPrime(candidate: unknown, options: unknown, callback?: unknown): void {
	if (typeof options === "function") {
		callback = options;
		options = {};
	}
	validateFunction(callback, "callback");
	const cb = callback as (err: Error | null, result?: boolean) => void;
	const { cand, checks } = checkPrimeArgs(candidate, options ?? {});
	native.checkPrime(cand, checks, (err, result) => {
		if (err) cb(err);
		else cb(null, result);
	});
}
