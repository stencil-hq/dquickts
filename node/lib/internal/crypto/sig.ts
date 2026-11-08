// node:crypto signatures (port of Node's lib/internal/crypto/sig.js and kem.js): Sign, Verify, createSign,
// createVerify, the one-shot sign() / verify() and encapsulate() / decapsulate().
import { Writable } from "node:stream";
import { native, type Bytes, type PKey } from "internal:crypto/native";
import {
		encodeOutput, errType, errValue, isArrayBufferView, toBytes, validateFunction, validateInt32,
	validateString,
} from "internal:crypto/util";
import { preparePrivateKey, preparePublicOrPrivateKey } from "internal:crypto/keys";
import { codedError } from "internal:errors";

const kChunks = Symbol("kChunks");
const kAlgorithm = Symbol("kAlgorithm");
const kDone = Symbol("kDone");

interface SignOptions {
	padding: number;
	saltLength: number | undefined;
	dsaEncoding: number;
	context: Bytes | null;
}

function signOptions(options: unknown): SignOptions {
	const o = (options !== null && typeof options === "object" && !isArrayBufferView(options) ? options : {}) as Record<
		string,
		unknown
	>;
	let padding = -1;
	let saltLength: number | undefined;
	let dsaEncoding = 0;
	if (o.padding !== undefined) {
		validateInt32(o.padding, "key.padding");
		padding = o.padding;
	}
	if (o.saltLength !== undefined) {
		validateInt32(o.saltLength, "key.saltLength");
		saltLength = o.saltLength;
	}
	if (o.dsaEncoding !== undefined) {
		if (o.dsaEncoding === "der") dsaEncoding = 0;
		else if (o.dsaEncoding === "ieee-p1363") dsaEncoding = 1;
		else throw errValue("options.dsaEncoding", o.dsaEncoding);
	}
	let context: Bytes | null = null;
	if (o.context !== undefined) context = toBytes(o.context, "options.context");
	return { padding, saltLength, dsaEncoding, context };
}

function checkDigest(algorithm: string): void {
	if (!native.digestInfo(algorithm))
		throw codedError("ERR_CRYPTO_INVALID_DIGEST", `Invalid digest: ${algorithm}`, TypeError);
}

function concat(chunks: Uint8Array[]): Buffer {
	return chunks.length === 1
		? Buffer.from(chunks[0].buffer, chunks[0].byteOffset, chunks[0].byteLength)
		: Buffer.concat(chunks);
}

class SignBase extends Writable { declare [kChunks]: Uint8Array[];
	declare [kAlgorithm]: string;
	declare [kDone]: boolean;

	constructor(algorithm: unknown, options?: unknown) {
		super(options);
		validateString(algorithm, "algorithm");
		checkDigest(algorithm);
		this[kChunks] = [];
		this[kAlgorithm] = algorithm;
		this[kDone] = false;
	}

	_write(chunk: Uint8Array | string, encoding: string, callback: () => void): void {
		this.update(chunk, encoding);
		callback();
	}

	update(data: unknown, encoding?: unknown): this {
		if (typeof data !== "string" && !isArrayBufferView(data)) {
			throw errType("data", "of type string or an instance of Buffer, TypedArray, or DataView", data);
		}
		if (this[kDone]) throw codedError("ERR_CRYPTO_INVALID_STATE", "Invalid state for operation update", Error);
		const bytes = toBytes(data, "data", encoding) as ArrayBufferView;
		// copy: the caller may reuse its buffer before sign()/verify()
		this[kChunks].push(
			new Uint8Array(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer),
		);
		return this;
	} }

export class Sign extends SignBase {
	sign(privateKey: unknown, outputEncoding?: unknown): Buffer | string {
		if (!privateKey) throw codedError("ERR_CRYPTO_SIGN_KEY_REQUIRED", "No key provided to sign", Error);
		const pkey = preparePrivateKey(privateKey);
		const o = signOptions(privateKey);
		const data = concat(this[kChunks]);
		this[kChunks] = [];
		this[kDone] = true;
		const sig = native.sign(pkey, data, this[kAlgorithm], o.padding, o.saltLength, o.dsaEncoding, null);
		return encodeOutput(sig, outputEncoding);
	}
}

export class Verify extends SignBase {
	verify(object: unknown, signature: unknown, sigEncoding?: unknown): boolean {
		const pkey = preparePublicOrPrivateKey(object);
		const o = signOptions(object);
		const sig = toBytes(signature, "signature", sigEncoding);
		const data = concat(this[kChunks]);
		this[kChunks] = [];
		this[kDone] = true;
		return native.verify(pkey, data, sig, this[kAlgorithm], o.padding, o.saltLength, o.dsaEncoding, null);
	}
}

export function createSign(algorithm: unknown, options?: unknown): Sign {
	return new Sign(algorithm, options);
}

export function createVerify(algorithm: unknown, options?: unknown): Verify {
	return new Verify(algorithm, options);
}

function oneShotAlgorithm(algorithm: unknown): string | null {
	if (algorithm === null || algorithm === undefined) return null;
	validateString(algorithm, "algorithm");
	checkDigest(algorithm);
	return algorithm;
}

export function sign(algorithm: unknown, data: unknown, key: unknown, callback?: unknown): Buffer | undefined {
	const alg = oneShotAlgorithm(algorithm);
	if (callback !== undefined) validateFunction(callback, "callback");
	const bytes = toBytes(data, "data");
	if (!key) throw codedError("ERR_CRYPTO_SIGN_KEY_REQUIRED", "No key provided to sign", Error);
	const pkey: PKey = preparePrivateKey(key);
	const o = signOptions(key);
	if (callback === undefined) return native.sign(pkey, bytes, alg, o.padding, o.saltLength, o.dsaEncoding, o.context);
	const cb = callback as (err: Error | null, sig?: Buffer) => void;
	native.sign(pkey, bytes, alg, o.padding, o.saltLength, o.dsaEncoding, o.context, (err, sig) => {
		if (err) cb(err);
		else cb(null, sig);
	});
	return undefined;
}

export function verify(
	algorithm: unknown,
	data: unknown,
	key: unknown,
	signature: unknown,
	callback?: unknown,
): boolean | undefined {
	const alg = oneShotAlgorithm(algorithm);
	if (callback !== undefined) validateFunction(callback, "callback");
	const bytes = toBytes(data, "data");
	const pkey = preparePublicOrPrivateKey(key);
	const o = signOptions(key);
	const sig = toBytes(signature, "signature");
	if (callback === undefined)
		return native.verify(pkey, bytes, sig, alg, o.padding, o.saltLength, o.dsaEncoding, o.context);
	const cb = callback as (err: Error | null, ok?: boolean) => void;
	native.verify(pkey, bytes, sig, alg, o.padding, o.saltLength, o.dsaEncoding, o.context, (err, ok) => {
		if (err) cb(err);
		else cb(null, ok);
	});
	return undefined;
}

export function encapsulate(key: unknown, callback?: unknown): { sharedKey: Buffer; ciphertext: Buffer } | undefined {
	if (callback !== undefined) validateFunction(callback, "callback");
	const pkey = preparePublicOrPrivateKey(key);
	if (callback === undefined) return native.kemEncapsulate(pkey);
	const cb = callback as (err: Error | null, result?: { sharedKey: Buffer; ciphertext: Buffer }) => void;
	native.kemEncapsulate(pkey, (err, result) => {
		if (err) cb(err);
		else cb(null, result);
	});
	return undefined;
}

export function decapsulate(key: unknown, ciphertext: unknown, callback?: unknown): Buffer | undefined {
	if (callback !== undefined) validateFunction(callback, "callback");
	const pkey = preparePrivateKey(key);
	const ct = toBytes(ciphertext, "ciphertext");
	if (callback === undefined) return native.kemDecapsulate(pkey, ct);
	const cb = callback as (err: Error | null, result?: Buffer) => void;
	native.kemDecapsulate(pkey, ct, (err, result) => {
		if (err) cb(err);
		else cb(null, result);
	});
	return undefined;
}
