// SubtleCrypto algorithm implementations (port of Node's lib/internal/crypto/{webcrypto,webidl,util,
// webcrypto_util,aes,mac,rsa,ec,cfrg,hkdf,pbkdf2}.js onto the qbun:crypto natives).
// Each SubtleCrypto method receives its `arguments` and returns a value or a Promise
// (webcrypto turns synchronous throws into rejections).
import { Buffer } from "node:buffer";
import * as native from "qbun:crypto";
import { codedError, invalidArgType } from "internal:errors";
import { createSecretKey, keyObjectFromHandle, kHandle } from "internal:crypto/keys";
import type { KeyObject } from "internal:crypto/keys";
import {
	createCryptoKey,
	getCryptoKeyAlgorithm,
	getCryptoKeyExtractable,
	getCryptoKeyObject,
	getCryptoKeyType,
	getCryptoKeyUsages,
	hasCryptoKeyUsage,
	isCryptoKey,
} from "internal:webcrypto";
import type { CryptoKey, KeyAlgorithm } from "internal:webcrypto";

// --- Natives ------------------------------------------------------------------------------------

type Bytes = ArrayBuffer | ArrayBufferView;
type Callback<T> = (err: Error | null, result: T) => void;

interface PKeyDetails {
	modulusLength?: number;
	publicExponent?: bigint | number;
	namedCurve?: string;
}

interface PKey {
	readonly type: "public" | "private";
	readonly asymmetricKeyType: string;
	details(): PKeyDetails;
	export(format: "der", type: "spki" | "pkcs8", cipher: null, passphrase: null): Buffer;
	jwk(): Record<string, Uint8Array | string>;
	rawPublic(): Buffer;
	ecPoint(form: "uncompressed"): Buffer;
	curve(): string | undefined;
	toPublic(): PKey;
}

interface NativeDigest {
	update(data: Bytes): void;
	digest(): Buffer;
}

interface NativeCipher {
	update(data: Bytes): Buffer;
	final(): Buffer;
	setAAD(aad: Bytes, plaintextLength: number): boolean;
	setAuthTag(tag: Bytes): boolean;
	getAuthTag(): Buffer | undefined;
}

interface CryptoNatives {
	hashOnce(name: string, data: Bytes, outputLength: number): Buffer | null;
	hmacNew(digest: string, key: Bytes): NativeDigest | null;
	getHashes(): string[];
	randomBytes(size: number, cb: Callback<Buffer>): void;
	timingSafeEqual(a: Bytes, b: Bytes): boolean;
	cipherNew(name: string, key: Bytes, iv: Bytes | null, encrypt: boolean, authTagLength: number): NativeCipher;
	pbkdf2(password: Bytes, salt: Bytes, iterations: number, keylen: number, digest: string, cb: Callback<Buffer>): void;
	hkdf(digest: string, ikm: Bytes, salt: Bytes, info: Bytes, keylen: number, cb: Callback<Buffer>): void;
	keyParse(data: Bytes, format: "der", kind: "public" | "private", type: "spki" | "pkcs8", passphrase: null): PKey;
	keyFromJwk(kty: string, parts: Record<string, Bytes | string>, isPrivate: boolean): PKey;
	keyFromRaw(type: string, data: Bytes, isPrivate: boolean): PKey;
	keyFromEcPoint(curve: string, point: Bytes): PKey;
	generateKeyPair(type: string, opts: Record<string, unknown>, cb: Callback<[PKey, PKey]>): void;
	sign(
		key: PKey,
		data: Bytes,
		digest: string | null,
		padding: number,
		saltLength: number,
		dsaEncoding: number,
		context: Bytes | null,
		cb: Callback<Buffer>,
	): void;
	verify(
		key: PKey,
		data: Bytes,
		signature: Bytes,
		digest: string | null,
		padding: number,
		saltLength: number,
		dsaEncoding: number,
		context: Bytes | null,
		cb: Callback<boolean>,
	): void;
	pkeyCrypt(
		op: number,
		key: PKey,
		data: Bytes,
		padding: number,
		oaepHash: string | null,
		oaepLabel: Bytes | null,
	): Buffer;
	pkeyDerive(priv: PKey, pub: PKey, cb: Callback<Buffer>): void;
}

const N = native as unknown as CryptoNatives;

const RSA_PKCS1_PADDING = 1;
const RSA_PKCS1_OAEP_PADDING = 4;
const RSA_PKCS1_PSS_PADDING = 6;
const kSaltLengthUnset = -2;
const kSigEncDER = 0;
const kSigEncP1363 = 1;
const kMaxBufferLength = 2 ** 31 - 1;

// --- Errors & jobs ------------------------------------------------------------------------------

function domException(message: string, name: string, cause?: unknown): DOMException {
	const err = new DOMException(message, name);
	if (cause !== undefined) Object.defineProperty(err, "cause", { value: cause, configurable: true, writable: true });
	return err;
}

const kOperationFailed = "The operation failed for an operation-specific reason";

function operationError(cause?: unknown): DOMException {
	return domException(kOperationFailed, "OperationError", cause);
}

function asOperationError(err: unknown): unknown {
	return err instanceof DOMException ? err : operationError(err);
}

/** Run an async (threadpool) native; failures reject with an OperationError. */
function job<T>(start: (cb: Callback<T>) => void): Promise<T> {
	const { promise, resolve, reject } = Promise.withResolvers<T>();
	try {
		start((err, result) => {
			if (err) reject(operationError(err));
			else resolve(result);
		});
	} catch (err) {
		reject(operationError(err));
	}
	return promise;
}

/** Run a synchronous native operation inside a promise; native failures become OperationErrors. */
function syncJob<T>(run: () => T): Promise<T> {
	try {
		return Promise.resolve(run());
	} catch (err) {
		return Promise.reject(asOperationError(err));
	}
}

// --- Bytes --------------------------------------------------------------------------------------

/** A Uint8Array view over a BufferSource (no copy). */
function bytesOf(data: Bytes): Uint8Array {
	return ArrayBuffer.isView(data)
		? new Uint8Array(data.buffer, data.byteOffset, data.byteLength)
		: new Uint8Array(data);
}

/** An ArrayBuffer holding exactly `buf`'s bytes (the backing store itself when it fits exactly). */
function toArrayBuffer(buf: Uint8Array): ArrayBuffer {
	const ab = buf.buffer as ArrayBuffer;
	if (buf.byteOffset === 0 && buf.byteLength === ab.byteLength) return ab;
	return ab.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
}

function concatToArrayBuffer(parts: Uint8Array[]): ArrayBuffer {
	let total = 0;
	for (const part of parts) total += part.byteLength;
	const out = new Uint8Array(total);
	let offset = 0;
	for (const part of parts) {
		out.set(part, offset);
		offset += part.byteLength;
	}
	return out.buffer;
}

function base64UrlEncode(bytes: Uint8Array): string {
	return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString("base64url");
}

function numBitsToBytes(length: number): number {
	return Math.floor(length / 8) + Math.floor((7 + (length % 8)) / 8);
}

/** Copy the bytes needed for `length` bits, clearing unused low bits of the final byte. */
function truncateToBitLength(length: number, bytes: Uint8Array): Uint8Array {
	const lengthBytes = numBitsToBytes(length);
	// Uint8Array#slice copies (Buffer#slice would return a view and the masking below would write through).
	const result = Uint8Array.prototype.slice.call(bytes, 0, Math.min(lengthBytes, bytes.byteLength)) as Uint8Array;
	const remainder = length % 8;
	if (remainder !== 0) result[lengthBytes - 1] &= (0xff << (8 - remainder)) & 0xff;
	return result;
}

function validateMaxBufferLength(data: Bytes, name: string, max = kMaxBufferLength): void {
	if (data.byteLength > max) throw domException(`${name} must be at most ${max} bytes`, "OperationError");
}

function isInt32(value: unknown): boolean {
	return value === ((value as number) | 0);
}

// RSA publicExponent is a WebIDL BigInteger: big-endian bytes with arbitrary leading zeros.
function bigIntArrayToUnsignedInt(input: Uint8Array): number {
	let result = 0;
	const length = input.length;
	for (let n = 0; n < length; ++n) {
		const reversed = length - n - 1;
		if (reversed >= 4 && input[n]) {
			throw domException("algorithm.publicExponent must fit in an unsigned 32-bit integer", "OperationError");
		}
		result |= input[n] << (8 * reversed);
	}
	return result >>> 0;
}

function bigIntToBytes(value: bigint | number): Uint8Array {
	let hex = BigInt(value).toString(16);
	if (hex.length % 2) hex = `0${hex}`;
	const out = new Uint8Array(hex.length / 2);
	for (let i = 0; i < out.length; i++) out[i] = Number.parseInt(hex.slice(i * 2, i * 2 + 2), 16);
	return out;
}

// --- WebIDL -------------------------------------------------------------------------------------

interface ConvOpts {
	prefix?: string;
	context?: string;
	code?: string;
}

type Converter<T = unknown> = (value: unknown, opts: ConvOpts) => T;

function makeException(message: string, opts: ConvOpts): Error {
	const prefix = opts.prefix ? `${opts.prefix}: ` : "";
	const context = opts.context?.length === 0 ? "" : `${opts.context ?? "Value"} `;
	return codedError(opts.code || "ERR_INVALID_ARG_TYPE", `${prefix}${context}${message}`, TypeError);
}

function withContext(opts: ConvOpts, context: string | undefined, code = opts.code): ConvOpts {
	return { prefix: opts.prefix, context, code };
}

function requiredArguments(length: number, required: number, prefix: string): void {
	if (length < required) {
		throw makeException(`${required} argument${required === 1 ? "" : "s"} required, but only ${length} present.`, {
			prefix,
			context: "",
			code: "ERR_MISSING_ARGS",
		});
	}
}

function isObject(value: unknown): value is Record<string, unknown> {
	return (typeof value === "object" && value !== null) || typeof value === "function";
}

const convertDOMString: Converter<string> = (value, opts) => {
	if (typeof value === "symbol") throw makeException("is a Symbol and cannot be converted to a string.", opts);
	return String(value);
};

const convertBoolean: Converter<boolean> = (value) => Boolean(value);

const convertAlgorithmIdentifier: Converter<Record<string, unknown> | string> = (value, opts) =>
	isObject(value) ? value : convertDOMString(value, opts);

// Integer conversion with [EnforceRange] (the only form Web Crypto uses).
function enforceRangeInteger(bits: number): Converter<number> {
	const upper = 2 ** bits - 1;
	return (value, opts) => {
		if (typeof value === "bigint") throw makeException("is a BigInt and cannot be converted to a number.", opts);
		if (typeof value === "symbol") throw makeException("is a Symbol and cannot be converted to a number.", opts);
		const x = +(value as number);
		if (!Number.isFinite(x)) throw makeException("is not a finite number.", opts);
		const integer = Math.trunc(x);
		if (integer < 0 || integer > upper) {
			throw makeException(
				`is outside the expected range of 0 to ${upper}.`,
				withContext(opts, opts.context, "ERR_OUT_OF_RANGE"),
			);
		}
		return integer === 0 ? 0 : integer;
	};
}

const convertOctet = enforceRangeInteger(8);
const convertUnsignedShort = enforceRangeInteger(16);
const convertUnsignedLong = enforceRangeInteger(32);

const abByteLength = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "byteLength")?.get as (
	this: unknown,
) => number;
const abResizable = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "resizable")?.get as
	| ((this: unknown) => boolean)
	| undefined;

/** Brand check for a non-shared ArrayBuffer. */
function isArrayBuffer(value: unknown): value is ArrayBuffer {
	if (value === null || typeof value !== "object") return false;
	try {
		abByteLength.call(value);
		return true;
	} catch {
		return false;
	}
}

function checkBacking(buffer: ArrayBuffer, opts: ConvOpts): void {
	if (abResizable !== undefined && abResizable.call(buffer)) {
		throw makeException("is backed by a resizable ArrayBuffer, which is not allowed.", opts);
	}
}

const convertBufferSource: Converter<Bytes> = (value, opts) => {
	if (ArrayBuffer.isView(value)) {
		if (!isArrayBuffer(value.buffer))
			throw makeException("is a view on a SharedArrayBuffer, which is not allowed.", opts);
		checkBacking(value.buffer, opts);
		return value;
	}
	if (!isArrayBuffer(value))
		throw makeException("is not instance of ArrayBuffer, Buffer, TypedArray, or DataView.", opts);
	checkBacking(value, opts);
	return value;
};

const convertUint8Array: Converter<Uint8Array> = (value, opts) => {
	if (!(value instanceof Uint8Array)) throw makeException("is not an Uint8Array object.", opts);
	if (!isArrayBuffer(value.buffer))
		throw makeException("is a view on a SharedArrayBuffer, which is not allowed.", opts);
	checkBacking(value.buffer, opts);
	return value;
};

const convertCryptoKey: Converter<CryptoKey> = (value, opts) => {
	if (isCryptoKey(value)) return value;
	throw makeException("is not of type CryptoKey.", opts);
};

function createEnumConverter(name: string, values: readonly string[]): Converter<string> {
	return (value, opts) => {
		const s = convertDOMString(value, opts);
		if (!values.includes(s)) {
			throw makeException(
				`'${s}' is not a valid enum value of type ${name}.`,
				withContext(opts, opts.context, "ERR_INVALID_ARG_VALUE"),
			);
		}
		return s;
	};
}

function createSequenceConverter<T>(converter: Converter<T>): Converter<T[]> {
	return (value, opts) => {
		if (!isObject(value)) throw makeException("cannot be converted to sequence.", opts);
		const method = (value as { [Symbol.iterator]?: unknown })[Symbol.iterator];
		if (typeof method !== "function") throw makeException("cannot be converted to sequence.", opts);
		const iterator = (method as () => unknown).call(value);
		if (!isObject(iterator)) throw makeException("cannot be converted to sequence.", opts);
		const next = iterator.next;
		if (typeof next !== "function") throw makeException("cannot be converted to sequence.", opts);
		const result: T[] = [];
		for (;;) {
			const step = (next as () => unknown).call(iterator);
			if (!isObject(step)) throw makeException("cannot be converted to sequence.", opts);
			if (step.done) break;
			result.push(converter(step.value, withContext(opts, `${opts.context ?? "Value"}[${result.length}]`)));
		}
		return result;
	};
}

interface DictionaryMember {
	key: string;
	converter: Converter;
	required?: boolean;
}

type IdlDictionary = Record<string, unknown>;

/** Members are grouped least- to most-derived; each group converts in lexicographic order. */
function createDictionaryConverter(name: string, groups: DictionaryMember[][]): Converter<IdlDictionary> {
	const members: DictionaryMember[] = [];
	for (const group of groups) members.push(...[...group].sort((a, b) => (a.key < b.key ? -1 : a.key > b.key ? 1 : 0)));
	return (value, opts) => {
		if (value != null && !isObject(value)) throw makeException("cannot be converted to a dictionary", opts);
		const result = Object.create(null) as IdlDictionary;
		for (const member of members) {
			const memberValue = value == null ? undefined : (value as IdlDictionary)[member.key];
			if (memberValue !== undefined) {
				const context = opts.context ? `${member.key} in ${opts.context}` : member.key;
				result[member.key] = member.converter(memberValue, withContext(opts, context));
			} else if (member.required) {
				throw makeException(
					`cannot be converted to '${name}' because '${member.key}' is required in '${name}'.`,
					withContext(opts, opts.context, "ERR_MISSING_OPTION"),
				);
			}
		}
		return result;
	};
}

const kKeyFormats = ["raw", "raw-public", "raw-seed", "raw-secret", "raw-private", "pkcs8", "spki", "jwk"] as const;
type KeyFormat = (typeof kKeyFormats)[number];
const convertKeyFormat = createEnumConverter("KeyFormat", kKeyFormats) as Converter<KeyFormat>;

const convertKeyUsage = createEnumConverter("KeyUsage", [
	"encrypt",
	"decrypt",
	"sign",
	"verify",
	"deriveKey",
	"deriveBits",
	"wrapKey",
	"unwrapKey",
	"encapsulateBits",
	"decapsulateBits",
	"encapsulateKey",
	"decapsulateKey",
]);
const convertKeyUsages = createSequenceConverter(convertKeyUsage);

interface JsonWebKey {
	kty?: string;
	use?: string;
	key_ops?: string[];
	alg?: string;
	ext?: boolean;
	crv?: string;
	x?: string;
	y?: string;
	d?: string;
	n?: string;
	e?: string;
	p?: string;
	q?: string;
	dp?: string;
	dq?: string;
	qi?: string;
	k?: string;
	[member: string]: unknown;
}

const stringMember = (key: string): DictionaryMember => ({ key, converter: convertDOMString });

const convertJsonWebKey = createDictionaryConverter("JsonWebKey", [
	[
		stringMember("kty"),
		stringMember("use"),
		{ key: "key_ops", converter: createSequenceConverter(convertDOMString) },
		stringMember("alg"),
		{ key: "ext", converter: convertBoolean },
		stringMember("crv"),
		stringMember("x"),
		stringMember("y"),
		stringMember("d"),
		stringMember("n"),
		stringMember("e"),
		stringMember("p"),
		stringMember("q"),
		stringMember("dp"),
		stringMember("dq"),
		stringMember("qi"),
		{
			key: "oth",
			converter: createSequenceConverter(
				createDictionaryConverter("RsaOtherPrimesInfo", [[stringMember("r"), stringMember("d"), stringMember("t")]]),
			),
		},
		stringMember("k"),
		stringMember("pub"),
		stringMember("priv"),
	],
]) as Converter<JsonWebKey>;

const kArgumentContexts = [
	"1st argument",
	"2nd argument",
	"3rd argument",
	"4th argument",
	"5th argument",
	"6th argument",
	"7th argument",
];

function prepare(method: string, args: ArrayLike<unknown>, required: number): string {
	const prefix = `Failed to execute '${method}' on 'SubtleCrypto'`;
	requiredArguments(args.length, required, prefix);
	return prefix;
}

function convertArg<T>(prefix: string, converter: Converter<T>, value: unknown, index: number): T {
	return converter(value, { prefix, context: kArgumentContexts[index] });
}

// --- Algorithm normalization --------------------------------------------------------------------

type Op =
	| "digest"
	| "generateKey"
	| "importKey"
	| "exportKey"
	| "sign"
	| "verify"
	| "encrypt"
	| "decrypt"
	| "deriveBits"
	| "wrapKey"
	| "unwrapKey"
	| "get key length"
	| "encapsulate"
	| "decapsulate"
	| "get shared key length";

interface Algorithm {
	name: string;
	hash?: Algorithm;
	length?: number;
	iv?: Uint8Array;
	additionalData?: Uint8Array;
	tagLength?: number;
	counter?: Uint8Array;
	label?: Uint8Array;
	saltLength?: number;
	modulusLength?: number;
	publicExponent?: Uint8Array;
	namedCurve?: string;
	public?: CryptoKey;
	salt?: Uint8Array;
	info?: Uint8Array;
	iterations?: number;
	context?: Uint8Array;
	outputLength?: number;
	functionName?: Uint8Array;
	customization?: Uint8Array;
	[member: string]: unknown;
}

const kAlgorithmDefinitions: Record<string, Partial<Record<Op, string | null>>> = {
	"AES-CBC": {
		generateKey: "AesKeyGenParams",
		exportKey: null,
		importKey: null,
		encrypt: "AesCbcParams",
		decrypt: "AesCbcParams",
		"get key length": "AesDerivedKeyParams",
	},
	"AES-CTR": {
		generateKey: "AesKeyGenParams",
		exportKey: null,
		importKey: null,
		encrypt: "AesCtrParams",
		decrypt: "AesCtrParams",
		"get key length": "AesDerivedKeyParams",
	},
	"AES-GCM": {
		generateKey: "AesKeyGenParams",
		exportKey: null,
		importKey: null,
		encrypt: "AeadParams",
		decrypt: "AeadParams",
		"get key length": "AesDerivedKeyParams",
	},
	"AES-KW": {
		generateKey: "AesKeyGenParams",
		exportKey: null,
		importKey: null,
		"get key length": "AesDerivedKeyParams",
		wrapKey: null,
		unwrapKey: null,
	},
	cSHAKE128: { digest: "CShakeParams" },
	cSHAKE256: { digest: "CShakeParams" },
	ECDH: {
		generateKey: "EcKeyGenParams",
		exportKey: null,
		importKey: "EcKeyImportParams",
		deriveBits: "EcdhKeyDeriveParams",
	},
	ECDSA: {
		generateKey: "EcKeyGenParams",
		exportKey: null,
		importKey: "EcKeyImportParams",
		sign: "EcdsaParams",
		verify: "EcdsaParams",
	},
	Ed25519: { generateKey: null, exportKey: null, importKey: null, sign: null, verify: null },
	Ed448: { generateKey: null, exportKey: null, importKey: null, sign: "ContextParams", verify: "ContextParams" },
	HKDF: { importKey: null, deriveBits: "HkdfParams", "get key length": null },
	HMAC: {
		generateKey: "HmacKeyGenParams",
		exportKey: null,
		importKey: "HmacImportParams",
		sign: null,
		verify: null,
		"get key length": "HmacImportParams",
	},
	PBKDF2: { importKey: null, deriveBits: "Pbkdf2Params", "get key length": null },
	"RSA-OAEP": {
		generateKey: "RsaHashedKeyGenParams",
		exportKey: null,
		importKey: "RsaHashedImportParams",
		encrypt: "RsaOaepParams",
		decrypt: "RsaOaepParams",
	},
	"RSA-PSS": {
		generateKey: "RsaHashedKeyGenParams",
		exportKey: null,
		importKey: "RsaHashedImportParams",
		sign: "RsaPssParams",
		verify: "RsaPssParams",
	},
	"RSASSA-PKCS1-v1_5": {
		generateKey: "RsaHashedKeyGenParams",
		exportKey: null,
		importKey: "RsaHashedImportParams",
		sign: null,
		verify: null,
	},
	"SHA-1": { digest: null },
	"SHA-256": { digest: null },
	"SHA-384": { digest: null },
	"SHA-512": { digest: null },
	"SHA3-256": { digest: null },
	"SHA3-384": { digest: null },
	"SHA3-512": { digest: null },
	X25519: { generateKey: null, exportKey: null, importKey: null, deriveBits: "EcdhKeyDeriveParams" },
	X448: { generateKey: null, exportKey: null, importKey: null, deriveBits: "EcdhKeyDeriveParams" },
};

// Algorithms that depend on the linked OpenSSL providing a digest.
const kConditionalDigests: Record<string, string> = {
	"SHA-1": "sha1",
	"SHA-256": "sha256",
	"SHA-384": "sha384",
	"SHA-512": "sha512",
	"SHA3-256": "sha3-256",
	"SHA3-384": "sha3-384",
	"SHA3-512": "sha3-512",
	cSHAKE128: "shake128",
	cSHAKE256: "shake256",
};

interface Registry {
	/** op → algorithm name → dictionary type (null = plain Algorithm). */
	dictionaries: Partial<Record<Op, Record<string, string | null>>>;
	/** op → upper-cased name → canonical name. */
	names: Partial<Record<Op, Map<string, string>>>;
}

let registry: Registry | undefined;
function getRegistry(): Registry {
	if (registry !== undefined) return registry;
	const hashes = N.getHashes().map((name) => name.toLowerCase());
	const dictionaries: Registry["dictionaries"] = {};
	const names: Registry["names"] = {};
	for (const algorithmName in kAlgorithmDefinitions) {
		const digest = kConditionalDigests[algorithmName];
		if (digest !== undefined && !hashes.includes(digest)) continue;
		const operations = kAlgorithmDefinitions[algorithmName];
		for (const key in operations) {
			const op = key as Op;
			const dictionary = operations[op] as string | null;
			(dictionaries[op] ??= Object.create(null) as Record<string, string | null>)[algorithmName] = dictionary;
			(names[op] ??= new Map()).set(algorithmName.toUpperCase(), algorithmName);
		}
	}
	registry = { dictionaries, names };
	return registry;
}

const kNamedCurveAliases: Record<string, string> = {
	"P-256": "prime256v1",
	"P-384": "secp384r1",
	"P-521": "secp521r1",
};
const kCurveToNamedCurve: Record<string, string> = {
	prime256v1: "P-256",
	secp256r1: "P-256",
	"P-256": "P-256",
	secp384r1: "P-384",
	"P-384": "P-384",
	secp521r1: "P-521",
	"P-521": "P-521",
};

function memberDictionary(name: string, members: DictionaryMember[]): Converter<IdlDictionary> {
	return createDictionaryConverter(name, [members]);
}

const req = (key: string, converter: Converter): DictionaryMember => ({ key, converter, required: true });
const opt = (key: string, converter: Converter): DictionaryMember => ({ key, converter });

const convertAlgorithm = createDictionaryConverter("Algorithm", [[req("name", convertDOMString)]]);

// Converters for the members of each algorithm dictionary besides `name` (already converted).
const kAlgorithmConverters: Record<string, Converter<IdlDictionary>> = {
	RsaHashedKeyGenParams: createDictionaryConverter("RsaHashedKeyGenParams", [
		[req("modulusLength", convertUnsignedLong), req("publicExponent", convertUint8Array)],
		[req("hash", convertAlgorithmIdentifier)],
	]),
	RsaHashedImportParams: memberDictionary("RsaHashedImportParams", [req("hash", convertAlgorithmIdentifier)]),
	EcKeyImportParams: memberDictionary("EcKeyImportParams", [req("namedCurve", convertDOMString)]),
	EcKeyGenParams: memberDictionary("EcKeyGenParams", [req("namedCurve", convertDOMString)]),
	AesKeyGenParams: memberDictionary("AesKeyGenParams", [req("length", convertUnsignedShort)]),
	RsaPssParams: memberDictionary("RsaPssParams", [req("saltLength", convertUnsignedLong)]),
	RsaOaepParams: memberDictionary("RsaOaepParams", [opt("label", convertBufferSource)]),
	EcdsaParams: memberDictionary("EcdsaParams", [req("hash", convertAlgorithmIdentifier)]),
	HmacKeyGenParams: memberDictionary("HmacKeyGenParams", [
		req("hash", convertAlgorithmIdentifier),
		opt("length", convertUnsignedLong),
	]),
	HmacImportParams: memberDictionary("HmacImportParams", [
		req("hash", convertAlgorithmIdentifier),
		opt("length", convertUnsignedLong),
	]),
	HkdfParams: memberDictionary("HkdfParams", [
		req("hash", convertAlgorithmIdentifier),
		req("salt", convertBufferSource),
		req("info", convertBufferSource),
	]),
	CShakeParams: memberDictionary("CShakeParams", [
		req("outputLength", convertUnsignedLong),
		opt("functionName", convertBufferSource),
		opt("customization", convertBufferSource),
	]),
	Pbkdf2Params: memberDictionary("Pbkdf2Params", [
		req("salt", convertBufferSource),
		req("iterations", convertUnsignedLong),
		req("hash", convertAlgorithmIdentifier),
	]),
	AesDerivedKeyParams: memberDictionary("AesDerivedKeyParams", [req("length", convertUnsignedShort)]),
	AesCbcParams: memberDictionary("AesCbcParams", [req("iv", convertBufferSource)]),
	AeadParams: memberDictionary("AeadParams", [
		req("iv", convertBufferSource),
		opt("additionalData", convertBufferSource),
		opt("tagLength", convertOctet),
	]),
	AesCtrParams: memberDictionary("AesCtrParams", [req("counter", convertBufferSource), req("length", convertOctet)]),
	EcdhKeyDeriveParams: memberDictionary("EcdhKeyDeriveParams", [req("public", convertCryptoKey)]),
	ContextParams: memberDictionary("ContextParams", [opt("context", convertBufferSource)]),
};

// Members normalized after conversion: BufferSources are copied, hashes normalized as digests.
const kPostProcessed: Record<string, Record<string, "BufferSource" | "Hash">> = {
	AesCbcParams: { iv: "BufferSource" },
	AesCtrParams: { counter: "BufferSource" },
	AeadParams: { iv: "BufferSource", additionalData: "BufferSource" },
	RsaHashedKeyGenParams: { hash: "Hash", publicExponent: "BufferSource" },
	HmacKeyGenParams: { hash: "Hash" },
	EcdsaParams: { hash: "Hash" },
	HmacImportParams: { hash: "Hash" },
	HkdfParams: { hash: "Hash", salt: "BufferSource", info: "BufferSource" },
	ContextParams: { context: "BufferSource" },
	Pbkdf2Params: { hash: "Hash", salt: "BufferSource" },
	RsaOaepParams: { label: "BufferSource" },
	RsaHashedImportParams: { hash: "Hash" },
	CShakeParams: { functionName: "BufferSource", customization: "BufferSource" },
};

const kNormalizeOpts: ConvOpts = { prefix: "Failed to normalize algorithm", context: "passed algorithm" };

// https://w3c.github.io/webcrypto/#algorithm-normalization-normalize-an-algorithm
function normalizeAlgorithm(algorithm: unknown, op: Op): Algorithm {
	if (typeof algorithm === "string") return normalizeAlgorithm({ name: algorithm }, op);
	const initial = convertAlgorithm(algorithm, kNormalizeOpts);
	const { dictionaries, names } = getRegistry();
	const requested = initial.name as string;
	const canonical = names[op]?.get(requested.toUpperCase());
	if (canonical === undefined) throw domException("Unrecognized algorithm name", "NotSupportedError");
	// Registered names are ASCII: reject non-ASCII names that only matched through case folding.
	if (requested !== canonical) {
		for (let i = 0; i < requested.length; i++) {
			if (requested.charCodeAt(i) > 0x7f) throw domException("Unrecognized algorithm name", "NotSupportedError");
		}
	}
	const desiredType = (dictionaries[op] as Record<string, string | null>)[canonical];
	if (desiredType === null) return { name: canonical };
	const normalized = kAlgorithmConverters[desiredType](algorithm, kNormalizeOpts) as Algorithm;
	normalized.name = canonical;
	const post = kPostProcessed[desiredType];
	if (post !== undefined) {
		for (const member in post) {
			const value = normalized[member];
			if (post[member] === "BufferSource") {
				if (value) normalized[member] = new Uint8Array(bytesOf(value as Bytes));
			} else {
				const hash = normalizeAlgorithm(value, "digest");
				if (!hash.name.startsWith("SHA")) {
					throw domException(`Only SHA hashes are supported in ${desiredType}`, "NotSupportedError");
				}
				normalized[member] = hash;
			}
		}
	}
	return normalized;
}

function validateByteLength(value: unknown, name: string, target: number): void {
	if ((value as Bytes).byteLength !== target)
		throw domException(`${name} must contain exactly ${target} bytes`, "OperationError");
}

function validateAesLength(value: unknown): void {
	if (value !== 128 && value !== 192 && value !== 256) {
		throw domException("AES key length must be 128, 192, or 256 bits", "OperationError");
	}
}

function validateNamedCurve(value: unknown): void {
	if (typeof value !== "string" || !Object.hasOwn(kNamedCurveAliases, value)) {
		throw domException("Unrecognized namedCurve", "NotSupportedError");
	}
}

type MemberValidator = (value: unknown, algorithm: Algorithm) => void;

// Operation-specific checks, run by the algorithm implementations (validateAlgorithm) after
// normalization, in each dictionary's member order.
const kValidators: Record<string, [string, MemberValidator][]> = {
	RsaHashedKeyGenParams: [
		[
			"modulusLength",
			(value) => {
				if ((value as number) < 512)
					throw domException("algorithm.modulusLength must be at least 512", "OperationError");
			},
		],
		[
			"publicExponent",
			(value) => {
				const converted = bigIntArrayToUnsignedInt(value as Uint8Array);
				if (converted < 3) throw domException("algorithm.publicExponent must be at least 3", "OperationError");
				if (converted % 2 === 0) throw domException("algorithm.publicExponent must be odd", "OperationError");
			},
		],
	],
	EcKeyImportParams: [["namedCurve", validateNamedCurve]],
	EcKeyGenParams: [["namedCurve", validateNamedCurve]],
	AesKeyGenParams: [["length", validateAesLength]],
	AesDerivedKeyParams: [["length", validateAesLength]],
	HmacKeyGenParams: [
		[
			"length",
			(value) => {
				if (value === 0) throw domException("HmacKeyGenParams.length cannot be 0", "OperationError");
			},
		],
	],
	HmacImportParams: [
		[
			"length",
			(value) => {
				if (value === 0) throw domException("HmacImportParams.length cannot be 0", "DataError");
			},
		],
	],
	HkdfParams: [["info", (value) => validateMaxBufferLength(value as Bytes, "algorithm.info", 1024)]],
	CShakeParams: [
		// OpenSSL provides no cSHAKE customization: only the plain SHAKE form is supported (as in Node).
		[
			"customization",
			(value) => {
				if ((value as Bytes).byteLength !== 0)
					throw domException("Unsupported CShakeParams customization", "NotSupportedError");
			},
		],
		[
			"functionName",
			(value) => {
				if ((value as Bytes).byteLength !== 0)
					throw domException("Unsupported CShakeParams functionName", "NotSupportedError");
			},
		],
		[
			"outputLength",
			(value) => {
				if ((value as number) % 8 !== 0) throw domException("Invalid CShakeParams outputLength", "NotSupportedError");
			},
		],
	],
	AesCbcParams: [["iv", (value) => validateByteLength(value, "algorithm.iv", 16)]],
	AeadParams: [
		["additionalData", (value) => validateMaxBufferLength(value as Bytes, "algorithm.additionalData")],
		["iv", (value) => validateMaxBufferLength(value as Bytes, "algorithm.iv")],
		[
			"tagLength",
			(value) => {
				if (![32, 64, 96, 104, 112, 120, 128].includes(value as number)) {
					throw domException(`${value} is not a valid AES-GCM tag length`, "OperationError");
				}
			},
		],
	],
	AesCtrParams: [
		["counter", (value) => validateByteLength(value, "algorithm.counter", 16)],
		[
			"length",
			(value) => {
				if (value === 0 || (value as number) > 128) {
					throw domException("AES-CTR algorithm.length must be between 1 and 128", "OperationError");
				}
			},
		],
	],
	EcdhKeyDeriveParams: [
		[
			"public",
			(value, algorithm) => {
				const key = value as CryptoKey;
				if (getCryptoKeyType(key) !== "public")
					throw domException("algorithm.public must be a public key", "InvalidAccessError");
				if (getCryptoKeyAlgorithm(key).name.toLowerCase() !== algorithm.name.toLowerCase()) {
					throw domException("key algorithm mismatch", "InvalidAccessError");
				}
			},
		],
	],
	ContextParams: [["context", (value) => validateMaxBufferLength(value as Bytes, "ContextParams.context", 255)]],
};

function validateAlgorithm(algorithm: Algorithm, op: Op): void {
	// HMAC's "get key length" handles zero with a TypeError (getKeyLength).
	if (op === "get key length" && algorithm.name === "HMAC") return;
	const desiredType = getRegistry().dictionaries[op]?.[algorithm.name];
	if (desiredType == null) return;
	const checks = kValidators[desiredType];
	if (checks === undefined) return;
	for (const [key, check] of checks) {
		const value = algorithm[key];
		if (value !== undefined) check(value, algorithm);
	}
}

// WebCrypto hash name → OpenSSL digest name, and JWK "alg" values per context.
const kHashNames: Record<string, { node: string; rsa?: string; pss?: string; oaep?: string; hmac?: string }> = {
	"SHA-1": { node: "sha1", rsa: "RS1", pss: "PS1", oaep: "RSA-OAEP", hmac: "HS1" },
	"SHA-256": { node: "sha256", rsa: "RS256", pss: "PS256", oaep: "RSA-OAEP-256", hmac: "HS256" },
	"SHA-384": { node: "sha384", rsa: "RS384", pss: "PS384", oaep: "RSA-OAEP-384", hmac: "HS384" },
	"SHA-512": { node: "sha512", rsa: "RS512", pss: "PS512", oaep: "RSA-OAEP-512", hmac: "HS512" },
	"SHA3-256": { node: "sha3-256" },
	"SHA3-384": { node: "sha3-384" },
	"SHA3-512": { node: "sha3-512" },
	cSHAKE128: { node: "shake128" },
	cSHAKE256: { node: "shake256" },
};

function nodeHashName(name: string): string {
	return kHashNames[name].node;
}

function getBlockSize(name: string | undefined): number | undefined {
	switch (name) {
		case "SHA-1":
		case "SHA-256":
			return 512;
		case "SHA-384":
		case "SHA-512":
			return 1024;
		case "SHA3-256":
		case "SHA3-384":
		case "SHA3-512":
			// https://github.com/WICG/webcrypto-modern-algos/issues/23
			throw domException("Explicit algorithm length member is required", "NotSupportedError");
	}
	return undefined;
}

function getDigestSizeInBytes(name: string): number {
	switch (name) {
		case "SHA-1":
			return 20;
		case "SHA-256":
		case "SHA3-256":
			return 32;
		case "SHA-384":
		case "SHA3-384":
			return 48;
		default:
			return 64;
	}
}

// --- Key usages ---------------------------------------------------------------------------------

interface PairUsages {
	public: readonly string[];
	private: readonly string[];
	keygen: readonly string[];
}

function pairUsages(publicUsages: string[], privateUsages: string[]): PairUsages {
	return { public: publicUsages, private: privateUsages, keygen: [...publicUsages, ...privateUsages] };
}

function hasAnyNotIn(usages: Set<string>, allowed: readonly string[]): boolean {
	for (const usage of usages) if (!allowed.includes(usage)) return true;
	return false;
}

function verifyAcceptableKeyUse(subject: string, usages: Set<string>, allowed: readonly string[]): void {
	if (hasAnyNotIn(usages, allowed)) throw domException(`Unsupported key usage for ${subject} key`, "SyntaxError");
}

function validateKeyUsages(usages: readonly string[], allowed: readonly string[], subject: string): Set<string> {
	const set = new Set(usages);
	verifyAcceptableKeyUse(subject, set, allowed);
	return set;
}

function validateUsagesNotEmpty(usages: Set<string>): void {
	if (usages.size === 0) throw domException("Usages cannot be empty when creating a key.", "SyntaxError");
}

function getKeyPairUsages(usages: Set<string>, allowed: PairUsages): { public: Set<string>; private: Set<string> } {
	return {
		public: new Set(allowed.public.filter((usage) => usages.has(usage))),
		private: new Set(allowed.private.filter((usage) => usages.has(usage))),
	};
}

// Every requested usage must appear in the JWK's key_ops (unknown key operations are ignored).
function validateKeyOps(keyOps: string[] | undefined, usages: Set<string>): void {
	if (keyOps === undefined) return;
	const seen = new Set<string>();
	for (const op of keyOps) {
		if (seen.has(op)) throw domException("Duplicate key operation", "DataError");
		seen.add(op);
	}
	for (const usage of usages) {
		if (!seen.has(usage)) throw domException("Key operations and usage mismatch", "DataError");
	}
}

function validateJwk(
	keyData: JsonWebKey,
	kty: string,
	extractable: boolean,
	usages: Set<string>,
	expectedUse: string,
): void {
	if (typeof keyData.kty !== "string") throw domException("Invalid keyData", "DataError");
	if (keyData.kty !== kty) throw domException('Invalid JWK "kty" Parameter', "DataError");
	const isString = (value: unknown): boolean => typeof value === "string";
	const optionalString = (value: unknown): boolean => value === undefined || typeof value === "string";
	let valid: boolean;
	switch (kty) {
		case "RSA":
			valid =
				isString(keyData.n) &&
				isString(keyData.e) &&
				optionalString(keyData.d) &&
				(typeof keyData.d !== "string" || [keyData.p, keyData.q, keyData.dp, keyData.dq, keyData.qi].every(isString));
			break;
		case "EC":
			valid = isString(keyData.crv) && isString(keyData.x) && isString(keyData.y) && optionalString(keyData.d);
			break;
		case "OKP":
			valid = isString(keyData.crv) && isString(keyData.x) && optionalString(keyData.d);
			break;
		default:
			valid = isString(keyData.k);
	}
	if (!valid) throw domException("Invalid keyData", "DataError");
	if (usages.size > 0 && keyData.use !== undefined && keyData.use !== expectedUse) {
		throw domException('Invalid JWK "use" Parameter', "DataError");
	}
	validateKeyOps(keyData.key_ops, usages);
	if (keyData.ext === false && extractable === true) {
		throw domException('JWK "ext" Parameter and extractable mismatch', "DataError");
	}
}

// --- Key material helpers -----------------------------------------------------------------------

type ImportFormat = KeyFormat | "KeyObjectHandle";

function keyHandle(keyObject: KeyObject): unknown {
	return keyObject[kHandle];
}

function secretBytesOf(key: CryptoKey): Uint8Array {
	return keyHandle(getCryptoKeyObject(key)) as Uint8Array;
}

function pkeyOf(key: CryptoKey): PKey {
	return keyHandle(getCryptoKeyObject(key)) as PKey;
}

function importDerKey(keyData: Bytes, isPublic: boolean): PKey {
	try {
		return N.keyParse(keyData, "der", isPublic ? "public" : "private", isPublic ? "spki" : "pkcs8", null);
	} catch (err) {
		throw domException("Invalid keyData", "DataError", err);
	}
}

const kJwkFields: Record<string, { public: string[]; private: string[] }> = {
	RSA: { public: ["n", "e"], private: ["n", "e", "d", "p", "q", "dp", "dq", "qi"] },
	EC: { public: ["x", "y"], private: ["x", "y", "d"] },
	OKP: { public: ["x"], private: ["x", "d"] },
};

function importJwkKey(isPublic: boolean, keyData: JsonWebKey): PKey {
	try {
		const kty = keyData.kty as string;
		const parts: Record<string, Bytes | string> = {};
		if (kty !== "RSA") parts.crv = keyData.crv as string;
		for (const field of kJwkFields[kty][isPublic ? "public" : "private"])
			parts[field] = Buffer.from(keyData[field] as string, "base64url");
		return N.keyFromJwk(kty, parts, !isPublic);
	} catch (err) {
		throw domException("Invalid keyData", "DataError", err);
	}
}

function importJwkSecret(keyData: JsonWebKey): Uint8Array {
	return Buffer.from(keyData.k as string, "base64url");
}

function asymmetricKeyObject(pkey: PKey): KeyObject {
	return keyObjectFromHandle(pkey.type, pkey);
}

function createKeyPair(
	pair: [PKey, PKey],
	algorithm: KeyAlgorithm,
	extractable: boolean,
	usages: { public: Set<string>; private: Set<string> },
): { publicKey: CryptoKey; privateKey: CryptoKey } {
	return {
		publicKey: createCryptoKey(asymmetricKeyObject(pair[0]), algorithm, true, usages.public),
		privateKey: createCryptoKey(asymmetricKeyObject(pair[1]), algorithm, extractable, usages.private),
	};
}

function generateSecret(length: number): Promise<Uint8Array> {
	return job<Buffer>((cb) => N.randomBytes(numBitsToBytes(length), cb)).then((bytes) =>
		length % 8 === 0 ? bytes : truncateToBitLength(length, bytes),
	);
}

function signJob(
	pkey: PKey,
	data: Bytes,
	digest: string | null,
	padding: number,
	saltLength: number,
	dsaEncoding: number,
	context: Bytes | null,
	signature: Bytes | undefined,
): Promise<ArrayBuffer | boolean> {
	if (signature === undefined) {
		return job<Buffer>((cb) => N.sign(pkey, data, digest, padding, saltLength, dsaEncoding, context, cb)).then(
			toArrayBuffer,
		);
	}
	// A signature the native cannot even parse (e.g. a wrong-length IEEE P1363 one) does not verify.
	return job<boolean>((cb) =>
		N.verify(pkey, data, signature, digest, padding, saltLength, dsaEncoding, context, cb),
	).catch(() => false);
}

function checkSignKeyType(key: CryptoKey, signature: Bytes | undefined): void {
	const type = signature === undefined ? "private" : "public";
	if (getCryptoKeyType(key) !== type) throw domException(`Key must be a ${type} key`, "InvalidAccessError");
}

// --- Digest -------------------------------------------------------------------------------------

function asyncDigest(algorithm: Algorithm, data: Bytes): Promise<ArrayBuffer> {
	validateMaxBufferLength(data, "data");
	validateAlgorithm(algorithm, "digest");
	const name = nodeHashName(algorithm.name);
	const outputLength = algorithm.outputLength === undefined ? -1 : algorithm.outputLength / 8;
	if (outputLength === 0) return Promise.resolve(new ArrayBuffer(0));
	return syncJob(() => {
		const result = N.hashOnce(name, data, outputLength);
		if (result === null) throw operationError();
		return toArrayBuffer(result);
	});
}

// --- AES ----------------------------------------------------------------------------------------

const kAesCipherUsages = ["encrypt", "decrypt", "wrapKey", "unwrapKey"];
const kAesUsages: Record<string, readonly string[]> = {
	"AES-CBC": kAesCipherUsages,
	"AES-CTR": kAesCipherUsages,
	"AES-GCM": kAesCipherUsages,
	"AES-KW": ["wrapKey", "unwrapKey"],
};

function aesJwkAlgorithm(name: string, length: number): string {
	return `A${length}${name.slice(4)}`;
}

function validateAesKeyLength(length: number): void {
	if (length !== 128 && length !== 192 && length !== 256) throw domException("Invalid key length", "DataError");
}

function aesGenerateKey(algorithm: Algorithm, extractable: boolean, usages: string[]): Promise<CryptoKey> {
	const { name } = algorithm;
	const usagesSet = validateKeyUsages(usages, kAesUsages[name], name);
	validateAlgorithm(algorithm, "generateKey");
	validateUsagesNotEmpty(usagesSet);
	const length = algorithm.length as number;
	return generateSecret(length).then((bytes) =>
		createCryptoKey(createSecretKey(bytes), { name, length }, extractable, usagesSet),
	);
}

function aesImportKey(
	algorithm: Algorithm,
	format: ImportFormat,
	keyData: unknown,
	extractable: boolean,
	usages: string[],
): CryptoKey | undefined {
	const { name } = algorithm;
	const usagesSet = validateKeyUsages(usages, kAesUsages[name], name);
	let keyObject: KeyObject;
	let length: number;
	switch (format) {
		case "KeyObjectHandle":
			keyObject = keyData as KeyObject;
			length = (keyHandle(keyObject) as Uint8Array).byteLength * 8;
			validateAesKeyLength(length);
			break;
		case "raw-secret":
		case "raw":
			length = (keyData as Bytes).byteLength * 8;
			validateAesKeyLength(length);
			keyObject = createSecretKey(new Uint8Array(bytesOf(keyData as Bytes)));
			break;
		case "jwk": {
			const jwk = keyData as JsonWebKey;
			validateJwk(jwk, "oct", extractable, usagesSet, "enc");
			const bytes = importJwkSecret(jwk);
			length = bytes.byteLength * 8;
			validateAesKeyLength(length);
			if (jwk.alg !== undefined && jwk.alg !== aesJwkAlgorithm(name, length)) {
				throw domException('JWK "alg" does not match the requested algorithm', "DataError");
			}
			keyObject = createSecretKey(bytes);
			break;
		}
		default:
			return undefined;
	}
	return createCryptoKey(keyObject, { name, length }, extractable, usagesSet);
}

// RFC 3394 default initial value.
const kAesKwIv = new Uint8Array([0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6]);

function runCipher(cipher: NativeCipher, data: Bytes): ArrayBuffer {
	return concatToArrayBuffer([cipher.update(data), cipher.final()]);
}

function aesGcmCipher(
	encrypt: boolean,
	keyBytes: Uint8Array,
	length: number,
	data: Bytes,
	algorithm: Algorithm,
): ArrayBuffer {
	const tagBytes = (algorithm.tagLength ?? 128) / 8;
	const cipherName = `aes-${length}-gcm`;
	if (encrypt) {
		const cipher = N.cipherNew(cipherName, keyBytes, algorithm.iv as Uint8Array, true, tagBytes);
		if (algorithm.additionalData !== undefined) cipher.setAAD(algorithm.additionalData, -1);
		const body = cipher.update(data);
		const final = cipher.final();
		return concatToArrayBuffer([body, final, cipher.getAuthTag() as Buffer]);
	}
	const input = bytesOf(data);
	if (input.byteLength < tagBytes) throw operationError();
	const cipher = N.cipherNew(cipherName, keyBytes, algorithm.iv as Uint8Array, false, tagBytes);
	cipher.setAuthTag(input.subarray(input.byteLength - tagBytes));
	if (algorithm.additionalData !== undefined) cipher.setAAD(algorithm.additionalData, -1);
	return runCipher(cipher, input.subarray(0, input.byteLength - tagBytes));
}

// AES-CTR with WebCrypto counter semantics: only the low `length` bits of the counter block
// increment and wrap to zero (OpenSSL increments all 128 bits, so a wrapping input runs in two parts).
function aesCtrCipher(
	encrypt: boolean,
	keyBytes: Uint8Array,
	length: number,
	data: Bytes,
	algorithm: Algorithm,
): ArrayBuffer {
	const counter = algorithm.counter as Uint8Array;
	const counterBits = BigInt(algorithm.length as number);
	const input = bytesOf(data);
	const cipherName = `aes-${length}-ctr`;
	let block = 0n;
	for (const byte of counter) block = (block << 8n) | BigInt(byte);
	const numCounters = 1n << counterBits;
	const current = block & (numCounters - 1n);
	const blocksNeeded = BigInt(Math.ceil(input.byteLength / 16));
	if (blocksNeeded > numCounters) throw operationError();
	const remaining = numCounters - current;
	if (remaining >= blocksNeeded) return runCipher(N.cipherNew(cipherName, keyBytes, counter, encrypt, -1), input);
	const split = Number(remaining) * 16;
	const first = runCipher(N.cipherNew(cipherName, keyBytes, counter, encrypt, -1), input.subarray(0, split));
	const zeroed = counter.slice();
	for (let bit = 0; bit < Number(counterBits); bit++) zeroed[15 - (bit >> 3)] &= ~(1 << (bit & 7));
	const second = runCipher(N.cipherNew(cipherName, keyBytes, zeroed, encrypt, -1), input.subarray(split));
	return concatToArrayBuffer([new Uint8Array(first), new Uint8Array(second)]);
}

function aesCipher(encrypt: boolean, key: CryptoKey, data: Bytes, algorithm: Algorithm): Promise<ArrayBuffer> {
	validateAlgorithm(algorithm, "encrypt");
	const keyBytes = secretBytesOf(key);
	const length = getCryptoKeyAlgorithm(key).length as number;
	return syncJob(() => {
		switch (algorithm.name) {
			case "AES-CTR":
				return aesCtrCipher(encrypt, keyBytes, length, data, algorithm);
			case "AES-CBC":
				return runCipher(N.cipherNew(`aes-${length}-cbc`, keyBytes, algorithm.iv as Uint8Array, encrypt, -1), data);
			case "AES-GCM":
				return aesGcmCipher(encrypt, keyBytes, length, data, algorithm);
			default:
				return runCipher(N.cipherNew(`aes-${length}-wrap`, keyBytes, kAesKwIv, encrypt, -1), data);
		}
	});
}

// --- HMAC ---------------------------------------------------------------------------------------

const kMacUsages = ["sign", "verify"];

function hmacGenerateKey(algorithm: Algorithm, extractable: boolean, usages: string[]): Promise<CryptoKey> {
	const { name, hash } = algorithm;
	const usagesSet = validateKeyUsages(usages, kMacUsages, name);
	validateAlgorithm(algorithm, "generateKey");
	const length = algorithm.length ?? (getBlockSize(hash?.name) as number);
	validateUsagesNotEmpty(usagesSet);
	return generateSecret(length).then((bytes) =>
		createCryptoKey(createSecretKey(bytes), { name, length, hash: hash as { name: string } }, extractable, usagesSet),
	);
}

function macImportKey(
	format: ImportFormat,
	keyData: unknown,
	algorithm: Algorithm,
	extractable: boolean,
	usages: string[],
): CryptoKey | undefined {
	validateAlgorithm(algorithm, "importKey");
	const usagesSet = validateKeyUsages(usages, kMacUsages, algorithm.name);
	let keyObject: KeyObject | undefined;
	let bytes: Uint8Array;
	switch (format) {
		case "KeyObjectHandle":
			keyObject = keyData as KeyObject;
			bytes = keyHandle(keyObject) as Uint8Array;
			break;
		case "raw-secret":
		case "raw":
			bytes = new Uint8Array(bytesOf(keyData as Bytes));
			break;
		case "jwk": {
			const jwk = keyData as JsonWebKey;
			validateJwk(jwk, "oct", extractable, usagesSet, "sig");
			if (jwk.alg !== undefined) {
				const expected = kHashNames[(algorithm.hash as Algorithm).name].hmac;
				if (expected && jwk.alg !== expected) {
					throw domException('JWK "alg" does not match the requested algorithm', "DataError");
				}
			}
			bytes = importJwkSecret(jwk);
			break;
		}
		default:
			return undefined;
	}
	let length = bytes.byteLength * 8;
	if (length === 0) throw domException("Zero-length key is not supported", "DataError");
	if (algorithm.length !== undefined) {
		if (numBitsToBytes(algorithm.length) !== bytes.byteLength) throw domException("Invalid key length", "DataError");
		if (algorithm.length % 8 !== 0) {
			bytes = truncateToBitLength(algorithm.length, bytes);
			keyObject = undefined;
		}
		length = algorithm.length;
	}
	keyObject ??= createSecretKey(bytes);
	return createCryptoKey(
		keyObject,
		{ name: algorithm.name, length, hash: algorithm.hash as { name: string } },
		extractable,
		usagesSet,
	);
}

function hmacSignVerify(key: CryptoKey, data: Bytes, signature: Bytes | undefined): Promise<ArrayBuffer | boolean> {
	const hash = nodeHashName((getCryptoKeyAlgorithm(key).hash as { name: string }).name);
	const keyBytes = secretBytesOf(key);
	return syncJob(() => {
		const hmac = N.hmacNew(hash, keyBytes);
		if (hmac === null) throw operationError();
		hmac.update(data);
		const mac = hmac.digest();
		if (signature === undefined) return toArrayBuffer(mac);
		return signature.byteLength === mac.byteLength && N.timingSafeEqual(mac, signature);
	});
}

// --- RSA ----------------------------------------------------------------------------------------

const kRsaUsages: Record<string, PairUsages> = {
	"RSA-OAEP": pairUsages(["encrypt", "wrapKey"], ["decrypt", "unwrapKey"]),
	"RSA-PSS": pairUsages(["verify"], ["sign"]),
	"RSASSA-PKCS1-v1_5": pairUsages(["verify"], ["sign"]),
};

function rsaJwkAlgorithm(name: string, hashName: string): string | undefined {
	const entry = kHashNames[hashName];
	return name === "RSASSA-PKCS1-v1_5" ? entry.rsa : name === "RSA-PSS" ? entry.pss : entry.oaep;
}

function rsaKeyGenerate(
	algorithm: Algorithm,
	extractable: boolean,
	usages: string[],
): Promise<{ publicKey: CryptoKey; privateKey: CryptoKey }> {
	const { name, modulusLength, hash } = algorithm;
	const publicExponent = algorithm.publicExponent as Uint8Array;
	const allowed = kRsaUsages[name];
	const usagesSet = validateKeyUsages(usages, allowed.keygen, name);
	validateAlgorithm(algorithm, "generateKey");
	const exponent = bigIntArrayToUnsignedInt(publicExponent);
	let firstNonzero = 0;
	while (publicExponent[firstNonzero] === 0) firstNonzero++;
	const keyUsages = getKeyPairUsages(usagesSet, allowed);
	validateUsagesNotEmpty(keyUsages.private);
	return job<[PKey, PKey]>((cb) => N.generateKeyPair("rsa", { modulusLength, publicExponent: exponent }, cb)).then(
		(pair) => {
			const keyAlgorithm: KeyAlgorithm = {
				name,
				modulusLength: pair[0].details().modulusLength ?? modulusLength,
				publicExponent: publicExponent.subarray(firstNonzero),
				hash: hash as { name: string },
			};
			return createKeyPair(pair, keyAlgorithm, extractable, keyUsages);
		},
	);
}

function rsaImportKey(
	format: ImportFormat,
	keyData: unknown,
	algorithm: Algorithm,
	extractable: boolean,
	usages: string[],
): CryptoKey | undefined {
	const allowed = kRsaUsages[algorithm.name];
	const usagesSet = new Set(usages);
	let pkey: PKey;
	let keyObject: KeyObject | undefined;
	switch (format) {
		case "KeyObjectHandle":
			keyObject = keyData as KeyObject;
			verifyAcceptableKeyUse(algorithm.name, usagesSet, keyObject.type === "public" ? allowed.public : allowed.private);
			pkey = keyHandle(keyObject) as PKey;
			break;
		case "spki":
			verifyAcceptableKeyUse(algorithm.name, usagesSet, allowed.public);
			pkey = importDerKey(keyData as Bytes, true);
			break;
		case "pkcs8":
			verifyAcceptableKeyUse(algorithm.name, usagesSet, allowed.private);
			pkey = importDerKey(keyData as Bytes, false);
			break;
		case "jwk": {
			const jwk = keyData as JsonWebKey;
			const isPublic = jwk.d === undefined;
			verifyAcceptableKeyUse(algorithm.name, usagesSet, isPublic ? allowed.public : allowed.private);
			validateJwk(jwk, "RSA", extractable, usagesSet, algorithm.name === "RSA-OAEP" ? "enc" : "sig");
			if (jwk.alg !== undefined && jwk.alg !== rsaJwkAlgorithm(algorithm.name, (algorithm.hash as Algorithm).name)) {
				throw domException('JWK "alg" does not match the requested algorithm', "DataError");
			}
			pkey = importJwkKey(isPublic, jwk);
			break;
		}
		default:
			return undefined;
	}
	if (pkey.asymmetricKeyType !== "rsa") throw domException("Invalid key type", "DataError");
	const details = pkey.details();
	return createCryptoKey(
		keyObject ?? asymmetricKeyObject(pkey),
		{
			name: algorithm.name,
			modulusLength: details.modulusLength,
			publicExponent: bigIntToBytes(details.publicExponent ?? 0),
			hash: algorithm.hash as { name: string },
		},
		extractable,
		usagesSet,
	);
}

function rsaSignVerify(
	key: CryptoKey,
	data: Bytes,
	algorithm: Algorithm,
	signature: Bytes | undefined,
): Promise<ArrayBuffer | boolean> | boolean {
	checkSignKeyType(key, signature);
	const keyAlgorithm = getCryptoKeyAlgorithm(key);
	const pss = keyAlgorithm.name === "RSA-PSS";
	if (pss && !isInt32(algorithm.saltLength)) {
		if (signature !== undefined) return false;
		throw operationError();
	}
	return signJob(
		pkeyOf(key),
		data,
		nodeHashName((keyAlgorithm.hash as { name: string }).name),
		pss ? RSA_PKCS1_PSS_PADDING : RSA_PKCS1_PADDING,
		pss ? (algorithm.saltLength as number) : kSaltLengthUnset,
		kSigEncDER,
		null,
		signature,
	);
}

function rsaOaepCipher(encrypt: boolean, key: CryptoKey, data: Bytes, algorithm: Algorithm): Promise<ArrayBuffer> {
	if (algorithm.label !== undefined) validateMaxBufferLength(algorithm.label, "algorithm.label");
	if (getCryptoKeyType(key) !== (encrypt ? "public" : "private")) {
		throw domException("The requested operation is not valid for the provided key", "InvalidAccessError");
	}
	const hash = nodeHashName((getCryptoKeyAlgorithm(key).hash as { name: string }).name);
	const pkey = pkeyOf(key);
	return syncJob(() =>
		toArrayBuffer(N.pkeyCrypt(encrypt ? 0 : 1, pkey, data, RSA_PKCS1_OAEP_PADDING, hash, algorithm.label ?? null)),
	);
}

// --- EC -----------------------------------------------------------------------------------------

const kEcUsages: Record<string, PairUsages> = {
	ECDH: pairUsages([], ["deriveKey", "deriveBits"]),
	ECDSA: pairUsages(["verify"], ["sign"]),
};

// DER SPKI sizes of uncompressed named-curve keys.
const kEcSpkiLength: Record<string, number> = { "P-256": 91, "P-384": 120, "P-521": 158 };

function pkeyNamedCurve(pkey: PKey): string | undefined {
	const curve = pkey.details().namedCurve ?? pkey.curve();
	return curve === undefined ? undefined : (kCurveToNamedCurve[curve] ?? curve);
}

function ecGenerateKey(
	algorithm: Algorithm,
	extractable: boolean,
	usages: string[],
): Promise<{ publicKey: CryptoKey; privateKey: CryptoKey }> {
	const { name } = algorithm;
	const allowed = kEcUsages[name];
	const usagesSet = validateKeyUsages(usages, allowed.keygen, name);
	validateAlgorithm(algorithm, "generateKey");
	const namedCurve = algorithm.namedCurve as string;
	const keyUsages = getKeyPairUsages(usagesSet, allowed);
	validateUsagesNotEmpty(keyUsages.private);
	return job<[PKey, PKey]>((cb) =>
		N.generateKeyPair("ec", { namedCurve: kNamedCurveAliases[namedCurve], paramEncoding: "named" }, cb),
	).then((pair) => createKeyPair(pair, { name, namedCurve }, extractable, keyUsages));
}

function ecImportKey(
	format: ImportFormat,
	keyData: unknown,
	algorithm: Algorithm,
	extractable: boolean,
	usages: string[],
): CryptoKey | undefined {
	const { name } = algorithm;
	validateAlgorithm(algorithm, "importKey");
	const namedCurve = algorithm.namedCurve as string;
	const allowed = kEcUsages[name];
	const usagesSet = new Set(usages);
	let pkey: PKey;
	let keyObject: KeyObject | undefined;
	switch (format) {
		case "KeyObjectHandle":
			keyObject = keyData as KeyObject;
			verifyAcceptableKeyUse(name, usagesSet, keyObject.type === "public" ? allowed.public : allowed.private);
			pkey = keyHandle(keyObject) as PKey;
			break;
		case "spki":
			verifyAcceptableKeyUse(name, usagesSet, allowed.public);
			pkey = importDerKey(keyData as Bytes, true);
			break;
		case "pkcs8":
			verifyAcceptableKeyUse(name, usagesSet, allowed.private);
			pkey = importDerKey(keyData as Bytes, false);
			break;
		case "jwk": {
			const jwk = keyData as JsonWebKey;
			const isPublic = jwk.d === undefined;
			verifyAcceptableKeyUse(name, usagesSet, isPublic ? allowed.public : allowed.private);
			validateJwk(jwk, "EC", extractable, usagesSet, name === "ECDH" ? "enc" : "sig");
			if (jwk.crv !== namedCurve) throw domException('JWK "crv" does not match the requested algorithm', "DataError");
			if (name === "ECDSA" && jwk.alg !== undefined) {
				const algNamedCurve =
					jwk.alg === "ES256" ? "P-256" : jwk.alg === "ES384" ? "P-384" : jwk.alg === "ES512" ? "P-521" : undefined;
				if (algNamedCurve !== namedCurve)
					throw domException('JWK "alg" does not match the requested algorithm', "DataError");
			}
			pkey = importJwkKey(isPublic, jwk);
			break;
		}
		case "raw":
			verifyAcceptableKeyUse(name, usagesSet, allowed.public);
			try {
				pkey = N.keyFromEcPoint(kNamedCurveAliases[namedCurve], keyData as Bytes);
			} catch (err) {
				throw domException("Invalid keyData", "DataError", err);
			}
			break;
		default:
			return undefined;
	}
	if (pkey.asymmetricKeyType !== "ec") throw domException("Invalid key type", "DataError");
	if (pkeyNamedCurve(pkey) !== namedCurve) throw domException("Named curve mismatch", "DataError");
	return createCryptoKey(keyObject ?? asymmetricKeyObject(pkey), { name, namedCurve }, extractable, usagesSet);
}

function ecdsaSignVerify(
	key: CryptoKey,
	data: Bytes,
	algorithm: Algorithm,
	signature: Bytes | undefined,
): Promise<ArrayBuffer | boolean> {
	checkSignKeyType(key, signature);
	return signJob(
		pkeyOf(key),
		data,
		nodeHashName((algorithm.hash as Algorithm).name),
		-1,
		kSaltLengthUnset,
		kSigEncP1363,
		null,
		signature,
	);
}

// --- Ed25519 / Ed448 / X25519 / X448 ------------------------------------------------------------

const kCfrgUsages: Record<string, PairUsages> = {
	X25519: pairUsages([], ["deriveKey", "deriveBits"]),
	X448: pairUsages([], ["deriveKey", "deriveBits"]),
	Ed25519: pairUsages(["verify"], ["sign"]),
	Ed448: pairUsages(["verify"], ["sign"]),
};

function cfrgGenerateKey(
	algorithm: Algorithm,
	extractable: boolean,
	usages: string[],
): Promise<{ publicKey: CryptoKey; privateKey: CryptoKey }> {
	const { name } = algorithm;
	const allowed = kCfrgUsages[name];
	const usagesSet = validateKeyUsages(usages, allowed.keygen, name);
	const keyUsages = getKeyPairUsages(usagesSet, allowed);
	validateUsagesNotEmpty(keyUsages.private);
	return job<[PKey, PKey]>((cb) => N.generateKeyPair(name.toLowerCase(), {}, cb)).then((pair) =>
		createKeyPair(pair, { name }, extractable, keyUsages),
	);
}

function cfrgImportKey(
	format: ImportFormat,
	keyData: unknown,
	algorithm: Algorithm,
	extractable: boolean,
	usages: string[],
): CryptoKey | undefined {
	const { name } = algorithm;
	const allowed = kCfrgUsages[name];
	const usagesSet = new Set(usages);
	let pkey: PKey;
	let keyObject: KeyObject | undefined;
	switch (format) {
		case "KeyObjectHandle":
			keyObject = keyData as KeyObject;
			verifyAcceptableKeyUse(name, usagesSet, keyObject.type === "public" ? allowed.public : allowed.private);
			pkey = keyHandle(keyObject) as PKey;
			break;
		case "spki":
			verifyAcceptableKeyUse(name, usagesSet, allowed.public);
			pkey = importDerKey(keyData as Bytes, true);
			break;
		case "pkcs8":
			verifyAcceptableKeyUse(name, usagesSet, allowed.private);
			pkey = importDerKey(keyData as Bytes, false);
			break;
		case "jwk": {
			const jwk = keyData as JsonWebKey;
			const isPublic = jwk.d === undefined;
			verifyAcceptableKeyUse(name, usagesSet, isPublic ? allowed.public : allowed.private);
			validateJwk(jwk, "OKP", extractable, usagesSet, name === "X25519" || name === "X448" ? "enc" : "sig");
			if (jwk.crv !== name) throw domException('JWK "crv" Parameter and algorithm name mismatch', "DataError");
			if (
				jwk.alg !== undefined &&
				(name === "Ed25519" || name === "Ed448") &&
				jwk.alg !== name &&
				jwk.alg !== "EdDSA"
			) {
				throw domException('JWK "alg" does not match the requested algorithm', "DataError");
			}
			pkey = importJwkKey(isPublic, jwk);
			break;
		}
		case "raw":
			verifyAcceptableKeyUse(name, usagesSet, allowed.public);
			try {
				pkey = N.keyFromRaw(name.toLowerCase(), keyData as Bytes, false);
			} catch (err) {
				throw domException("Invalid keyData", "DataError", err);
			}
			break;
		default:
			return undefined;
	}
	if (pkey.asymmetricKeyType !== name.toLowerCase()) throw domException("Invalid key type", "DataError");
	return createCryptoKey(keyObject ?? asymmetricKeyObject(pkey), { name }, extractable, usagesSet);
}

function eddsaSignVerify(
	key: CryptoKey,
	data: Bytes,
	algorithm: Algorithm,
	signature: Bytes | undefined,
): Promise<ArrayBuffer | boolean> {
	checkSignKeyType(key, signature);
	validateAlgorithm(algorithm, signature === undefined ? "sign" : "verify");
	const context = algorithm.name === "Ed448" && algorithm.context !== undefined ? algorithm.context : null;
	return signJob(pkeyOf(key), data, null, -1, kSaltLengthUnset, kSigEncDER, context, signature);
}

// --- ECDH / X25519 / X448 derivation ------------------------------------------------------------

function ecdhDeriveBits(algorithm: Algorithm, baseKey: CryptoKey, length: number | null): Promise<ArrayBuffer> {
	validateAlgorithm(algorithm, "deriveBits");
	const publicKey = algorithm.public as CryptoKey;
	if (getCryptoKeyType(baseKey) !== "private")
		throw domException("baseKey must be a private key", "InvalidAccessError");
	const keyAlgorithm = getCryptoKeyAlgorithm(publicKey);
	const baseKeyAlgorithm = getCryptoKeyAlgorithm(baseKey);
	if (keyAlgorithm.name !== baseKeyAlgorithm.name) {
		throw domException("The public and private keys must be of the same type", "InvalidAccessError");
	}
	if (keyAlgorithm.name === "ECDH" && keyAlgorithm.namedCurve !== baseKeyAlgorithm.namedCurve) {
		throw domException("Named curve mismatch", "InvalidAccessError");
	}
	const priv = pkeyOf(baseKey);
	const pub = pkeyOf(publicKey);
	const bits = job<Buffer>((cb) => N.pkeyDerive(priv, pub, cb));
	if (length === null) return bits.then(toArrayBuffer);
	return bits.then((secret) => {
		const sliceLength = numBitsToBytes(length);
		if (secret.byteLength < sliceLength) throw domException("derived bit length is too small", "OperationError");
		if (length % 8 === 0 && secret.byteLength === sliceLength) return toArrayBuffer(secret);
		return toArrayBuffer(truncateToBitLength(length, secret));
	});
}

// --- HKDF / PBKDF2 ------------------------------------------------------------------------------

function importGenericSecretKey(
	algorithm: Algorithm,
	format: ImportFormat,
	keyData: unknown,
	extractable: boolean,
	usages: string[],
): CryptoKey | undefined {
	const usagesSet = new Set(usages);
	const { name } = algorithm;
	if (extractable) throw domException(`${name} keys are not extractable`, "SyntaxError");
	if (hasAnyNotIn(usagesSet, ["deriveKey", "deriveBits"])) {
		throw domException(`Unsupported key usage for a ${name} key`, "SyntaxError");
	}
	let keyObject: KeyObject;
	switch (format) {
		case "KeyObjectHandle":
			keyObject = keyData as KeyObject;
			break;
		case "raw-secret":
		case "raw":
			keyObject = createSecretKey(new Uint8Array(bytesOf(keyData as Bytes)));
			break;
		default:
			return undefined;
	}
	return createCryptoKey(keyObject, { name }, false, usagesSet);
}

function validateHkdfDeriveBitsLength(length: number | null | undefined, hash: Algorithm): void {
	if (length == null) throw domException("length cannot be null", "OperationError");
	if (length % 8) throw domException("length must be a multiple of 8", "OperationError");
	if (length > 255 * getDigestSizeInBytes(hash.name) * 8) {
		throw domException("length exceeds the maximum derived bit length", "OperationError");
	}
}

function hkdfDeriveBits(algorithm: Algorithm, baseKey: CryptoKey, length: number | null): Promise<ArrayBuffer> {
	const hash = algorithm.hash as Algorithm;
	validateHkdfDeriveBitsLength(length, hash);
	validateAlgorithm(algorithm, "deriveBits");
	const bytes = (length as number) / 8;
	if (bytes === 0) return Promise.resolve(new ArrayBuffer(0));
	const ikm = secretBytesOf(baseKey);
	return job<Buffer>((cb) =>
		N.hkdf(nodeHashName(hash.name), ikm, algorithm.salt as Uint8Array, algorithm.info as Uint8Array, bytes, cb),
	).then(toArrayBuffer);
}

function validatePbkdf2DeriveBits(algorithm: Algorithm, length: number | null | undefined): void {
	if (length == null) throw domException("length cannot be null", "OperationError");
	if (length % 8) throw domException("length must be a multiple of 8", "OperationError");
	if (algorithm.iterations === 0) throw domException("iterations cannot be zero", "OperationError");
	if (length !== 0 && !isInt32(algorithm.iterations)) {
		throw domException("iterations exceeds the implementation limit", "OperationError");
	}
}

function pbkdf2DeriveBits(algorithm: Algorithm, baseKey: CryptoKey, length: number | null): Promise<ArrayBuffer> {
	validatePbkdf2DeriveBits(algorithm, length);
	const bytes = (length as number) / 8;
	if (bytes === 0) return Promise.resolve(new ArrayBuffer(0));
	const password = secretBytesOf(baseKey);
	const hash = nodeHashName((algorithm.hash as Algorithm).name);
	return job<Buffer>((cb) =>
		N.pbkdf2(password, algorithm.salt as Uint8Array, algorithm.iterations as number, bytes, hash, cb),
	).then(toArrayBuffer);
}

// --- Shared operations --------------------------------------------------------------------------

function deriveBitsFor(algorithm: Algorithm, baseKey: CryptoKey, length: number | null): Promise<ArrayBuffer> {
	switch (algorithm.name) {
		case "HKDF":
			return hkdfDeriveBits(algorithm, baseKey, length);
		case "PBKDF2":
			return pbkdf2DeriveBits(algorithm, baseKey, length);
		default:
			return ecdhDeriveBits(algorithm, baseKey, length);
	}
}

function getKeyLength(algorithm: Algorithm): number | null {
	validateAlgorithm(algorithm, "get key length");
	const { name, length } = algorithm;
	switch (name) {
		case "AES-CTR":
		case "AES-CBC":
		case "AES-GCM":
		case "AES-KW":
			if (length !== 128 && length !== 192 && length !== 256)
				throw domException("Invalid key length", "OperationError");
			return length;
		case "HMAC":
			if (length === 0) {
				throw codedError(
					"ERR_INVALID_ARG_VALUE",
					"The property 'algorithm.length' must not be zero. Received 0",
					TypeError,
				);
			}
			if (length === undefined) return getBlockSize(algorithm.hash?.name) ?? null;
			if (typeof length === "number") return length;
			throw domException("Invalid key length", "OperationError");
		default:
			return null;
	}
}

function importKeySync(
	format: KeyFormat,
	keyData: unknown,
	algorithm: Algorithm,
	extractable: boolean,
	usages: string[],
): CryptoKey {
	let result: CryptoKey | undefined;
	switch (algorithm.name) {
		case "RSASSA-PKCS1-v1_5":
		case "RSA-PSS":
		case "RSA-OAEP":
			result = rsaImportKey(format, keyData, algorithm, extractable, usages);
			break;
		case "ECDSA":
		case "ECDH":
			result = ecImportKey(format === "raw-public" ? "raw" : format, keyData, algorithm, extractable, usages);
			break;
		case "Ed25519":
		case "Ed448":
		case "X25519":
		case "X448":
			result = cfrgImportKey(format === "raw-public" ? "raw" : format, keyData, algorithm, extractable, usages);
			break;
		case "HMAC":
			result = macImportKey(format, keyData, algorithm, extractable, usages);
			break;
		case "AES-CTR":
		case "AES-CBC":
		case "AES-GCM":
		case "AES-KW":
			result = aesImportKey(algorithm, format, keyData, extractable, usages);
			break;
		case "HKDF":
		case "PBKDF2":
			result = importGenericSecretKey(algorithm, format, keyData, extractable, usages);
			break;
	}
	if (result === undefined)
		throw domException(`Unable to import ${algorithm.name} using ${format} format`, "NotSupportedError");
	const type = getCryptoKeyType(result);
	if ((type === "secret" || type === "private") && getCryptoKeyUsages(result).length === 0) {
		throw domException(`Usages cannot be empty when importing a ${type} key.`, "SyntaxError");
	}
	return result;
}

function exportAsymmetric(key: CryptoKey, run: (pkey: PKey) => Uint8Array): ArrayBuffer {
	try {
		return toArrayBuffer(run(pkeyOf(key)));
	} catch (err) {
		throw operationError(err);
	}
}

function isAsymmetricAlgorithm(name: string): boolean {
	return (
		name.startsWith("RSA") || name.startsWith("EC") || name.startsWith("Ed") || name === "X25519" || name === "X448"
	);
}

function exportKeySpki(key: CryptoKey): ArrayBuffer | undefined {
	const algorithm = getCryptoKeyAlgorithm(key);
	if (!isAsymmetricAlgorithm(algorithm.name)) return undefined;
	if (getCryptoKeyType(key) !== "public") throw domException("Key must be a public key", "InvalidAccessError");
	return exportAsymmetric(key, (pkey) => {
		const spki = pkey.export("der", "spki", null, null);
		const namedCurve = algorithm.namedCurve;
		// Always encode EC points uncompressed with the named curve.
		if (namedCurve !== undefined && spki.byteLength !== kEcSpkiLength[namedCurve]) {
			return N.keyFromEcPoint(kNamedCurveAliases[namedCurve], pkey.ecPoint("uncompressed")).export(
				"der",
				"spki",
				null,
				null,
			);
		}
		return spki;
	});
}

function exportKeyPkcs8(key: CryptoKey): ArrayBuffer | undefined {
	if (!isAsymmetricAlgorithm(getCryptoKeyAlgorithm(key).name)) return undefined;
	if (getCryptoKeyType(key) !== "private") throw domException("Key must be a private key", "InvalidAccessError");
	return exportAsymmetric(key, (pkey) => pkey.export("der", "pkcs8", null, null));
}

function exportKeyRawPublic(key: CryptoKey): ArrayBuffer | undefined {
	const { name } = getCryptoKeyAlgorithm(key);
	const ec = name === "ECDSA" || name === "ECDH";
	if (!ec && name !== "Ed25519" && name !== "Ed448" && name !== "X25519" && name !== "X448") return undefined;
	if (getCryptoKeyType(key) !== "public") throw domException("Key must be a public key", "InvalidAccessError");
	return exportAsymmetric(key, (pkey) => (ec ? pkey.ecPoint("uncompressed") : pkey.rawPublic()));
}

function exportKeyRawSecret(key: CryptoKey): ArrayBuffer | undefined {
	switch (getCryptoKeyAlgorithm(key).name) {
		case "AES-CTR":
		case "AES-CBC":
		case "AES-GCM":
		case "AES-KW":
		case "HMAC":
			return new Uint8Array(secretBytesOf(key)).buffer;
		default:
			return undefined;
	}
}

function exportKeyJWK(key: CryptoKey): JsonWebKey | undefined {
	const algorithm = getCryptoKeyAlgorithm(key);
	let alg: string | undefined;
	switch (algorithm.name) {
		case "RSASSA-PKCS1-v1_5":
		case "RSA-PSS":
		case "RSA-OAEP":
			alg = rsaJwkAlgorithm(algorithm.name, (algorithm.hash as { name: string }).name);
			break;
		case "ECDSA":
		case "ECDH":
		case "X25519":
		case "X448":
			break;
		case "Ed25519":
		case "Ed448":
			alg = algorithm.name;
			break;
		case "AES-CTR":
		case "AES-CBC":
		case "AES-GCM":
		case "AES-KW":
			alg = aesJwkAlgorithm(algorithm.name, algorithm.length as number);
			break;
		case "HMAC":
			alg = kHashNames[(algorithm.hash as { name: string }).name].hmac;
			break;
		default:
			return undefined;
	}
	const jwk: JsonWebKey = { key_ops: [...getCryptoKeyUsages(key)], ext: getCryptoKeyExtractable(key), alg };
	if (alg === undefined) delete jwk.alg;
	try {
		if (getCryptoKeyType(key) === "secret") {
			jwk.kty = "oct";
			jwk.k = base64UrlEncode(secretBytesOf(key));
		} else {
			const parts = pkeyOf(key).jwk();
			jwk.kty = parts.kty as string;
			for (const field in parts) {
				const value = parts[field];
				jwk[field] = typeof value === "string" ? value : base64UrlEncode(value);
			}
		}
	} catch (err) {
		throw operationError(err);
	}
	return jwk;
}

function exportKeySync(format: KeyFormat, key: CryptoKey): ArrayBuffer | JsonWebKey {
	const algorithm = getCryptoKeyAlgorithm(key);
	try {
		normalizeAlgorithm(algorithm, "exportKey");
	} catch {
		throw domException(`${algorithm.name} key export is not supported`, "NotSupportedError");
	}
	if (!getCryptoKeyExtractable(key)) throw domException("key is not extractable", "InvalidAccessError");
	const type = getCryptoKeyType(key);
	let result: ArrayBuffer | JsonWebKey | undefined;
	switch (format) {
		case "spki":
			result = exportKeySpki(key);
			break;
		case "pkcs8":
			result = exportKeyPkcs8(key);
			break;
		case "jwk":
			result = exportKeyJWK(key);
			break;
		case "raw-secret":
			if (type === "secret") result = exportKeyRawSecret(key);
			break;
		case "raw-public":
			result = exportKeyRawPublic(key);
			break;
		case "raw":
			result = type === "secret" ? exportKeyRawSecret(key) : exportKeyRawPublic(key);
			break;
	}
	if (result === undefined) {
		throw domException(`Unable to export ${algorithm.name} ${type} key using ${format} format`, "NotSupportedError");
	}
	return result;
}

function cipherOrWrap(encrypt: boolean, algorithm: Algorithm, key: CryptoKey, data: Bytes): Promise<ArrayBuffer> {
	// WebCrypto allows larger inputs, but OpenSSL's APIs take int lengths.
	validateMaxBufferLength(data, "data");
	if (algorithm.name === "RSA-OAEP") return rsaOaepCipher(encrypt, key, data, algorithm);
	return aesCipher(encrypt, key, data, algorithm);
}

function signVerify(
	algorithm: unknown,
	key: CryptoKey,
	data: Bytes,
	signature?: Bytes,
): Promise<ArrayBuffer | boolean> | boolean {
	const operation = signature !== undefined ? "verify" : "sign";
	const normalized = normalizeAlgorithm(algorithm, operation);
	if (normalized.name !== getCryptoKeyAlgorithm(key).name)
		throw domException("Key algorithm mismatch", "InvalidAccessError");
	if (!hasCryptoKeyUsage(key, operation))
		throw domException(`Unable to use this key to ${operation}`, "InvalidAccessError");
	switch (normalized.name) {
		case "RSA-PSS":
		case "RSASSA-PKCS1-v1_5":
			return rsaSignVerify(key, data, normalized, signature);
		case "ECDSA":
			return ecdsaSignVerify(key, data, normalized, signature);
		case "Ed25519":
		case "Ed448":
			return eddsaSignVerify(key, data, normalized, signature);
		default:
			return hmacSignVerify(key, data, signature);
	}
}

function parseJwk(data: ArrayBuffer): JsonWebKey {
	let key: JsonWebKey;
	try {
		const json = new TextDecoder("utf-8", { fatal: true }).decode(data);
		key = convertJsonWebKey(JSON.parse(json), {});
	} catch (err) {
		throw domException("Invalid wrapped JWK key", "DataError", err);
	}
	if (key.kty === undefined) throw domException("Invalid wrapped JWK key", "DataError");
	return key;
}

type CryptoKeyPair = { publicKey: CryptoKey; privateKey: CryptoKey };

function toCryptoKeySecret(
	keyObject: KeyObject,
	algorithm: Algorithm,
	extractable: boolean,
	usages: string[],
): CryptoKey {
	let result: CryptoKey | undefined;
	switch (algorithm.name) {
		case "HMAC":
			result = macImportKey("KeyObjectHandle", keyObject, algorithm, extractable, usages);
			break;
		case "AES-CTR":
		case "AES-CBC":
		case "AES-GCM":
		case "AES-KW":
			result = aesImportKey(algorithm, "KeyObjectHandle", keyObject, extractable, usages);
			break;
		case "HKDF":
		case "PBKDF2":
			result = importGenericSecretKey(algorithm, "KeyObjectHandle", keyObject, extractable, usages);
			break;
		default:
			throw domException("Unrecognized algorithm name", "NotSupportedError");
	}
	const key = result as CryptoKey;
	if (getCryptoKeyUsages(key).length === 0) {
		throw domException(`Usages cannot be empty when importing a ${getCryptoKeyType(key)} key.`, "SyntaxError");
	}
	return key;
}

function toCryptoKeyAsymmetric(
	keyObject: KeyObject,
	algorithm: Algorithm,
	extractable: boolean,
	usages: string[],
): CryptoKey {
	let result: CryptoKey | undefined;
	switch (algorithm.name) {
		case "RSASSA-PKCS1-v1_5":
		case "RSA-PSS":
		case "RSA-OAEP":
			result = rsaImportKey("KeyObjectHandle", keyObject, algorithm, extractable, usages);
			break;
		case "ECDSA":
		case "ECDH":
			result = ecImportKey("KeyObjectHandle", keyObject, algorithm, extractable, usages);
			break;
		case "Ed25519":
		case "Ed448":
		case "X25519":
		case "X448":
			result = cfrgImportKey("KeyObjectHandle", keyObject, algorithm, extractable, usages);
			break;
		default:
			throw domException("Unrecognized algorithm name", "NotSupportedError");
	}
	const key = result as CryptoKey;
	const type = getCryptoKeyType(key);
	if (type === "private" && getCryptoKeyUsages(key).length === 0) {
		throw domException(`Usages cannot be empty when importing a ${type} key.`, "SyntaxError");
	}
	return key;
}

// --- SubtleCrypto methods -----------------------------------------------------------------------

export function digest(args: ArrayLike<unknown>): Promise<ArrayBuffer> {
	const prefix = prepare("digest", args, 2);
	const algorithm = convertArg(prefix, convertAlgorithmIdentifier, args[0], 0);
	const data = convertArg(prefix, convertBufferSource, args[1], 1);
	return asyncDigest(normalizeAlgorithm(algorithm, "digest"), data);
}

export function generateKey(args: ArrayLike<unknown>): Promise<CryptoKey | CryptoKeyPair> {
	const prefix = prepare("generateKey", args, 3);
	const algorithm = convertArg(prefix, convertAlgorithmIdentifier, args[0], 0);
	const extractable = convertArg(prefix, convertBoolean, args[1], 1);
	const usages = convertArg(prefix, convertKeyUsages, args[2], 2);
	const normalized = normalizeAlgorithm(algorithm, "generateKey");
	switch (normalized.name) {
		case "RSASSA-PKCS1-v1_5":
		case "RSA-PSS":
		case "RSA-OAEP":
			return rsaKeyGenerate(normalized, extractable, usages);
		case "Ed25519":
		case "Ed448":
		case "X25519":
		case "X448":
			return cfrgGenerateKey(normalized, extractable, usages);
		case "ECDSA":
		case "ECDH":
			return ecGenerateKey(normalized, extractable, usages);
		case "HMAC":
			return hmacGenerateKey(normalized, extractable, usages);
		default:
			return aesGenerateKey(normalized, extractable, usages);
	}
}

export function deriveBits(args: ArrayLike<unknown>): Promise<ArrayBuffer> {
	const prefix = prepare("deriveBits", args, 2);
	const algorithm = convertArg(prefix, convertAlgorithmIdentifier, args[0], 0);
	const baseKey = convertArg(prefix, convertCryptoKey, args[1], 1);
	let length: number | null = null;
	if (args.length > 2 && args[2] !== undefined && args[2] !== null)
		length = convertArg(prefix, convertUnsignedLong, args[2], 2);
	const normalized = normalizeAlgorithm(algorithm, "deriveBits");
	if (!hasCryptoKeyUsage(baseKey, "deriveBits"))
		throw domException("baseKey does not have deriveBits usage", "InvalidAccessError");
	if (getCryptoKeyAlgorithm(baseKey).name !== normalized.name)
		throw domException("Key algorithm mismatch", "InvalidAccessError");
	return deriveBitsFor(normalized, baseKey, length);
}

export function deriveKey(args: ArrayLike<unknown>): Promise<CryptoKey> {
	const prefix = prepare("deriveKey", args, 5);
	const algorithm = convertArg(prefix, convertAlgorithmIdentifier, args[0], 0);
	const baseKey = convertArg(prefix, convertCryptoKey, args[1], 1);
	const derivedKeyType = convertArg(prefix, convertAlgorithmIdentifier, args[2], 2);
	const extractable = convertArg(prefix, convertBoolean, args[3], 3);
	const usages = convertArg(prefix, convertKeyUsages, args[4], 4);
	const normalized = normalizeAlgorithm(algorithm, "deriveBits");
	const importAlgorithm = normalizeAlgorithm(derivedKeyType, "importKey");
	const lengthAlgorithm = normalizeAlgorithm(derivedKeyType, "get key length");
	if (!hasCryptoKeyUsage(baseKey, "deriveKey"))
		throw domException("baseKey does not have deriveKey usage", "InvalidAccessError");
	if (getCryptoKeyAlgorithm(baseKey).name !== normalized.name)
		throw domException("Key algorithm mismatch", "InvalidAccessError");
	const length = getKeyLength(lengthAlgorithm);
	return deriveBitsFor(normalized, baseKey, length).then((secret) =>
		importKeySync("raw-secret", secret, importAlgorithm, extractable, usages),
	);
}

export function importKey(args: ArrayLike<unknown>): CryptoKey {
	const prefix = prepare("importKey", args, 5);
	const format = convertArg(prefix, convertKeyFormat, args[0], 0);
	const isBufferSource = ArrayBuffer.isView(args[1]) || isArrayBuffer(args[1]);
	const keyData = isBufferSource
		? convertArg(prefix, convertBufferSource, args[1], 1)
		: convertArg(prefix, convertJsonWebKey, args[1], 1);
	const algorithm = convertArg(prefix, convertAlgorithmIdentifier, args[2], 2);
	const extractable = convertArg(prefix, convertBoolean, args[3], 3);
	const usages = convertArg(prefix, convertKeyUsages, args[4], 4);
	const normalized = normalizeAlgorithm(algorithm, "importKey");
	if ((format === "jwk") === isBufferSource) {
		throw invalidArgType("keyData", `an instance of ${format === "jwk" ? "JsonWebKey" : "BufferSource"}`, keyData);
	}
	return importKeySync(format, keyData, normalized, extractable, usages);
}

export function exportKey(args: ArrayLike<unknown>): ArrayBuffer | JsonWebKey {
	const prefix = prepare("exportKey", args, 2);
	const format = convertArg(prefix, convertKeyFormat, args[0], 0);
	const key = convertArg(prefix, convertCryptoKey, args[1], 1);
	return exportKeySync(format, key);
}

// subtle.wrapKey() is an exportKey() followed by an encrypt().
export function wrapKey(args: ArrayLike<unknown>): Promise<ArrayBuffer> {
	const prefix = prepare("wrapKey", args, 4);
	const format = convertArg(prefix, convertKeyFormat, args[0], 0);
	const key = convertArg(prefix, convertCryptoKey, args[1], 1);
	const wrappingKey = convertArg(prefix, convertCryptoKey, args[2], 2);
	const algorithm = convertArg(prefix, convertAlgorithmIdentifier, args[3], 3);
	let normalized: Algorithm;
	try {
		normalized = normalizeAlgorithm(algorithm, "wrapKey");
	} catch {
		normalized = normalizeAlgorithm(algorithm, "encrypt");
	}
	if (normalized.name !== getCryptoKeyAlgorithm(wrappingKey).name)
		throw domException("Key algorithm mismatch", "InvalidAccessError");
	if (!hasCryptoKeyUsage(wrappingKey, "wrapKey"))
		throw domException("Unable to use this key to wrapKey", "InvalidAccessError");
	const exported = exportKeySync(format, key);
	let bytes: Bytes;
	if (format === "jwk") {
		let json = JSON.stringify(exported);
		// Pad AES-KW wrapped JWKs to a multiple of 8 bytes (note in step 13 of the spec's wrapKey).
		if (normalized.name === "AES-KW" && json.length % 8 !== 0) json += " ".repeat(8 - (json.length % 8));
		bytes = Buffer.from(json, "utf8");
	} else {
		bytes = exported as ArrayBuffer;
	}
	return cipherOrWrap(true, normalized, wrappingKey, bytes);
}

// subtle.unwrapKey() is a decrypt() followed by an importKey().
export function unwrapKey(args: ArrayLike<unknown>): Promise<CryptoKey> {
	const prefix = prepare("unwrapKey", args, 7);
	const format = convertArg(prefix, convertKeyFormat, args[0], 0);
	const wrappedKey = convertArg(prefix, convertBufferSource, args[1], 1);
	const unwrappingKey = convertArg(prefix, convertCryptoKey, args[2], 2);
	const algorithm = convertArg(prefix, convertAlgorithmIdentifier, args[3], 3);
	const unwrappedKeyAlgorithm = convertArg(prefix, convertAlgorithmIdentifier, args[4], 4);
	const extractable = convertArg(prefix, convertBoolean, args[5], 5);
	const usages = convertArg(prefix, convertKeyUsages, args[6], 6);
	let normalized: Algorithm;
	try {
		normalized = normalizeAlgorithm(algorithm, "unwrapKey");
	} catch {
		normalized = normalizeAlgorithm(algorithm, "decrypt");
	}
	const keyAlgorithm = normalizeAlgorithm(unwrappedKeyAlgorithm, "importKey");
	if (normalized.name !== getCryptoKeyAlgorithm(unwrappingKey).name)
		throw domException("Key algorithm mismatch", "InvalidAccessError");
	if (!hasCryptoKeyUsage(unwrappingKey, "unwrapKey"))
		throw domException("Unable to use this key to unwrapKey", "InvalidAccessError");
	return cipherOrWrap(false, normalized, unwrappingKey, wrappedKey).then((bytes) =>
		importKeySync(format, format === "jwk" ? parseJwk(bytes) : bytes, keyAlgorithm, extractable, usages),
	);
}

export function sign(args: ArrayLike<unknown>): Promise<ArrayBuffer | boolean> | boolean {
	const prefix = prepare("sign", args, 3);
	const algorithm = convertArg(prefix, convertAlgorithmIdentifier, args[0], 0);
	const key = convertArg(prefix, convertCryptoKey, args[1], 1);
	const data = convertArg(prefix, convertBufferSource, args[2], 2);
	return signVerify(algorithm, key, data);
}

export function verify(args: ArrayLike<unknown>): Promise<ArrayBuffer | boolean> | boolean {
	const prefix = prepare("verify", args, 4);
	const algorithm = convertArg(prefix, convertAlgorithmIdentifier, args[0], 0);
	const key = convertArg(prefix, convertCryptoKey, args[1], 1);
	const signature = convertArg(prefix, convertBufferSource, args[2], 2);
	const data = convertArg(prefix, convertBufferSource, args[3], 3);
	return signVerify(algorithm, key, data, signature);
}

function cipherMethod(method: "encrypt" | "decrypt", args: ArrayLike<unknown>): Promise<ArrayBuffer> {
	const prefix = prepare(method, args, 3);
	const algorithm = convertArg(prefix, convertAlgorithmIdentifier, args[0], 0);
	const key = convertArg(prefix, convertCryptoKey, args[1], 1);
	const data = convertArg(prefix, convertBufferSource, args[2], 2);
	const normalized = normalizeAlgorithm(algorithm, method);
	if (normalized.name !== getCryptoKeyAlgorithm(key).name)
		throw domException("Key algorithm mismatch", "InvalidAccessError");
	if (!hasCryptoKeyUsage(key, method)) throw domException(`Unable to use this key to ${method}`, "InvalidAccessError");
	return cipherOrWrap(method === "encrypt", normalized, key, data);
}

export function encrypt(args: ArrayLike<unknown>): Promise<ArrayBuffer> {
	return cipherMethod("encrypt", args);
}

export function decrypt(args: ArrayLike<unknown>): Promise<ArrayBuffer> {
	return cipherMethod("decrypt", args);
}

// https://wicg.github.io/webcrypto-modern-algos/#SubtleCrypto-method-getPublicKey
export function getPublicKey(args: ArrayLike<unknown>): CryptoKey {
	const prefix = prepare("getPublicKey", args, 2);
	const key = convertArg(prefix, convertCryptoKey, args[0], 0);
	const usages = convertArg(prefix, convertKeyUsages, args[1], 1);
	const type = getCryptoKeyType(key);
	if (type !== "private") {
		throw domException("key must be a private key", type === "secret" ? "NotSupportedError" : "InvalidAccessError");
	}
	const publicKeyObject = keyObjectFromHandle("public", pkeyOf(key).toPublic());
	return toCryptoKeyAsymmetric(publicKeyObject, getCryptoKeyAlgorithm(key) as Algorithm, true, usages);
}

// Key encapsulation is only defined for ML-KEM, which qbun does not provide: normalization rejects
// every algorithm with NotSupportedError after the WebIDL conversions, as on a Node build without ML-KEM.
function kemMethod(method: string, args: ArrayLike<unknown>, required: number, op: Op, converters: Converter[]): never {
	const prefix = prepare(method, args, required);
	const converted = converters.map((converter, index) => convertArg(prefix, converter, args[index], index));
	normalizeAlgorithm(converted[0], op);
	throw domException("Unrecognized algorithm name", "NotSupportedError");
}

export function encapsulateBits(args: ArrayLike<unknown>): never {
	return kemMethod("encapsulateBits", args, 2, "encapsulate", [convertAlgorithmIdentifier, convertCryptoKey]);
}

export function encapsulateKey(args: ArrayLike<unknown>): never {
	return kemMethod("encapsulateKey", args, 5, "encapsulate", [
		convertAlgorithmIdentifier,
		convertCryptoKey,
		convertAlgorithmIdentifier,
		convertBoolean,
		convertKeyUsages,
	]);
}

export function decapsulateBits(args: ArrayLike<unknown>): never {
	return kemMethod("decapsulateBits", args, 3, "decapsulate", [
		convertAlgorithmIdentifier,
		convertCryptoKey,
		convertBufferSource,
	]);
}

export function decapsulateKey(args: ArrayLike<unknown>): never {
	return kemMethod("decapsulateKey", args, 6, "decapsulate", [
		convertAlgorithmIdentifier,
		convertCryptoKey,
		convertBufferSource,
		convertAlgorithmIdentifier,
		convertBoolean,
		convertKeyUsages,
	]);
}

// --- SubtleCrypto.supports ----------------------------------------------------------------------

const kSupportsOperations = [
	"decapsulateBits",
	"decapsulateKey",
	"decrypt",
	"deriveBits",
	"deriveKey",
	"digest",
	"encapsulateBits",
	"encapsulateKey",
	"encrypt",
	"exportKey",
	"generateKey",
	"getPublicKey",
	"importKey",
	"sign",
	"unwrapKey",
	"verify",
	"wrapKey",
];

const kEcdhMaxLength: Record<string, number> = { "P-256": 256, "P-384": 384, "P-521": 528 };

function check(op: string, algorithm: unknown, length?: number | null): boolean {
	if (op === "encapsulateBits" || op === "encapsulateKey") op = "encapsulate";
	if (op === "decapsulateBits" || op === "decapsulateKey") op = "decapsulate";
	let normalized: Algorithm;
	try {
		normalized = normalizeAlgorithm(algorithm, op as Op);
		validateAlgorithm(normalized, op as Op);
	} catch {
		if (op === "wrapKey") return check("encrypt", algorithm);
		if (op === "unwrapKey") return check("decrypt", algorithm);
		return false;
	}
	if (op === "deriveBits") {
		const bits = length as number;
		if (normalized.name === "HKDF") validateHkdfDeriveBitsLength(length, normalized.hash as Algorithm);
		if (normalized.name === "PBKDF2") validatePbkdf2DeriveBits(normalized, length);
		if (normalized.name === "X25519" && bits > 256) return false;
		if (normalized.name === "X448" && bits > 448) return false;
		if (normalized.name === "ECDH") {
			const namedCurve = getCryptoKeyAlgorithm(normalized.public as CryptoKey).namedCurve as string;
			if (bits > kEcdhMaxLength[namedCurve]) return false;
		}
		return true;
	}
	if (op === "generateKey") {
		return !(
			normalized.name === "HMAC" &&
			normalized.length === undefined &&
			(normalized.hash as Algorithm).name.startsWith("SHA3-")
		);
	}
	return true;
}

export function supports(args: ArrayLike<unknown>): boolean {
	const prefix = "Failed to execute 'supports' on 'SubtleCrypto'";
	requiredArguments(args.length, 2, prefix);
	let operation = convertDOMString(args[0], { prefix, context: "1st argument" });
	const algorithm = convertAlgorithmIdentifier(args[1], { prefix, context: "2nd argument" });
	const third = args.length > 2 && args[2] !== undefined ? args[2] : null;
	let length: number | null = null;
	let additionalAlgorithm: unknown;
	if (third === null || typeof third === "number") {
		if (third !== null) length = convertUnsignedLong(third, { prefix, context: "3rd argument" });
	} else {
		additionalAlgorithm = convertAlgorithmIdentifier(third, { prefix, context: "3rd argument" });
	}
	if (!kSupportsOperations.includes(operation)) return false;
	if (additionalAlgorithm !== undefined && operation === "deriveKey") {
		if (!check("importKey", additionalAlgorithm)) return false;
		try {
			length = getKeyLength(normalizeAlgorithm(additionalAlgorithm, "get key length"));
		} catch {
			return false;
		}
		operation = "deriveBits";
	} else if (additionalAlgorithm !== undefined && operation === "wrapKey") {
		if (!check("exportKey", additionalAlgorithm)) return false;
	} else if (additionalAlgorithm !== undefined && operation === "unwrapKey") {
		if (!check("importKey", additionalAlgorithm)) return false;
	} else if (operation === "getPublicKey") {
		try {
			return isAsymmetricAlgorithm(normalizeAlgorithm(algorithm, "exportKey").name);
		} catch {
			return false;
		}
	} else if (additionalAlgorithm !== undefined && (operation === "encapsulateKey" || operation === "decapsulateKey")) {
		// No algorithm defines "get shared key length".
		return false;
	}
	try {
		return check(operation, algorithm, length);
	} catch {
		return false;
	}
}

// --- KeyObject#toCryptoKey ----------------------------------------------------------------------

export function keyObjectToCryptoKey(
	keyObject: KeyObject,
	algorithm: unknown,
	extractable: unknown,
	usages: unknown,
): CryptoKey {
	const normalized = normalizeAlgorithm(convertAlgorithmIdentifier(algorithm, {}), "importKey");
	const isExtractable = convertBoolean(extractable, {});
	const keyUsages = convertKeyUsages(usages, {});
	if (keyObject.type === "secret") return toCryptoKeySecret(keyObject, normalized, isExtractable, keyUsages);
	return toCryptoKeyAsymmetric(keyObject, normalized, isExtractable, keyUsages);
}
