// node:crypto Hash, Hmac and Mac (ports of Node's lib/internal/crypto/hash.js and mac.js). The classes are
// stream.Transform subclasses.
import { Transform } from "node:stream";
import { native, type NativeDigest } from "internal:crypto/native";
import {
		cachedResult, encodeOutput, errType, errValue, filterDuplicateStrings, getStringOption, isArrayBufferView,
	normalizeEncoding, toBytes, toData, validateObject, validateString, validateUint32,
} from "internal:crypto/util";
import { prepareSecretKey } from "internal:crypto/keys";
import { codedError } from "internal:errors";

const kHandle = Symbol("kHandle");
const kFinalized = Symbol("kFinalized");
const kAlgorithm = Symbol("kAlgorithm");

function errFinalized(): Error {
	return codedError("ERR_CRYPTO_HASH_FINALIZED", "Digest already called", Error);
}

function checkData(data: unknown, encoding: unknown): string | ArrayBufferView {
	if (typeof data === "string") {
		if (
			encoding !== undefined &&
			encoding !== null &&
			normalizeEncoding(encoding) === undefined &&
			encoding !== "buffer"
		) {
			throw codedError("ERR_UNKNOWN_ENCODING", `Unknown encoding: ${String(encoding)}`, TypeError);
		}
		return data;
	}
	if (!isArrayBufferView(data))
		throw errType("data", "of type string or an instance of Buffer, TypedArray, or DataView", data);
	return data;
}

function unsupportedDigest(): Error {
	return codedError("ERR_OSSL_EVP_UNSUPPORTED", "Digest method not supported", Error);
}

function invalidXofLength(): Error {
	return codedError(
		"ERR_OSSL_EVP_NOT_XOF_OR_INVALID_LENGTH",
		"error:030000B2:digital envelope routines::not XOF or invalid length",
		Error,
	);
}

interface HashOptions {
	outputLength?: number;
	[key: string]: unknown;
}

function outputLengthOf(algorithm: string, options: HashOptions | undefined): number {
	if (options === undefined || options === null || options.outputLength === undefined) return -1;
	validateUint32(options.outputLength, "options.outputLength");
	const info = native.digestInfo(algorithm);
	if (!info) throw unsupportedDigest();
	if (!info.xof && options.outputLength !== info.size) throw invalidXofLength();
	return info.xof ? options.outputLength : -1;
}

export class Hash extends Transform { declare [kHandle]: NativeDigest;
	declare [kFinalized]: boolean;

	constructor(algorithm: unknown, options?: HashOptions) {
		super(options);
		let handle: NativeDigest | null;
		if (algorithm instanceof Hash) {
			if (algorithm[kFinalized]) throw errFinalized();
			const len = options?.outputLength;
			if (len !== undefined) validateUint32(len, "options.outputLength");
			handle = algorithm[kHandle].copy(len ?? -1);
		} else {
			validateString(algorithm, "algorithm");
			const len = outputLengthOf(algorithm, options);
			handle = native.hashNew(algorithm, len);
			if (!handle) throw unsupportedDigest();
		}
		this[kHandle] = handle;
		this[kFinalized] = false;
	}

	copy(options?: HashOptions): Hash {
		if (this[kFinalized]) throw errFinalized();
		return new Hash(this, options);
	}

	_transform(chunk: Uint8Array | string, encoding: string, callback: () => void): void {
		this.update(chunk, encoding);
		callback();
	}

	_flush(callback: () => void): void {
		(this as unknown as { push(chunk: Buffer): void }).push(this.digest() as Buffer);
		callback();
	}

	update(data: unknown, encoding?: unknown): this {
		if (this[kFinalized]) throw errFinalized();
		const d = checkData(data, encoding);
		this[kHandle].update(typeof d === "string" ? toData(d, "data", encoding) : d);
		return this;
	}

	digest(outputEncoding?: unknown): Buffer | string {
		if (this[kFinalized]) throw errFinalized();
		this[kFinalized] = true;
		return encodeOutput(this[kHandle].digest(), outputEncoding);
	} }

export class Hmac extends Transform { declare [kHandle]: NativeDigest;
	declare [kFinalized]: boolean;

	constructor(hmac: unknown, key: unknown, options?: Record<string, unknown>) {
		super(options);
		validateString(hmac, "hmac");
		const encoding = getStringOption(options, "encoding");
		const keyBytes = prepareSecretKey(key, encoding);
		const handle = native.hmacNew(hmac, keyBytes);
		if (!handle) throw codedError("ERR_CRYPTO_INVALID_DIGEST", `Invalid digest: ${hmac}`, TypeError);
		this[kHandle] = handle;
		this[kFinalized] = false;
	}

	_transform(chunk: Uint8Array | string, encoding: string, callback: () => void): void {
		this.update(chunk, encoding);
		callback();
	}

	_flush(callback: () => void): void {
		(this as unknown as { push(chunk: Buffer): void }).push(this.digest() as Buffer);
		callback();
	}

	update(data: unknown, encoding?: unknown): this {
		if (this[kFinalized]) throw errFinalized();
		const d = checkData(data, encoding);
		this[kHandle].update(typeof d === "string" ? toData(d, "data", encoding) : d);
		return this;
	}

	digest(outputEncoding?: unknown): Buffer | string {
		if (this[kFinalized]) return encodeOutput(Buffer.alloc(0), outputEncoding);
		this[kFinalized] = true;
		return encodeOutput(this[kHandle].digest(), outputEncoding);
	} }

/** Mac (Node's createMac(), provider MACs: HMAC, CMAC, GMAC, KMAC, BLAKE2 MACs, Poly1305). */
export class Mac extends Transform { declare [kHandle]: NativeDigest;
	declare [kFinalized]: boolean;
	declare [kAlgorithm]: string;

	constructor(algorithm: unknown, key: unknown, options?: Record<string, unknown>) {
		super(options);
		validateString(algorithm, "algorithm");
		if (algorithm === "") throw errValue("algorithm", algorithm, "must be a non-empty string");
		const keyBytes = prepareSecretKey(key);
		const opts: Record<string, unknown> = {};
		if (options !== undefined) {
			validateObject(options, "options");
			const { digest, cipher, iv, customization, salt, outputLength } = options;
			if (digest !== undefined) {
				validateString(digest, "options.digest");
				opts.digest = digest;
			}
			if (cipher !== undefined) {
				validateString(cipher, "options.cipher");
				opts.cipher = cipher;
			}
			if (iv !== undefined) opts.iv = toBytes(iv, "options.iv");
			if (customization !== undefined) opts.customization = toBytes(customization, "options.customization");
			if (salt !== undefined) opts.salt = toBytes(salt, "options.salt");
			if (outputLength !== undefined) {
				validateUint32(outputLength, "options.outputLength");
				opts.outputLength = outputLength;
			}
		}
		this[kHandle] = native.macNew(algorithm, keyBytes, opts);
		this[kFinalized] = false;
		this[kAlgorithm] = algorithm;
	}

	_transform(chunk: Uint8Array | string, encoding: string, callback: () => void): void {
		this.update(chunk, encoding);
		callback();
	}

	_flush(callback: () => void): void {
		const tag = this.final() as Buffer;
		if (tag.byteLength > 0) (this as unknown as { push(chunk: Buffer): void }).push(tag);
		callback();
	}

	update(data: unknown, inputEncoding?: unknown): this {
		if (this[kFinalized]) throw codedError("ERR_CRYPTO_INVALID_STATE", "Invalid state for operation update", Error);
		const d = checkData(data, inputEncoding);
		this[kHandle].update(typeof d === "string" ? toData(d, "data", inputEncoding) : d);
		return this;
	}

	final(outputEncoding?: unknown): Buffer | string {
		if (this[kFinalized]) throw codedError("ERR_CRYPTO_INVALID_STATE", "Invalid state for operation final", Error);
		this[kFinalized] = true;
		return encodeOutput(this[kHandle].digest(), outputEncoding);
	} }

export function createHash(algorithm: unknown, options?: HashOptions): Hash {
	return new Hash(algorithm, options);
}

export function createHmac(hmac: unknown, key: unknown, options?: Record<string, unknown>): Hmac {
	return new Hmac(hmac, key, options);
}

export function createMac(algorithm: unknown, key: unknown, options?: Record<string, unknown>): Mac {
	return new Mac(algorithm, key, options);
}

/** crypto.hash(algorithm, data[, outputEncoding | options]) */
export function hash(algorithm: unknown, input: unknown, options?: unknown): Buffer | string {
	validateString(algorithm, "algorithm");
	if (typeof input !== "string" && !isArrayBufferView(input)) {
		throw errType("data", "of type string or an instance of Buffer, TypedArray, or DataView", input);
	}
	let outputEncoding: unknown = "hex";
	let outputLength = -1;
	if (typeof options === "string") {
		outputEncoding = options;
	} else if (options !== undefined) {
		validateObject(options, "options");
		if (options.outputEncoding !== undefined) outputEncoding = options.outputEncoding;
		if (options.outputLength !== undefined) outputLength = outputLengthOf(algorithm, options as HashOptions);
	}
	if (outputEncoding !== "hex" && outputEncoding !== "buffer") {
		validateString(outputEncoding, "outputEncoding");
		if (normalizeEncoding(outputEncoding) === undefined && outputEncoding.toLowerCase() !== "buffer") {
			throw errValue("outputEncoding", outputEncoding);
		}
		if (outputEncoding.toLowerCase() === "buffer") outputEncoding = "buffer";
	}
	const out = native.hashOnce(algorithm, input, outputLength);
	if (!out) throw unsupportedDigest();
	return encodeOutput(out, outputEncoding);
}

export const getHashes = cachedResult(() => filterDuplicateStrings(native.getHashes()));

export const getMacs = cachedResult(() => [...new Set(native.getMacs())].sort());
