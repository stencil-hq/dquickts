// node:crypto — Node's crypto module on OpenSSL (qbun:crypto). The implementation lives in
// internal:crypto/*; this module assembles the public API.
import { native } from "internal:crypto/native";
import { cachedResult, errType, isAnyArrayBuffer, isArrayBufferView, validateString } from "internal:crypto/util";
import { codedError } from "internal:errors";
import { crypto as webcryptoInstance } from "internal:webcrypto";
import {
	KeyObject,
	createPrivateKey,
	createPublicKey,
	createSecretKey,
	generateKey,
	generateKeyPair,
	generateKeyPairSync,
	generateKeySync,
} from "internal:crypto/keys";
import { Hash, Hmac, createHash, createHmac, createMac, getHashes, getMacs, hash } from "internal:crypto/hash";
import {
	Cipheriv,
	Decipheriv,
	createCipheriv,
	createDecipheriv,
	getCipherInfo,
	getCiphers,
	privateDecrypt,
	privateEncrypt,
	publicDecrypt,
	publicEncrypt,
} from "internal:crypto/cipher";
import { Sign, Verify, createSign, createVerify, decapsulate, encapsulate, sign, verify } from "internal:crypto/sig";
import { argon2, argon2Sync, hkdf, hkdfSync, pbkdf2, pbkdf2Sync, scrypt, scryptSync } from "internal:crypto/kdf";
import {
	checkPrime,
	checkPrimeSync,
	generatePrime,
	generatePrimeSync,
	getRandomValues,
	prng,
	pseudoRandomBytes,
	randomBytes,
	randomFill,
	randomFillSync,
	randomInt,
	randomUUID,
	randomUUIDv7,
	rng,
} from "internal:crypto/random";
import {
	DiffieHellman,
	DiffieHellmanGroup,
	ECDH,
	createDiffieHellman,
	createDiffieHellmanGroup,
	createECDH,
	diffieHellman,
	getDiffieHellman,
} from "internal:crypto/dh";
import { Certificate, X509Certificate, parsePKCS12 } from "internal:crypto/x509";

function opensslVersionNumber(): number {
	const [major, minor, patch] = native.opensslVersion.split(".").map((n) => Number(n) || 0);
	return major * 0x10000000 + minor * 0x100000 + patch * 0x10;
}

const defaultCoreCipherList =
	"TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256:ECDHE-RSA-AES128-GCM-SHA256:" +
	"ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:" +
	"ECDHE-RSA-AES128-SHA256:DHE-RSA-AES128-SHA256:ECDHE-RSA-AES256-SHA384:DHE-RSA-AES256-SHA384:ECDHE-RSA-AES256-SHA256:" +
	"DHE-RSA-AES256-SHA256:HIGH:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!MD5:!PSK:!SRP:!CAMELLIA";

export const constants: Record<string, number | string> = {
	OPENSSL_VERSION_NUMBER: opensslVersionNumber(),
	SSL_OP_ALL: 2147485776,
	SSL_OP_ALLOW_NO_DHE_KEX: 1024,
	SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION: 262144,
	SSL_OP_CIPHER_SERVER_PREFERENCE: 4194304,
	SSL_OP_CISCO_ANYCONNECT: 32768,
	SSL_OP_COOKIE_EXCHANGE: 8192,
	SSL_OP_CRYPTOPRO_TLSEXT_BUG: 2147483648,
	SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS: 2048,
	SSL_OP_LEGACY_SERVER_CONNECT: 4,
	SSL_OP_NO_COMPRESSION: 131072,
	SSL_OP_NO_ENCRYPT_THEN_MAC: 524288,
	SSL_OP_NO_QUERY_MTU: 4096,
	SSL_OP_NO_RENEGOTIATION: 1073741824,
	SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION: 65536,
	SSL_OP_NO_SSLv2: 0,
	SSL_OP_NO_SSLv3: 33554432,
	SSL_OP_NO_TICKET: 16384,
	SSL_OP_NO_TLSv1: 67108864,
	SSL_OP_NO_TLSv1_1: 268435456,
	SSL_OP_NO_TLSv1_2: 134217728,
	SSL_OP_NO_TLSv1_3: 536870912,
	SSL_OP_PRIORITIZE_CHACHA: 2097152,
	SSL_OP_TLS_ROLLBACK_BUG: 8388608,
	ENGINE_METHOD_RSA: 1,
	ENGINE_METHOD_DSA: 2,
	ENGINE_METHOD_DH: 4,
	ENGINE_METHOD_RAND: 8,
	ENGINE_METHOD_EC: 2048,
	ENGINE_METHOD_CIPHERS: 64,
	ENGINE_METHOD_DIGESTS: 128,
	ENGINE_METHOD_PKEY_METHS: 512,
	ENGINE_METHOD_PKEY_ASN1_METHS: 1024,
	ENGINE_METHOD_ALL: 65535,
	ENGINE_METHOD_NONE: 0,
	DH_CHECK_P_NOT_SAFE_PRIME: 2,
	DH_CHECK_P_NOT_PRIME: 1,
	DH_UNABLE_TO_CHECK_GENERATOR: 4,
	DH_NOT_SUITABLE_GENERATOR: 8,
	RSA_PKCS1_PADDING: 1,
	RSA_NO_PADDING: 3,
	RSA_PKCS1_OAEP_PADDING: 4,
	RSA_X931_PADDING: 5,
	RSA_PKCS1_PSS_PADDING: 6,
	RSA_PSS_SALTLEN_DIGEST: -1,
	RSA_PSS_SALTLEN_MAX_SIGN: -2,
	RSA_PSS_SALTLEN_AUTO: -2,
	defaultCoreCipherList,
	defaultCipherList: defaultCoreCipherList,
	TLS1_VERSION: 769,
	TLS1_1_VERSION: 770,
	TLS1_2_VERSION: 771,
	TLS1_3_VERSION: 772,
	POINT_CONVERSION_COMPRESSED: 2,
	POINT_CONVERSION_UNCOMPRESSED: 4,
	POINT_CONVERSION_HYBRID: 6,
};

export function timingSafeEqual(buf1: unknown, buf2: unknown): boolean {
	const kTypes = "an instance of ArrayBuffer, Buffer, TypedArray, or DataView";
	if (!isAnyArrayBuffer(buf1) && !isArrayBufferView(buf1)) throw errType("buf1", kTypes, buf1);
	if (!isAnyArrayBuffer(buf2) && !isArrayBufferView(buf2)) throw errType("buf2", kTypes, buf2);
	if (buf1.byteLength !== buf2.byteLength) {
		throw codedError("ERR_CRYPTO_TIMING_SAFE_EQUAL_LENGTH", "Input buffers must have the same byte length", RangeError);
	}
	return native.timingSafeEqual(buf1, buf2);
}

export const getCurves = cachedResult(() => native.getCurves().sort());

export function getFips(): number {
	return 0;
}

export function setFips(enabled: unknown): void {
	if (enabled) throw codedError("ERR_CRYPTO_FIPS_FORCED", "Cannot set FIPS mode in a non-FIPS build.", Error);
}

export function setEngine(id: unknown, flags?: unknown): void {
	validateString(id, "id");
	if (flags !== undefined && typeof flags !== "number") throw errType("flags", "of type number", flags);
	throw codedError("ERR_CRYPTO_CUSTOM_ENGINE_NOT_SUPPORTED", "Custom engines not supported by this OpenSSL", Error);
}

export function secureHeapUsed(): { total: number; min: number; used: number; utilization: number } {
	return { total: 0, min: 0, used: 0, utilization: 0 };
}

export const webcrypto = webcryptoInstance;
export const subtle = webcryptoInstance.subtle;

export {
	Certificate,
	Cipheriv,
	Decipheriv,
	DiffieHellman,
	DiffieHellmanGroup,
	ECDH,
	Hash,
	Hmac,
	KeyObject,
	Sign,
	Verify,
	X509Certificate,
	argon2,
	argon2Sync,
	checkPrime,
	checkPrimeSync,
	createCipheriv,
	createDecipheriv,
	createDiffieHellman,
	createDiffieHellmanGroup,
	createECDH,
	createHash,
	createHmac,
	createMac,
	createPrivateKey,
	createPublicKey,
	createSecretKey,
	createSign,
	createVerify,
	decapsulate,
	diffieHellman,
	encapsulate,
	generateKey,
	generateKeyPair,
	generateKeyPairSync,
	generateKeySync,
	generatePrime,
	generatePrimeSync,
	getCipherInfo,
	getCiphers,
	getDiffieHellman,
	getHashes,
	getMacs,
	getRandomValues,
	hash,
	hkdf,
	hkdfSync,
	parsePKCS12,
	pbkdf2,
	pbkdf2Sync,
	privateDecrypt,
	privateEncrypt,
	prng,
	pseudoRandomBytes,
	publicDecrypt,
	publicEncrypt,
	randomBytes,
	randomFill,
	randomFillSync,
	randomInt,
	randomUUID,
	randomUUIDv7,
	rng,
	scrypt,
	scryptSync,
	sign,
	verify,
};

const cryptoModule = {
	Certificate,
	Cipheriv,
	Decipheriv,
	DiffieHellman,
	DiffieHellmanGroup,
	ECDH,
	Hash,
	Hmac,
	KeyObject,
	Sign,
	Verify,
	X509Certificate,
	argon2,
	argon2Sync,
	checkPrime,
	checkPrimeSync,
	constants,
	createCipheriv,
	createDecipheriv,
	createDiffieHellman,
	createDiffieHellmanGroup,
	createECDH,
	createHash,
	createHmac,
	createMac,
	createPrivateKey,
	createPublicKey,
	createSecretKey,
	createSign,
	createVerify,
	decapsulate,
	diffieHellman,
	encapsulate,
	generateKey,
	generateKeyPair,
	generateKeyPairSync,
	generateKeySync,
	generatePrime,
	generatePrimeSync,
	getCipherInfo,
	getCiphers,
	getCurves,
	getDiffieHellman,
	getFips,
	getHashes,
	getMacs,
	getRandomValues,
	hash,
	hkdf,
	hkdfSync,
	parsePKCS12,
	pbkdf2,
	pbkdf2Sync,
	privateDecrypt,
	privateEncrypt,
	prng,
	pseudoRandomBytes,
	publicDecrypt,
	publicEncrypt,
	randomBytes,
	randomFill,
	randomFillSync,
	randomInt,
	randomUUID,
	randomUUIDv7,
	rng,
	scrypt,
	scryptSync,
	secureHeapUsed,
	setEngine,
	setFips,
	sign,
	subtle,
	timingSafeEqual,
	verify,
	webcrypto,
};

Object.defineProperty(cryptoModule, "fips", {
	enumerable: false,
	configurable: true,
	get: getFips,
	set: setFips,
});

export default cryptoModule;
