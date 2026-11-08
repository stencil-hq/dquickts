// node:crypto key objects (port of Node's lib/internal/crypto/keys.js and keygen.js): KeyObject,
// SecretKeyObject, PublicKeyObject, PrivateKeyObject, createSecretKey/PublicKey/PrivateKey, JWK import and
// export, generateKeyPair(Sync) and generateKey(Sync).
import { native, type PKey, type Bytes } from "internal:crypto/native";
import {
	base64url,
	copyBytes,
	errIncompatible,
	errKeyObjectType,
	errMissingOption,
	errType,
	errValue,
	isAnyArrayBuffer,
	isArrayBufferView,
	isStringOrBuffer,
	toBytes,
	validateFunction,
	validateInt32,
	validateInteger,
	validateObject,
	validateOneOf,
	validateString,
	validateUint32,
	type Callback,
} from "internal:crypto/util";
import { codedError } from "internal:errors";
import { getCryptoKeyObject, isCryptoKey, keyObjectToCryptoKey, type CryptoKey } from "internal:webcrypto";

export const kHandle: unique symbol = Symbol("kHandle");
const kType = Symbol("kKeyType");
const kDetails = Symbol("kAsymmetricKeyDetails");

type KeyType = "secret" | "public" | "private";

export interface JwkKey {
	kty?: string;
	[key: string]: unknown;
}

function isPKey(v: unknown): v is PKey {
	return v !== null && typeof v === "object" && "asymmetricKeyType" in v && "details" in v;
}

// ------------------------------------------------------------ key encodings

type KeyFormat = "pem" | "der" | "jwk";

interface KeyEncoding {
	format: KeyFormat;
	type: string | undefined;
	cipher?: string;
	passphrase?: Bytes;
}

function option(name: string, objName?: string): string {
	return objName === undefined ? `options.${name}` : `options.${objName}.${name}`;
}

function parseKeyFormat(formatStr: unknown, defaultFormat: KeyFormat | undefined, optionName: string): KeyFormat {
	if (formatStr === undefined && defaultFormat !== undefined) return defaultFormat;
	if (formatStr === "pem" || formatStr === "der" || formatStr === "jwk") return formatStr;
	throw errValue(optionName, formatStr);
}

function parseKeyType(
	typeStr: unknown,
	required: boolean,
	keyType: string | undefined,
	isPublic: boolean | undefined,
	optionName: string,
): string | undefined {
	if (typeStr === undefined && !required) return undefined;
	if (typeStr === "pkcs1") {
		if (keyType !== undefined && keyType !== "rsa") throw errIncompatible(typeStr, "can only be used for RSA keys");
		return typeStr;
	}
	if (typeStr === "spki" && isPublic !== false) return typeStr;
	if (typeStr === "pkcs8" && isPublic !== true) return typeStr;
	if (typeStr === "sec1" && isPublic !== true) {
		if (keyType !== undefined && keyType !== "ec") throw errIncompatible(typeStr, "can only be used for EC keys");
		return typeStr;
	}
	throw errValue(optionName, typeStr);
}

/** Node's parseKeyEncoding(): keyType undefined = input (parsing), defined = output (export). */
export function parseKeyEncoding(
	enc: Record<string, unknown>,
	keyType: string | undefined,
	isPublic: boolean | undefined,
	objName?: string,
): KeyEncoding {
	validateObject(enc, "options");
	const isInput = keyType === undefined;
	const format = parseKeyFormat(enc.format, isInput ? "pem" : undefined, option("format", objName));
	const isRequired = (!isInput || format === "der") && format !== "jwk";
	const type = parseKeyType(enc.type, isRequired, keyType, isPublic, option("type", objName));
	let cipher: unknown;
	let passphrase: unknown;
	let encoding: unknown;
	if (isPublic !== true) {
		({ cipher, passphrase, encoding } = enc);
		if (!isInput) {
			if (cipher != null) {
				if (typeof cipher !== "string") throw errValue(option("cipher", objName), cipher);
				if (format === "der" && (type === "pkcs1" || type === "sec1"))
					throw errIncompatible(type, "does not support encryption");
			} else if (passphrase !== undefined) {
				throw errValue(option("cipher", objName), cipher);
			}
		}
		if (
			(isInput && passphrase !== undefined && !isStringOrBuffer(passphrase)) ||
			(!isInput && cipher != null && !isStringOrBuffer(passphrase))
		) {
			throw errValue(option("passphrase", objName), passphrase);
		}
	}
	const result: KeyEncoding = { format, type };
	if (cipher != null) result.cipher = cipher as string;
	if (passphrase !== undefined) result.passphrase = toBytes(passphrase, "key.passphrase", encoding);
	return result;
}

// ---------------------------------------------------------------- KeyObject

export class KeyObject {
	declare [kHandle]: Buffer | PKey;
	declare [kType]: KeyType;

	constructor(type: unknown, handle: unknown) {
		validateOneOf(type, "type", ["secret", "public", "private"]);
		if (type === "secret" ? !(handle instanceof Uint8Array) : !isPKey(handle))
			throw errType("handle", "of type object", handle);
		Object.defineProperty(this, kHandle, { value: handle, writable: false, enumerable: false, configurable: false });
		Object.defineProperty(this, kType, { value: type, writable: false, enumerable: false, configurable: false });
	}

	get type(): KeyType {
		return this[kType];
	}

	static from(key: unknown): KeyObject {
		if (!isCryptoKey(key)) throw errType("key", "an instance of CryptoKey", key);
		return getCryptoKeyObject(key);
	}

	equals(otherKeyObject: unknown): boolean {
		if (!isKeyObject(otherKeyObject)) throw errType("otherKeyObject", "an instance of KeyObject", otherKeyObject);
		if (otherKeyObject.type !== this.type) return false;
		const a = this[kHandle];
		const b = otherKeyObject[kHandle];
		if (this.type === "secret") {
			const x = a as Buffer;
			const y = b as Buffer;
			return x.byteLength === y.byteLength && native.timingSafeEqual(x, y);
		}
		return (a as PKey).equals(b as PKey);
	}

	toCryptoKey(algorithm: unknown, extractable: unknown, keyUsages: unknown): CryptoKey {
		return keyObjectToCryptoKey(this, algorithm, extractable, keyUsages);
	}

	get [Symbol.toStringTag](): string {
		return "KeyObject";
	}
}

export class SecretKeyObject extends KeyObject {
	constructor(handle: Buffer) {
		super("secret", handle);
	}

	get symmetricKeySize(): number {
		return (this[kHandle] as Buffer).byteLength;
	}

	export(options?: unknown): Buffer | { kty: string; k: string } {
		const handle = this[kHandle] as Buffer;
		if (options !== undefined) {
			validateObject(options, "options");
			const { format } = options;
			if (format === "jwk") return { kty: "oct", k: base64url(handle) };
			if (format !== undefined && format !== "buffer")
				throw errValue("options.format", format, "must be one of: undefined, 'buffer', 'jwk'");
		}
		return Buffer.from(handle);
	}
}

function exportJwk(pkey: PKey): Record<string, string> {
	const parts = pkey.jwk();
	const out: Record<string, string> = {};
	for (const key in parts) {
		const v = parts[key];
		out[key] = typeof v === "string" ? v : base64url(v);
	}
	return out;
}

export class AsymmetricKeyObject extends KeyObject {
	declare [kDetails]: Record<string, unknown> | undefined;

	get asymmetricKeyType(): string {
		return (this[kHandle] as PKey).asymmetricKeyType;
	}

	get asymmetricKeyDetails(): Record<string, unknown> {
		const type = this.asymmetricKeyType;
		if (type !== "rsa" && type !== "rsa-pss" && type !== "dsa" && type !== "ec") return {};
		if (this[kDetails] === undefined) {
			Object.defineProperty(this, kDetails, {
				value: (this[kHandle] as PKey).details(),
				writable: true,
				configurable: true,
				enumerable: false,
			});
		}
		return { ...(this[kDetails] as Record<string, unknown>) };
	}

	protected exportAsym(options: unknown, isPublic: boolean): string | Buffer | Record<string, string> {
		const pkey = this[kHandle] as PKey;
		if (options !== null && typeof options === "object" && (options as Record<string, unknown>).format === "jwk") {
			return exportJwk(pkey);
		}
		validateObject(options, "options");
		const { format, type, cipher, passphrase } = parseKeyEncoding(options, this.asymmetricKeyType, isPublic);
		if (format === "jwk") return exportJwk(pkey);
		return pkey.export(format, type as string, cipher ?? null, passphrase ?? null);
	}
}

export class PublicKeyObject extends AsymmetricKeyObject {
	constructor(handle: PKey) {
		super("public", handle);
	}

	export(options?: unknown): string | Buffer | Record<string, string> {
		return this.exportAsym(options, true);
	}
}

export class PrivateKeyObject extends AsymmetricKeyObject {
	constructor(handle: PKey) {
		super("private", handle);
	}

	export(options?: unknown): string | Buffer | Record<string, string> {
		return this.exportAsym(options, false);
	}
}

export function isKeyObject(v: unknown): v is KeyObject {
	return v instanceof KeyObject;
}

export function keyObjectFromHandle(type: "public" | "private", handle: PKey): KeyObject {
	return type === "public" ? new PublicKeyObject(handle) : new PrivateKeyObject(handle);
}

// ------------------------------------------------------------ key inputs

export const kConsumePublic = 0;
export const kConsumePrivate = 1;
export const kCreatePublic = 2;
export const kCreatePrivate = 3;

const kKeyTypes = "of type string or an instance of ArrayBuffer, Buffer, TypedArray, DataView, KeyObject, or CryptoKey";

/** The key material of `key` as a secret (Buffer view). */
export function prepareSecretKey(
	key: unknown,
	encoding?: unknown,
	bufferOnly = false,
): ArrayBuffer | SharedArrayBuffer | ArrayBufferView {
	if (!bufferOnly) {
		if (isKeyObject(key)) {
			if (key.type !== "secret") throw errKeyObjectType(key.type, "secret");
			return key[kHandle] as Buffer;
		}
		if (isCryptoKey(key)) {
			const ko = getCryptoKeyObject(key);
			if (ko.type !== "secret") throw errKeyObjectType(ko.type, "secret");
			return ko[kHandle] as Buffer;
		}
	}
	if (typeof key !== "string" && !isArrayBufferView(key) && !isAnyArrayBuffer(key)) {
		throw errType(
			"key",
			bufferOnly ? "of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView" : kKeyTypes,
			key,
		);
	}
	return toBytes(key, "key", encoding);
}

function keyObjectHandle(key: KeyObject, ctx: number): PKey {
	if (ctx === kCreatePrivate)
		throw errType("key", "of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView", key);
	if (key.type !== "private") {
		if (ctx === kConsumePrivate || ctx === kCreatePublic) throw errKeyObjectType(key.type, "private");
		if (key.type !== "public") throw errKeyObjectType(key.type, "private or public");
	}
	return key[kHandle] as PKey;
}

const kOkpLengths: Record<string, number> = { Ed25519: 32, X25519: 32, Ed448: 57, X448: 56 };

function decodeB64(value: string): Buffer {
	return Buffer.from(value, "base64url");
}

function invalidJwk(): Error {
	return codedError("ERR_CRYPTO_INVALID_JWK", "Invalid JWK data", TypeError);
}

/** A PKey from a JWK object (Node's getKeyObjectHandleFromJwk). */
export function pkeyFromJwk(key: unknown, ctx: number): PKey {
	validateObject(key, "key");
	const jwk = key as JwkKey;
	validateOneOf(jwk.kty, "key.kty", ["RSA", "EC", "OKP"]);
	const isPublic = ctx === kConsumePublic || ctx === kCreatePublic;
	if (jwk.kty === "OKP") {
		validateString(jwk.crv, "key.crv");
		validateOneOf(jwk.crv, "key.crv", ["Ed25519", "Ed448", "X25519", "X448"]);
		validateString(jwk.x, "key.x");
		if (!isPublic) validateString(jwk.d, "key.d");
		const data = decodeB64(isPublic ? jwk.x : (jwk.d as string));
		if (data.byteLength !== kOkpLengths[jwk.crv]) throw invalidJwk();
		try {
			return native.keyFromRaw(jwk.crv.toLowerCase(), data, !isPublic);
		} catch {
			throw invalidJwk();
		}
	}
	if (jwk.kty === "EC") {
		validateString(jwk.crv, "key.crv");
		validateOneOf(jwk.crv, "key.crv", ["P-256", "secp256k1", "P-384", "P-521"]);
		validateString(jwk.x, "key.x");
		validateString(jwk.y, "key.y");
		const parts: Record<string, Bytes | string> = { crv: jwk.crv, x: decodeB64(jwk.x), y: decodeB64(jwk.y) };
		if (!isPublic) {
			validateString(jwk.d, "key.d");
			parts.d = decodeB64(jwk.d);
		}
		return native.keyFromJwk("EC", parts, !isPublic);
	}
	validateString(jwk.n, "key.n");
	validateString(jwk.e, "key.e");
	const parts: Record<string, Bytes | string> = { n: decodeB64(jwk.n), e: decodeB64(jwk.e) };
	if (!isPublic) {
		for (const name of ["d", "p", "q", "dp", "dq", "qi"]) {
			validateString(jwk[name], `key.${name}`);
			parts[name] = decodeB64(jwk[name] as string);
		}
	}
	return native.keyFromJwk("RSA", parts, !isPublic);
}

function parseKind(ctx: number): "public" | "private" | "any" {
	if (ctx === kCreatePublic) return "public";
	if (ctx === kConsumePrivate || ctx === kCreatePrivate) return "private";
	return "any";
}

/** Node's prepareAsymmetricKey() + parsing: the PKey designated by `key` for `ctx`. */
export function prepareAsymmetricKey(key: unknown, ctx: number): PKey {
	if (isKeyObject(key)) return keyObjectHandle(key, ctx);
	if (isCryptoKey(key)) return keyObjectHandle(getCryptoKeyObject(key), ctx);
	if (isStringOrBuffer(key)) {
		return native.keyParse(toBytes(key, "key"), "pem", parseKind(ctx), null, null);
	}
	if (key !== null && typeof key === "object") {
		const opts = key as Record<string, unknown>;
		const { key: data, encoding, format } = opts;
		if (isKeyObject(data)) return keyObjectHandle(data, ctx);
		if (isCryptoKey(data)) return keyObjectHandle(getCryptoKeyObject(data), ctx);
		if (format === "jwk") {
			validateObject(data, "key.key");
			return pkeyFromJwk(data, ctx);
		}
		if (!isStringOrBuffer(data)) {
			throw errType("key.key", "of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView", data);
		}
		const isPublic = ctx === kConsumePrivate || ctx === kCreatePrivate ? false : undefined;
		const enc = parseKeyEncoding(opts, undefined, isPublic);
		const bytes = toBytes(data, "key", encoding);
		return native.keyParse(
			bytes,
			enc.format as "pem" | "der",
			parseKind(ctx),
			enc.type ?? null,
			enc.passphrase ?? null,
		);
	}
	throw errType("key", kKeyTypes, key);
}

export function preparePrivateKey(key: unknown): PKey {
	return prepareAsymmetricKey(key, kConsumePrivate);
}

export function preparePublicOrPrivateKey(key: unknown): PKey {
	return prepareAsymmetricKey(key, kConsumePublic);
}

export function createSecretKey(key: unknown, encoding?: unknown): SecretKeyObject {
	const bytes = prepareSecretKey(key, encoding, true);
	return new SecretKeyObject(copyBytes(bytes));
}

export function createPublicKey(key: unknown): PublicKeyObject {
	const pkey = prepareAsymmetricKey(key, kCreatePublic);
	return new PublicKeyObject(pkey.type === "private" ? pkey.toPublic() : pkey);
}

export function createPrivateKey(key: unknown): PrivateKeyObject {
	return new PrivateKeyObject(prepareAsymmetricKey(key, kCreatePrivate));
}

// ------------------------------------------------------------ key generation

function incompatiblePair(a: string, b: string): Error {
	return codedError(
		"ERR_INCOMPATIBLE_OPTION_PAIR",
		`Option "${a}" cannot be used in combination with option "${b}"`,
		TypeError,
	);
}

interface KeyPairEncodings {
	publicEncoding?: KeyEncoding;
	privateEncoding?: KeyEncoding;
}

function parseKeyPairEncodings(keyType: string, options: Record<string, unknown> | undefined): KeyPairEncodings {
	const out: KeyPairEncodings = {};
	if (!options) return out;
	const { publicKeyEncoding, privateKeyEncoding } = options;
	if (publicKeyEncoding != null) {
		if (typeof publicKeyEncoding !== "object") throw errValue("options.publicKeyEncoding", publicKeyEncoding);
		out.publicEncoding = parseKeyEncoding(
			publicKeyEncoding as Record<string, unknown>,
			keyType,
			keyType === undefined ? undefined : true,
			"publicKeyEncoding",
		);
	}
	if (privateKeyEncoding != null) {
		if (typeof privateKeyEncoding !== "object") throw errValue("options.privateKeyEncoding", privateKeyEncoding);
		out.privateEncoding = parseKeyEncoding(
			privateKeyEncoding as Record<string, unknown>,
			keyType,
			false,
			"privateKeyEncoding",
		);
	}
	return out;
}

function parseKeyGenOptions(
	type: unknown,
	options: unknown,
): { nativeType: string; opts: Record<string, unknown>; encodings: KeyPairEncodings } {
	validateString(type, "type");
	if (options !== undefined) validateObject(options, "options");
	const o = (options ?? {}) as Record<string, unknown>;
	const opts: Record<string, unknown> = {};
	switch (type) {
		case "rsa":
		case "rsa-pss": {
			validateObject(options, "options");
			validateUint32(o.modulusLength, "options.modulusLength");
			opts.modulusLength = o.modulusLength;
			if (o.publicExponent == null) opts.publicExponent = 0x10001;
			else {
				validateUint32(o.publicExponent, "options.publicExponent");
				opts.publicExponent = o.publicExponent;
			}
			if (type === "rsa-pss") {
				const { hash, mgf1Hash, hashAlgorithm, mgf1HashAlgorithm, saltLength } = o;
				if (saltLength !== undefined) validateInt32(saltLength, "options.saltLength", 0);
				if (hashAlgorithm !== undefined) validateString(hashAlgorithm, "options.hashAlgorithm");
				if (mgf1HashAlgorithm !== undefined) validateString(mgf1HashAlgorithm, "options.mgf1HashAlgorithm");
				if (hash !== undefined) {
					validateString(hash, "options.hash");
					if (hashAlgorithm && hash !== hashAlgorithm) throw errValue("options.hash", hash);
				}
				if (mgf1Hash !== undefined) {
					validateString(mgf1Hash, "options.mgf1Hash");
					if (mgf1HashAlgorithm && mgf1Hash !== mgf1HashAlgorithm) throw errValue("options.mgf1Hash", mgf1Hash);
				}
				const h = (hashAlgorithm ?? hash) as string | undefined;
				const m = (mgf1HashAlgorithm ?? mgf1Hash) as string | undefined;
				if (h !== undefined && !native.digestInfo(h))
					throw codedError("ERR_CRYPTO_INVALID_DIGEST", `Invalid digest: ${h}`, TypeError);
				if (m !== undefined && !native.digestInfo(m))
					throw codedError("ERR_CRYPTO_INVALID_DIGEST", `Invalid MGF1 digest: ${m}`, TypeError);
				if (h !== undefined) opts.hashAlgorithm = h;
				if (m !== undefined) opts.mgf1HashAlgorithm = m;
				if (saltLength !== undefined) opts.saltLength = saltLength;
			}
			break;
		}
		case "dsa": {
			validateObject(options, "options");
			validateUint32(o.modulusLength, "options.modulusLength");
			opts.modulusLength = o.modulusLength;
			if (o.divisorLength != null) {
				validateInt32(o.divisorLength, "options.divisorLength", 0);
				opts.divisorLength = o.divisorLength;
			}
			break;
		}
		case "ec": {
			validateObject(options, "options");
			validateString(o.namedCurve, "options.namedCurve");
			opts.namedCurve = o.namedCurve;
			if (o.paramEncoding == null || o.paramEncoding === "named") opts.paramEncoding = "named";
			else if (o.paramEncoding === "explicit") opts.paramEncoding = "explicit";
			else throw errValue("options.paramEncoding", o.paramEncoding);
			break;
		}
		case "ed25519":
		case "ed448":
		case "x25519":
		case "x448":
			break;
		case "dh": {
			validateObject(options, "options");
			const { group, primeLength, prime, generator } = o;
			if (group != null) {
				if (prime != null) throw incompatiblePair("group", "prime");
				if (primeLength != null) throw incompatiblePair("group", "primeLength");
				if (generator != null) throw incompatiblePair("group", "generator");
				validateString(group, "options.group");
				if (!native.dhGroup(group)) throw codedError("ERR_CRYPTO_UNKNOWN_DH_GROUP", "Unknown DH group", Error);
				opts.group = group;
			} else {
				if (prime != null) {
					if (primeLength != null) throw incompatiblePair("prime", "primeLength");
					if (!isArrayBufferView(prime))
						throw errType("options.prime", "an instance of Buffer, TypedArray, or DataView", prime);
					opts.prime = prime;
				} else if (primeLength != null) {
					validateInt32(primeLength, "options.primeLength", 0);
					opts.primeLength = primeLength;
				} else {
					throw errMissingOption("At least one of the group, prime, or primeLength options");
				}
				if (generator != null) {
					validateInt32(generator, "options.generator", 0);
					opts.generator = generator;
				}
			}
			break;
		}
		default:
			if (!/^(ml-dsa-(44|65|87)|ml-kem-(512|768|1024)|slh-dsa-(sha2|shake)-(128|192|256)[fs])$/.test(type)) {
				throw errValue("type", type, "must be a supported key type");
			}
	}
	return {
		nativeType: type,
		opts,
		encodings: parseKeyPairEncodings(type, options as Record<string, unknown> | undefined),
	};
}

function encodeKey(key: KeyObject, enc: KeyEncoding | undefined): KeyObject | string | Buffer | Record<string, string> {
	if (!enc) return key;
	const asym = key as AsymmetricKeyObject;
	const pkey = asym[kHandle] as PKey;
	if (enc.format === "jwk") return exportJwk(pkey);
	return pkey.export(enc.format, enc.type as string, enc.cipher ?? null, enc.passphrase ?? null);
}

export function generateKeyPairSync(type: unknown, options?: unknown): { publicKey: unknown; privateKey: unknown } {
	const { nativeType, opts, encodings } = parseKeyGenOptions(type, options);
	const [pub, priv] = native.generateKeyPair(nativeType, opts);
	return {
		publicKey: encodeKey(new PublicKeyObject(pub), encodings.publicEncoding),
		privateKey: encodeKey(new PrivateKeyObject(priv), encodings.privateEncoding),
	};
}

export function generateKeyPair(type: unknown, options: unknown, callback?: unknown): void {
	if (typeof options === "function") {
		callback = options;
		options = undefined;
	}
	validateFunction(callback, "callback");
	const cb = callback as (err: Error | null, publicKey?: unknown, privateKey?: unknown) => void;
	const { nativeType, opts, encodings } = parseKeyGenOptions(type, options);
	native.generateKeyPair(nativeType, opts, (err, pair) => {
		if (err) {
			cb(err);
			return;
		}
		let publicKey: unknown;
		let privateKey: unknown;
		try {
			publicKey = encodeKey(new PublicKeyObject(pair[0]), encodings.publicEncoding);
			privateKey = encodeKey(new PrivateKeyObject(pair[1]), encodings.privateEncoding);
		} catch (e) {
			cb(e as Error);
			return;
		}
		cb(null, publicKey, privateKey);
	});
}

function secretKeyLength(type: unknown, options: unknown): number {
	validateString(type, "type");
	validateObject(options, "options");
	const { length } = options;
	switch (type) {
		case "hmac":
			validateInteger(length, "options.length", 8, 2 ** 31 - 1);
			return Math.floor(length / 8);
		case "aes":
			validateOneOf(length, "options.length", [128, 192, 256]);
			return length / 8;
		default:
			throw errValue("type", type, "must be a supported key type");
	}
}

export function generateKeySync(type: unknown, options: unknown): SecretKeyObject {
	const n = secretKeyLength(type, options);
	return new SecretKeyObject(native.randomBytes(n));
}

export function generateKey(type: unknown, options: unknown, callback: unknown): void {
	if (typeof options === "function") {
		callback = options;
		options = undefined;
	}
	validateFunction(callback, "callback");
	const cb = callback as Callback<SecretKeyObject>;
	const n = secretKeyLength(type, options);
	native.randomBytes(n, (err, buf) => {
		if (err) cb(err);
		else cb(null, new SecretKeyObject(buf));
	});
}
