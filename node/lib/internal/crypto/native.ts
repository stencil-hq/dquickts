// Typed view of the qbun:crypto natives (node/crypto.c).
import * as raw from "qbun:crypto";

export type Bytes = ArrayBuffer | SharedArrayBuffer | ArrayBufferView;
export type Data = Bytes | string;
export type NativeCallback<T> = (err: Error | null, result: T) => void;

export interface NativeDigest {
	update(data: Data): void;
	digest(): Buffer;
	copy(outputLength: number): NativeDigest;
	readonly size: number;
}

export interface NativeCipher {
	update(data: Bytes): Buffer;
	final(): Buffer;
	setAAD(aad: Bytes, plaintextLength: number): boolean;
	setAuthTag(tag: Bytes): boolean;
	getAuthTag(): Buffer | undefined;
	setAutoPadding(on: boolean): boolean;
}

export interface PKeyDetails {
	modulusLength?: number;
	publicExponent?: bigint;
	hashAlgorithm?: string;
	mgf1HashAlgorithm?: string;
	saltLength?: number;
	divisorLength?: number;
	namedCurve?: string;
}

export interface PKey {
	readonly type: "public" | "private";
	readonly asymmetricKeyType: string;
	readonly bits: number;
	details(): PKeyDetails;
	export(format: "pem" | "der", type: string, cipher: string | null, passphrase: Bytes | null): string | Buffer;
	jwk(): Record<string, Buffer | string>;
	rawPublic(): Buffer | undefined;
	rawPrivate(): Buffer | undefined;
	ecPoint(form: string): Buffer | undefined;
	curve(): string | undefined;
	toPublic(): PKey;
	equals(other: PKey): boolean;
}

export interface X509Info {
	subject: string;
	issuer: string;
	subjectAltName?: string;
	infoAccess?: string;
	validFrom: string;
	validTo: string;
	validFromDate: number;
	validToDate: number;
	fingerprint: string;
	fingerprint256: string;
	fingerprint512: string;
	serialNumber: string;
	keyUsage?: string[];
	ca: boolean;
	raw: Buffer;
	pem: string;
	signatureAlgorithm?: string;
	signatureAlgorithmOid: string;
	version: number;
	tbs: Buffer;
	signature: { algorithm: { oid: string; parameters?: Buffer }; value: Buffer };
	extensions: { oid: string; critical: boolean; value: Buffer }[];
	subjectName: { raw: Buffer; rdns: { oid: string; value?: string }[][] };
	issuerName: { raw: Buffer; rdns: { oid: string; value?: string }[][] };
	subjectObject: Record<string, string | string[]>;
	issuerObject: Record<string, string | string[]>;
}

export interface NativeX509 {
	info(): X509Info;
	publicKey(): PKey;
	verify(pub: PKey): boolean;
	checkIssued(issuer: NativeX509): boolean;
	checkPrivateKey(priv: PKey): boolean;
}

export interface CipherInfo {
	name: string;
	nid: number;
	blockSize?: number;
	ivLength?: number;
	keyLength: number;
	mode?: string;
}

export interface Natives {
	hashNew(name: string, outputLength: number): NativeDigest | null;
	hmacNew(digest: string, key: Data): NativeDigest | null;
	macNew(algorithm: string, key: Data, opts: Record<string, unknown>): NativeDigest;
	hashOnce(name: string, data: Data, outputLength: number): Buffer | null;
	digestInfo(name: string): { name: string; size: number; blockSize: number; xof: boolean } | null;
	getHashes(): string[];
	getCiphers(): string[];
	getCurves(): string[];
	getMacs(): string[];
	randomFill(target: Bytes, offset: number, size: number): void;
	randomBytes(size: number, cb?: NativeCallback<Buffer>): Buffer;
	timingSafeEqual(a: Bytes, b: Bytes): boolean;
	cipherNew(name: string, key: Bytes, iv: Bytes | null, encrypt: boolean, authTagLength: number): NativeCipher;
	cipherInfo(nameOrNid: string | number, keyLength: number, ivLength: number): CipherInfo | undefined;
	pbkdf2(
		password: Data,
		salt: Data,
		iterations: number,
		keylen: number,
		digest: string,
		cb?: NativeCallback<Buffer>,
	): Buffer;
	scrypt(
		password: Data,
		salt: Data,
		keylen: number,
		N: number,
		r: number,
		p: number,
		maxmem: number,
		cb?: NativeCallback<Buffer>,
	): Buffer;
	hkdf(digest: string, ikm: Data, salt: Data, info: Data, keylen: number, cb?: NativeCallback<Buffer>): Buffer;
	argon2(
		variant: string,
		message: Data,
		nonce: Data,
		secret: Data | null,
		ad: Data | null,
		parallelism: number,
		memory: number,
		passes: number,
		tagLength: number,
		cb?: NativeCallback<Buffer>,
	): Buffer;
	bcrypt(password: Data, salt: Bytes, cost: number, cb?: NativeCallback<Buffer>): Buffer;
	keyParse(
		data: Data,
		format: "pem" | "der",
		kind: "public" | "private" | "any",
		type: string | null,
		passphrase: Bytes | null,
	): PKey;
	keyFromJwk(kty: string, parts: Record<string, Bytes | string>, isPrivate: boolean): PKey;
	keyFromRaw(type: string, data: Bytes, isPrivate: boolean): PKey;
	keyFromEcPoint(curve: string, point: Bytes): PKey;
	keyFromEcPrivate(curve: string, d: Bytes): PKey;
	generateKeyPair(type: string, opts: Record<string, unknown>, cb?: NativeCallback<[PKey, PKey]>): [PKey, PKey];
	sign(
		key: PKey,
		data: Bytes,
		digest: string | null,
		padding: number,
		saltLength: number | undefined,
		dsaEncoding: number,
		context: Bytes | null,
		cb?: NativeCallback<Buffer>,
	): Buffer;
	verify(
		key: PKey,
		data: Bytes,
		signature: Bytes,
		digest: string | null,
		padding: number,
		saltLength: number | undefined,
		dsaEncoding: number,
		context: Bytes | null,
		cb?: NativeCallback<boolean>,
	): boolean;
	pkeyCrypt(
		op: number,
		key: PKey,
		data: Bytes,
		padding: number,
		oaepHash: string | null,
		oaepLabel: Bytes | null,
	): Buffer;
	pkeyDerive(priv: PKey, pub: PKey, cb?: NativeCallback<Buffer>): Buffer;
	kemEncapsulate(
		pub: PKey,
		cb?: NativeCallback<{ sharedKey: Buffer; ciphertext: Buffer }>,
	): { sharedKey: Buffer; ciphertext: Buffer };
	kemDecapsulate(priv: PKey, ct: Bytes, cb?: NativeCallback<Buffer>): Buffer;
	dhGroup(name: string): { prime: Buffer; generator: Buffer } | null;
	dhGeneratePrime(bits: number, generator: number): Buffer;
	dhCheck(prime: Bytes, generator: Bytes): number;
	dhGenerateKey(prime: Bytes, generator: Bytes, priv: Bytes | null): { priv: Buffer; pub: Buffer };
	dhCompute(prime: Bytes, priv: Bytes, peer: Bytes): Buffer;
	ecdhGenerate(curve: string): [Buffer, Buffer];
	ecdhPublic(curve: string, priv: Bytes): Buffer;
	ecdhCompute(curve: string, priv: Bytes, peer: Bytes): Buffer;
	ecdhConvert(curve: string, point: Bytes, form: string): Buffer;
	x509Parse(data: Data): NativeX509;
	parsePKCS12(der: Bytes, passphrase: Data | null): { key: PKey | null; cert: NativeX509 | null; ca: NativeX509[] };
	spkacVerify(data: Data): boolean;
	spkacPublicKey(data: Data): Buffer | null;
	spkacChallenge(data: Data): Buffer | null;
	generatePrime(bits: number, safe: boolean, add: Bytes | null, rem: Bytes | null, cb?: NativeCallback<Buffer>): Buffer;
	checkPrime(candidate: Bytes, checks: number, cb?: NativeCallback<boolean>): boolean;
	wyhash(d: Data, seed: bigint): bigint;
	adler32(d: Data): number;
	crc32(d: Data, seed: number): number;
	cityHash32(d: Data): number;
	cityHash64(d: Data, seed: bigint): bigint;
	xxHash32(d: Data, seed: number): number;
	xxHash64(d: Data, seed: bigint): bigint;
	xxHash3(d: Data, seed: bigint): bigint;
	murmur32v2(d: Data, seed: number): number;
	murmur32v3(d: Data, seed: number): number;
	murmur64v2(d: Data, seed: bigint): bigint;
	rapidhash(d: Data, seed: bigint): bigint;
	readonly opensslVersion: string;
}

// The C module's namespace has exactly the Natives shape (node/crypto.c qc_crypto_funcs).
const natives = raw as unknown as Natives;
export const native: Natives = natives;
