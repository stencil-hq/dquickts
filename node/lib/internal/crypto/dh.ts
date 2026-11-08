// node:crypto Diffie-Hellman (port of Node's lib/internal/crypto/diffiehellman.js): DiffieHellman,
// DiffieHellmanGroup, ECDH, createDiffieHellman, getDiffieHellman, createECDH and diffieHellman().
import { native, type PKey } from "internal:crypto/native";
import {
	encodeOutput,
	errKeyObjectType,
	errType,
	errValue,
	isAnyArrayBuffer,
	isArrayBufferView,
	normalizeEncoding,
	toBytes,
	validateFunction,
	validateInt32,
	validateObject,
	validateString,
} from "internal:crypto/util";
import { KeyObject, kHandle } from "internal:crypto/keys";
import { codedError } from "internal:errors";

const kPrime = Symbol("kPrime");
const kGenerator = Symbol("kGenerator");
const kPub = Symbol("kPub");
const kPriv = Symbol("kPriv");
const kCurve = Symbol("kCurve");

function asBuf(b: ArrayBuffer | SharedArrayBuffer | ArrayBufferView): Buffer {
	if (isArrayBufferView(b)) return Buffer.from(b.buffer, b.byteOffset, b.byteLength);
	return Buffer.from(b);
}

function intToBuffer(n: number): Buffer {
	const out: number[] = [];
	let v = n >>> 0;
	while (v > 0) {
		out.unshift(v & 0xff);
		v = Math.floor(v / 256);
	}
	return Buffer.from(out.length ? out : [0]);
}

function isEncodingLike(enc: unknown): boolean {
	return enc === "buffer" || (typeof enc === "string" && normalizeEncoding(enc) !== undefined);
}

const kSizeOrKeyTypes = "of type number or string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView";

class DiffieHellmanBase {
	declare [kPrime]: Buffer;
	declare [kGenerator]: Buffer;
	declare [kPub]: Buffer | undefined;
	declare [kPriv]: Buffer | undefined;
	declare verifyError: number;

	protected init(prime: Buffer, generator: Buffer): void {
		this[kPrime] = prime;
		this[kGenerator] = generator;
		this[kPub] = undefined;
		this[kPriv] = undefined;
		Object.defineProperty(this, "verifyError", {
			enumerable: true,
			value: native.dhCheck(prime, generator),
			writable: true,
		});
	}

	generateKeys(encoding?: unknown): Buffer | string {
		const { priv, pub } = native.dhGenerateKey(this[kPrime], this[kGenerator], this[kPriv] ?? null);
		this[kPriv] = priv;
		this[kPub] = pub;
		return encodeOutput(pub, encoding);
	}

	computeSecret(key: unknown, inputEncoding?: unknown, outputEncoding?: unknown): Buffer | string {
		const peer = toBytes(key, "key", inputEncoding);
		if (!this[kPriv]) throw codedError("ERR_CRYPTO_INVALID_STATE", "Invalid state for operation computeSecret", Error);
		return encodeOutput(native.dhCompute(this[kPrime], this[kPriv], peer), outputEncoding);
	}

	getPrime(encoding?: unknown): Buffer | string {
		return encodeOutput(Buffer.from(this[kPrime]), encoding);
	}

	getGenerator(encoding?: unknown): Buffer | string {
		return encodeOutput(Buffer.from(this[kGenerator]), encoding);
	}

	getPublicKey(encoding?: unknown): Buffer | string {
		if (!this[kPub])
			throw codedError("ERR_CRYPTO_INVALID_STATE", "No public key - did you forget to generate one?", Error);
		return encodeOutput(Buffer.from(this[kPub]), encoding);
	}

	getPrivateKey(encoding?: unknown): Buffer | string {
		if (!this[kPriv])
			throw codedError("ERR_CRYPTO_INVALID_STATE", "No private key - did you forget to generate one?", Error);
		return encodeOutput(Buffer.from(this[kPriv]), encoding);
	}
}

export class DiffieHellman extends DiffieHellmanBase {
	constructor(sizeOrKey: unknown, keyEncoding?: unknown, generator?: unknown, genEncoding?: unknown) {
		super();
		if (
			typeof sizeOrKey !== "number" &&
			typeof sizeOrKey !== "string" &&
			!isArrayBufferView(sizeOrKey) &&
			!isAnyArrayBuffer(sizeOrKey)
		) {
			throw errType("sizeOrKey", kSizeOrKeyTypes, sizeOrKey);
		}
		if (typeof sizeOrKey === "number") validateInt32(sizeOrKey, "sizeOrKey");
		if (keyEncoding && !isEncodingLike(keyEncoding)) {
			genEncoding = generator;
			generator = keyEncoding;
			keyEncoding = false;
		}
		let gen: Buffer;
		let genNumber = 2;
		if (!generator) {
			gen = intToBuffer(2);
		} else if (typeof generator === "number") {
			validateInt32(generator, "generator");
			genNumber = generator;
			gen = intToBuffer(generator);
		} else if (typeof generator === "string") {
			gen = asBuf(toBytes(generator, "generator", genEncoding || "buffer"));
			genNumber = gen.length <= 4 && gen.length > 0 ? gen.readUIntBE(0, gen.length) : 2;
		} else if (isArrayBufferView(generator) || isAnyArrayBuffer(generator)) {
			gen = Buffer.from(asBuf(generator));
			genNumber = gen.length <= 4 && gen.length > 0 ? gen.readUIntBE(0, gen.length) : 2;
		} else {
			throw errType("generator", kSizeOrKeyTypes, generator);
		}
		let prime: Buffer;
		if (typeof sizeOrKey === "number") {
			prime = native.dhGeneratePrime(sizeOrKey, genNumber);
		} else {
			prime = Buffer.from(asBuf(toBytes(sizeOrKey, "sizeOrKey", keyEncoding || "buffer")));
		}
		this.init(prime, gen);
	}

	setPublicKey(key: unknown, encoding?: unknown): this {
		this[kPub] = Buffer.from(asBuf(toBytes(key, "key", encoding)));
		return this;
	}

	setPrivateKey(key: unknown, encoding?: unknown): this {
		this[kPriv] = Buffer.from(asBuf(toBytes(key, "key", encoding)));
		return this;
	}
}

export class DiffieHellmanGroup extends DiffieHellmanBase {
	constructor(name: unknown) {
		super();
		validateString(name, "name");
		const group = native.dhGroup(name);
		if (!group) throw codedError("ERR_CRYPTO_UNKNOWN_DH_GROUP", "Unknown DH group", Error);
		this.init(group.prime, group.generator);
	}
}

export function createDiffieHellman(
	sizeOrKey: unknown,
	keyEncoding?: unknown,
	generator?: unknown,
	genEncoding?: unknown,
): DiffieHellman {
	return new DiffieHellman(sizeOrKey, keyEncoding, generator, genEncoding);
}

export function getDiffieHellman(name: unknown): DiffieHellmanGroup {
	return new DiffieHellmanGroup(name);
}

export const createDiffieHellmanGroup = getDiffieHellman;

// ------------------------------------------------------------------- ECDH

function pointFormat(format: unknown): string {
	if (format === undefined || format === null || format === "uncompressed") return "uncompressed";
	if (format === "compressed" || format === "hybrid") return format;
	throw codedError("ERR_CRYPTO_ECDH_INVALID_FORMAT", `Invalid ECDH format: ${String(format)}`, TypeError);
}

export class ECDH {
	declare [kCurve]: string;
	declare [kPub]: Buffer | undefined;
	declare [kPriv]: Buffer | undefined;

	constructor(curve: unknown) {
		validateString(curve, "curve");
		try {
			native.ecdhConvert(curve, Buffer.alloc(0), "uncompressed");
		} catch (e) {
			// only the curve name matters here: an invalid curve throws ERR_CRYPTO_INVALID_CURVE
			if (e instanceof Error && "code" in e && e.code === "ERR_CRYPTO_INVALID_CURVE") throw e;
		}
		this[kCurve] = curve;
		this[kPub] = undefined;
		this[kPriv] = undefined;
	}

	static convertKey(
		key: unknown,
		curve: unknown,
		inEnc?: unknown,
		outEnc?: unknown,
		format?: unknown,
	): Buffer | string {
		validateString(curve, "curve");
		const k = toBytes(key, "key", inEnc);
		return encodeOutput(native.ecdhConvert(curve, k, pointFormat(format)), outEnc);
	}

	generateKeys(encoding?: unknown, format?: unknown): Buffer | string {
		const [priv, pub] = native.ecdhGenerate(this[kCurve]);
		this[kPriv] = priv;
		this[kPub] = pub;
		return this.getPublicKey(encoding, format);
	}

	computeSecret(key: unknown, inputEncoding?: unknown, outputEncoding?: unknown): Buffer | string {
		const peer = toBytes(key, "key", inputEncoding);
		if (!this[kPriv]) throw codedError("ERR_CRYPTO_INVALID_STATE", "Invalid state for operation computeSecret", Error);
		return encodeOutput(native.ecdhCompute(this[kCurve], this[kPriv], peer), outputEncoding);
	}

	getPublicKey(encoding?: unknown, format?: unknown): Buffer | string {
		const f = pointFormat(format);
		if (!this[kPub]) throw codedError("ERR_CRYPTO_OPERATION_FAILED", "Failed to get ECDH public key", Error);
		const pub = f === "uncompressed" ? Buffer.from(this[kPub]) : native.ecdhConvert(this[kCurve], this[kPub], f);
		return encodeOutput(pub, encoding);
	}

	getPrivateKey(encoding?: unknown): Buffer | string {
		if (!this[kPriv]) throw codedError("ERR_CRYPTO_OPERATION_FAILED", "Failed to get ECDH private key", Error);
		return encodeOutput(Buffer.from(this[kPriv]), encoding);
	}

	setPrivateKey(key: unknown, encoding?: unknown): this {
		const priv = Buffer.from(asBuf(toBytes(key, "key", encoding)));
		this[kPub] = native.ecdhPublic(this[kCurve], priv);
		this[kPriv] = priv;
		return this;
	}

	setPublicKey(key: unknown, encoding?: unknown): this {
		const pub = toBytes(key, "key", encoding);
		this[kPub] = native.ecdhConvert(this[kCurve], pub, "uncompressed");
		return this;
	}
}

export function createECDH(curve: unknown): ECDH {
	return new ECDH(curve);
}

// ------------------------------------------------------- diffieHellman()

const kDhTypes: Record<string, true> = { dh: true, ec: true, x448: true, x25519: true };

export function diffieHellman(options: unknown, callback?: unknown): Buffer | undefined {
	validateObject(options, "options");
	if (callback !== undefined) validateFunction(callback, "callback");
	const { privateKey, publicKey } = options;
	if (!(privateKey instanceof KeyObject)) throw errValue("options.privateKey", privateKey);
	if (!(publicKey instanceof KeyObject)) throw errValue("options.publicKey", publicKey);
	if (privateKey.type !== "private") throw errKeyObjectType(privateKey.type, "private");
	if (publicKey.type !== "public" && publicKey.type !== "private")
		throw errKeyObjectType(publicKey.type, "private or public");
	const priv = privateKey[kHandle] as PKey;
	const pub = publicKey[kHandle] as PKey;
	const privateType = priv.asymmetricKeyType;
	const publicType = pub.asymmetricKeyType;
	if (privateType !== publicType || !kDhTypes[privateType]) {
		throw codedError(
			"ERR_CRYPTO_INCOMPATIBLE_KEY",
			`Incompatible key types for Diffie-Hellman: ${privateType} and ${publicType}`,
			Error,
		);
	}
	if (callback === undefined) return native.pkeyDerive(priv, pub);
	const cb = callback as (err: Error | null, secret?: Buffer) => void;
	native.pkeyDerive(priv, pub, (err, secret) => {
		if (err) cb(err);
		else cb(null, secret);
	});
	return undefined;
}
