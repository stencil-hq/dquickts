// node:crypto key derivation (ports of Node's pbkdf2.js, scrypt.js, hkdf.js and argon2.js).
import { native, type Bytes } from "internal:crypto/native";
import {
	errRange,
	errType,
	isAnyArrayBuffer,
	isArrayBufferView,
	toArrayBuffer,
	toBytes,
	validateFunction,
	validateInt32,
	validateInteger,
	validateObject,
	validateOneOf,
	validateString,
	validateUint32,
} from "internal:crypto/util";
import { createSecretKey, isKeyObject, kHandle, type KeyObject } from "internal:crypto/keys";
import { codedError } from "internal:errors";

const kMaxLength = 2 ** 53 - 1;

function invalidDigest(digest: string): Error {
	return codedError("ERR_CRYPTO_INVALID_DIGEST", `Invalid digest: ${digest}`, TypeError);
}

function checkDigest(digest: string): void {
	const info = native.digestInfo(digest);
	if (!info || info.xof) throw invalidDigest(digest);
}

// ------------------------------------------------------------------ pbkdf2

function pbkdf2Check(password: unknown, salt: unknown, iterations: unknown, keylen: unknown, digest: unknown) {
	validateString(digest, "digest");
	const p = toBytes(password, "password");
	const s = toBytes(salt, "salt");
	validateInt32(iterations, "iterations", 1);
	validateInt32(keylen, "keylen", 0);
	checkDigest(digest);
	return { p, s, iterations, keylen, digest };
}

export function pbkdf2(
	password: unknown,
	salt: unknown,
	iterations: unknown,
	keylen: unknown,
	digest: unknown,
	callback?: unknown,
): void {
	if (typeof digest === "function") {
		callback = digest;
		digest = undefined;
	}
	const c = pbkdf2Check(password, salt, iterations, keylen, digest);
	validateFunction(callback, "callback");
	const cb = callback as (err: Error | null, key?: Buffer) => void;
	native.pbkdf2(c.p, c.s, c.iterations, c.keylen, c.digest, (err, key) => {
		if (err) cb(err);
		else cb(null, key);
	});
}

export function pbkdf2Sync(
	password: unknown,
	salt: unknown,
	iterations: unknown,
	keylen: unknown,
	digest: unknown,
): Buffer {
	const c = pbkdf2Check(password, salt, iterations, keylen, digest);
	return native.pbkdf2(c.p, c.s, c.iterations, c.keylen, c.digest);
}

// ------------------------------------------------------------------ scrypt

const scryptDefaults = { N: 16384, r: 8, p: 1, maxmem: 32 << 20 };

function scryptParamError(): Error {
	return codedError("ERR_CRYPTO_SCRYPT_INVALID_PARAMETER", "Invalid scrypt parameter", TypeError);
}

function scryptCheck(password: unknown, salt: unknown, keylen: unknown, options: unknown) {
	const pw = toBytes(password, "password");
	const s = toBytes(salt, "salt");
	validateInt32(keylen, "keylen", 0);
	let { N, r, p, maxmem } = scryptDefaults;
	if (options !== undefined && options !== null && options !== scryptDefaults) {
		const o = options as Record<string, unknown>;
		const hasN = o.N !== undefined;
		if (hasN) {
			validateUint32(o.N, "N");
			N = o.N;
		}
		if (o.cost !== undefined) {
			if (hasN) throw scryptParamError();
			validateUint32(o.cost, "cost");
			N = o.cost;
		}
		const hasR = o.r !== undefined;
		if (hasR) {
			validateUint32(o.r, "r");
			r = o.r;
		}
		if (o.blockSize !== undefined) {
			if (hasR) throw scryptParamError();
			validateUint32(o.blockSize, "blockSize");
			r = o.blockSize;
		}
		const hasP = o.p !== undefined;
		if (hasP) {
			validateUint32(o.p, "p");
			p = o.p;
		}
		if (o.parallelization !== undefined) {
			if (hasP) throw scryptParamError();
			validateUint32(o.parallelization, "parallelization");
			p = o.parallelization;
		}
		if (o.maxmem !== undefined) {
			validateInteger(o.maxmem, "maxmem", 0);
			maxmem = o.maxmem;
		}
		if (N === 0) N = scryptDefaults.N;
		if (r === 0) r = scryptDefaults.r;
		if (p === 0) p = scryptDefaults.p;
		if (maxmem === 0) maxmem = scryptDefaults.maxmem;
	}
	return { pw, s, keylen, N, r, p, maxmem };
}

export function scrypt(password: unknown, salt: unknown, keylen: unknown, options?: unknown, callback?: unknown): void {
	if (callback === undefined) {
		callback = options;
		options = undefined;
	}
	const c = scryptCheck(password, salt, keylen, options);
	validateFunction(callback, "callback");
	const cb = callback as (err: Error | null, key?: Buffer) => void;
	native.scrypt(c.pw, c.s, c.keylen, c.N, c.r, c.p, c.maxmem, (err, key) => {
		if (err) cb(err);
		else cb(null, key);
	});
}

export function scryptSync(password: unknown, salt: unknown, keylen: unknown, options?: unknown): Buffer {
	const c = scryptCheck(password, salt, keylen, options);
	return native.scrypt(c.pw, c.s, c.keylen, c.N, c.r, c.p, c.maxmem);
}

// -------------------------------------------------------------------- hkdf

function byteSource(val: unknown, name: string): Bytes {
	if (typeof val === "string") return Buffer.from(val, "utf8");
	if (isAnyArrayBuffer(val) || isArrayBufferView(val)) return val;
	throw errType(name, "of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView", val);
}

function hkdfKey(key: unknown): KeyObject {
	if (isKeyObject(key)) return key;
	if (isAnyArrayBuffer(key)) return createSecretKey(key);
	const k = typeof key === "string" ? Buffer.from(key, "utf8") : key;
	if (!isArrayBufferView(k)) {
		throw errType(
			"ikm",
			"of type string or an instance of SecretKeyObject, ArrayBuffer, TypedArray, DataView, or Buffer",
			key,
		);
	}
	return createSecretKey(k);
}

function hkdfCheck(hash: unknown, key: unknown, salt: unknown, info: unknown, length: unknown) {
	validateString(hash, "digest");
	const ko = hkdfKey(key);
	const s = byteSource(salt, "salt");
	const i = byteSource(info, "info");
	validateInteger(length, "length", 0, kMaxLength);
	if (i.byteLength > 1024) throw errRange("info", "must not contain more than 1024 bytes", i.byteLength);
	if (ko.type !== "secret")
		throw codedError(
			"ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE",
			`Invalid key object type ${ko.type}, expected secret.`,
			TypeError,
		);
	const infoD = native.digestInfo(hash);
	if (!infoD || infoD.xof) throw invalidDigest(hash);
	if (length > infoD.size * 255) throw codedError("ERR_CRYPTO_INVALID_KEYLEN", "Invalid key length", RangeError);
	return { hash, ikm: ko[kHandle] as Buffer, s, i, length };
}

export function hkdf(
	digest: unknown,
	key: unknown,
	salt: unknown,
	info: unknown,
	keylen: unknown,
	callback: unknown,
): void {
	const c = hkdfCheck(digest, key, salt, info, keylen);
	validateFunction(callback, "callback");
	const cb = callback as (err: Error | null, key?: ArrayBuffer) => void;
	native.hkdf(c.hash, c.ikm, c.s, c.i, c.length, (err, out) => {
		if (err) cb(err);
		else cb(null, toArrayBuffer(out));
	});
}

export function hkdfSync(digest: unknown, key: unknown, salt: unknown, info: unknown, keylen: unknown): ArrayBuffer {
	const c = hkdfCheck(digest, key, salt, info, keylen);
	return toArrayBuffer(native.hkdf(c.hash, c.ikm, c.s, c.i, c.length));
}

// ------------------------------------------------------------------ argon2

const kMaxU32 = 2 ** 32 - 1;

function argon2Check(algorithm: unknown, parameters: unknown) {
	validateString(algorithm, "algorithm");
	validateOneOf(algorithm, "algorithm", ["argon2d", "argon2i", "argon2id"]);
	validateObject(parameters, "parameters");
	const { parallelism, tagLength, memory, passes } = parameters;
	const message = toBytes(parameters.message, "parameters.message");
	validateInteger(message.byteLength, "parameters.message.byteLength", 0, kMaxU32);
	const nonce = toBytes(parameters.nonce, "parameters.nonce");
	validateInteger(nonce.byteLength, "parameters.nonce.byteLength", 8, kMaxU32);
	validateInteger(parallelism, "parameters.parallelism", 1, 2 ** 24 - 1);
	validateInteger(tagLength, "parameters.tagLength", 4, kMaxU32);
	validateInteger(memory, "parameters.memory", 8 * parallelism, kMaxU32);
	validateUint32(passes, "parameters.passes", true);
	let secret: Bytes | null = null;
	if (parameters.secret !== undefined) {
		secret = toBytes(parameters.secret, "parameters.secret");
		validateInteger(secret.byteLength, "parameters.secret.byteLength", 0, kMaxU32);
	}
	let associatedData: Bytes | null = null;
	if (parameters.associatedData !== undefined) {
		associatedData = toBytes(parameters.associatedData, "parameters.associatedData");
		validateInteger(associatedData.byteLength, "parameters.associatedData.byteLength", 0, kMaxU32);
	}
	return { algorithm, message, nonce, secret, associatedData, parallelism, tagLength, memory, passes };
}

export function argon2(algorithm: unknown, parameters: unknown, callback: unknown): void {
	const c = argon2Check(algorithm, parameters);
	validateFunction(callback, "callback");
	const cb = callback as (err: Error | null, key?: Buffer) => void;
	native.argon2(
		c.algorithm,
		c.message,
		c.nonce,
		c.secret,
		c.associatedData,
		c.parallelism,
		c.memory,
		c.passes,
		c.tagLength,
		(err, out) => {
			if (err) cb(err);
			else cb(null, out);
		},
	);
}

export function argon2Sync(algorithm: unknown, parameters: unknown): Buffer {
	const c = argon2Check(algorithm, parameters);
	return native.argon2(
		c.algorithm,
		c.message,
		c.nonce,
		c.secret,
		c.associatedData,
		c.parallelism,
		c.memory,
		c.passes,
		c.tagLength,
	);
}
