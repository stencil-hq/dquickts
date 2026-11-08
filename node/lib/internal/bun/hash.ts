// internal:bun/hash — Bun.hash (non-cryptographic hashes, bit-exact with Bun), Bun.CryptoHasher, the
// Bun.SHA1 / SHA224 / SHA256 / SHA384 / SHA512 / SHA512_256 / MD4 / MD5 classes, Bun.sha and Bun.password.
import { getBlobBytes } from "node:buffer";
import { native, type Data, type NativeDigest } from "internal:crypto/native";
import { codedError } from "internal:errors";

type TypedArray = Exclude<ArrayBufferView, DataView>;

function isBlob(v: unknown): v is Blob {
	return v instanceof Blob;
}

/** Bun's "BlobOrStringOrBuffer" input; null when `v` is none of those. */
function blobOrStringOrBuffer(v: unknown): Data | null {
	if (typeof v === "string" || ArrayBuffer.isView(v) || v instanceof ArrayBuffer) return v;
	if (typeof SharedArrayBuffer !== "undefined" && v instanceof SharedArrayBuffer) return v;
	if (v !== null && typeof v === "object" && isBlob(v)) return getBlobBytes(v);
	return null;
}

function inputError(name: string): Error {
	return codedError(
		"ERR_INVALID_ARG_TYPE",
		`${name} must be a string, ArrayBuffer, ArrayBufferView or Blob`,
		TypeError,
	);
}

function stringWithEncoding(data: string, encoding: unknown): Data {
	if (encoding === undefined || encoding === null) return data;
	const enc = String(encoding).toLowerCase();
	if (enc === "utf8" || enc === "utf-8") return data;
	return Buffer.from(data, enc as BufferEncoding);
}

// ------------------------------------------------------------------ Bun.hash

const kU64Max = (1n << 64n) - 1n;

/** Bun's toUInt64NoTruncate() of a seed argument (numbers saturate, BigInts wrap). */
function seed64(seed: unknown): bigint {
	if (typeof seed === "bigint") return BigInt.asUintN(64, seed);
	if (typeof seed === "number") {
		if (Number.isNaN(seed)) return 0n;
		if (seed >= 18446744073709551616) return kU64Max;
		if (seed <= -9223372036854775808) return 1n << 63n;
		return BigInt.asUintN(64, BigInt(Math.trunc(seed)));
	}
	return 0n;
}

function seed32(seed: unknown): number {
	return Number(seed64(seed) & 0xffffffffn);
}

function hashInput(data: unknown): Data {
	if (data === undefined) throw inputError("data");
	const d = blobOrStringOrBuffer(data);
	return d ?? String(data);
}

function wyhash(data: unknown, seed: unknown = undefined): bigint {
	return native.wyhash(hashInput(data), seed64(seed));
}

export const hash = Object.assign(
	function hash(data: unknown, seed: unknown = undefined): bigint {
		return native.wyhash(hashInput(data), seed64(seed));
	},
	{
		wyhash,
		adler32(data: unknown): number {
			return native.adler32(hashInput(data));
		},
		crc32(data: unknown, seed: unknown = undefined): number {
			return native.crc32(hashInput(data), seed32(seed));
		},
		cityHash32(data: unknown): number {
			return native.cityHash32(hashInput(data));
		},
		cityHash64(data: unknown, seed: unknown = undefined): bigint {
			return native.cityHash64(hashInput(data), seed64(seed));
		},
		xxHash32(data: unknown, seed: unknown = undefined): number {
			return native.xxHash32(hashInput(data), seed32(seed));
		},
		xxHash64(data: unknown, seed: unknown = undefined): bigint {
			return native.xxHash64(hashInput(data), seed64(seed));
		},
		xxHash3(data: unknown, seed: unknown = undefined): bigint {
			return native.xxHash3(hashInput(data), seed64(seed));
		},
		murmur32v2(data: unknown, seed: unknown = undefined): number {
			return native.murmur32v2(hashInput(data), seed32(seed));
		},
		murmur32v3(data: unknown, seed: unknown = undefined): number {
			return native.murmur32v3(hashInput(data), seed32(seed));
		},
		murmur64v2(data: unknown, seed: unknown = undefined): bigint {
			return native.murmur64v2(hashInput(data), seed64(seed));
		},
		rapidhash(data: unknown, seed: unknown = undefined): bigint {
			return native.rapidhash(hashInput(data), seed64(seed));
		},
	},
);

// -------------------------------------------------------------- CryptoHasher

const kAlgorithms = [
	"blake2b256",
	"blake2b512",
	"blake2s256",
	"md4",
	"md5",
	"ripemd160",
	"sha1",
	"sha224",
	"sha256",
	"sha384",
	"sha512",
	"sha512-224",
	"sha512-256",
	"sha3-224",
	"sha3-256",
	"sha3-384",
	"sha3-512",
	"shake128",
	"shake256",
];

function normalizeAlgorithm(algorithm: unknown): string | undefined {
	if (typeof algorithm !== "string") return undefined;
	const lower = algorithm.toLowerCase();
	if (kAlgorithms.includes(lower)) return lower;
	const candidates = [
		lower.replace(/^sha-(\d+)$/, "sha$1"),
		lower.replace(/^sha-(\d+)\/(\d+)$/, "sha$1-$2"),
		lower.replace(/^sha(\d+)\/(\d+)$/, "sha$1-$2"),
		lower.replace(/^rsa-/, ""),
		lower.replace(/-/g, ""),
	];
	for (const c of candidates) if (kAlgorithms.includes(c)) return c;
	return undefined;
}

/** digest output: encoding string, TypedArray to fill, or a fresh Buffer / Uint8Array */
function output(buf: Buffer, target: unknown, plain: boolean): Buffer | Uint8Array | string {
	if (typeof target === "string") {
		const enc = target.toLowerCase();
		if (enc === "buffer") return buf;
		return buf.toString(enc as BufferEncoding);
	}
	if (target !== undefined && target !== null && ArrayBuffer.isView(target) && !(target instanceof DataView)) {
		const t = target as TypedArray;
		if (t.byteLength < buf.byteLength) {
			throw codedError("ERR_INVALID_ARG_TYPE", `TypedArray must be at least ${buf.byteLength} bytes`, TypeError);
		}
		new Uint8Array(t.buffer, t.byteOffset, t.byteLength).set(buf);
		return t;
	}
	return plain ? new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength) : buf;
}

function updateInput(data: unknown, encoding: unknown): Data {
	if (typeof data === "string") return stringWithEncoding(data, encoding);
	const d = blobOrStringOrBuffer(data);
	if (d === null) throw inputError("input");
	return d;
}

function unsupported(algorithm: unknown): Error {
	return codedError("ERR_INVALID_ARG_TYPE", `Unsupported algorithm ${String(algorithm)}`, TypeError);
}

function consumed(): Error {
	return new Error("HMAC has been consumed and is no longer usable");
}

const kState = Symbol("kState");

interface HasherState {
	algorithm: string;
	digest: NativeDigest | null;
	hmac: boolean;
}

export class CryptoHasher {
	static readonly algorithms: string[] = kAlgorithms.slice();
	declare [kState]: HasherState;

	constructor(algorithm: unknown, hmacKey?: unknown) {
		const alg = normalizeAlgorithm(algorithm);
		if (!alg) throw unsupported(algorithm);
		let digest: NativeDigest | null;
		if (hmacKey !== undefined && hmacKey !== null) {
			const key = typeof hmacKey === "string" ? hmacKey : blobOrStringOrBuffer(hmacKey);
			if (key === null) throw inputError("hmacKey");
			digest = native.hmacNew(alg, key);
			if (!digest) throw new Error("HMAC is not supported for this algorithm yet");
		} else {
			digest = native.hashNew(alg, -1);
			if (!digest) throw unsupported(algorithm);
		}
		this[kState] = { algorithm: alg, digest, hmac: hmacKey !== undefined && hmacKey !== null };
	}

	static hash(algorithm: unknown, input: unknown, encoding?: unknown): Buffer | Uint8Array | string {
		const alg = normalizeAlgorithm(algorithm);
		if (!alg) throw unsupported(algorithm);
		const out = native.hashOnce(alg, updateInput(input, undefined), -1);
		if (!out) throw unsupported(algorithm);
		return output(out, encoding, false);
	}

	get algorithm(): string {
		return this[kState].algorithm;
	}

	get byteLength(): number {
		const s = this[kState];
		if (!s.digest) throw consumed();
		return s.digest.size;
	}

	update(data: unknown, encoding?: unknown): this {
		const s = this[kState];
		if (!s.digest) throw consumed();
		s.digest.update(updateInput(data, encoding));
		return this;
	}

	digest(encoding?: unknown): Buffer | Uint8Array | string {
		const s = this[kState];
		if (!s.digest) throw consumed();
		const out = s.digest.digest();
		// a hash restarts after digest(); an HMAC is consumed
		s.digest = s.hmac ? null : native.hashNew(s.algorithm, -1);
		return output(out, encoding, false);
	}

	copy(): CryptoHasher {
		const s = this[kState];
		if (!s.digest) throw consumed();
		const c = Object.create(CryptoHasher.prototype) as CryptoHasher;
		c[kState] = { algorithm: s.algorithm, digest: s.digest.copy(-1), hmac: s.hmac };
		return c;
	}

	get [Symbol.toStringTag](): string {
		return "CryptoHasher";
	}
}

// ------------------------------------------------------- SHA* / MD* classes

interface ShaClass {
	new (): ShaHasher;
	readonly byteLength: number;
	hash(input: unknown, encoding?: unknown): Uint8Array | string;
}

interface ShaHasher {
	readonly byteLength: number;
	update(data: unknown, encoding?: unknown): ShaHasher;
	digest(encoding?: unknown): Uint8Array | string;
}

function shaClass(name: string, algorithm: string, size: number): ShaClass {
	const kDigest = Symbol(name);
	const cls = class {
		declare [kDigest]: NativeDigest | null;

		constructor() {
			const d = native.hashNew(algorithm, -1);
			if (!d) throw unsupported(algorithm);
			this[kDigest] = d;
		}

		static get byteLength(): number {
			return size;
		}

		static hash(input: unknown, encoding?: unknown): Uint8Array | string {
			const out = native.hashOnce(algorithm, updateInput(input, undefined), -1);
			if (!out) throw unsupported(algorithm);
			return output(out, encoding, true) as Uint8Array | string;
		}

		get byteLength(): number {
			return size;
		}

		update(data: unknown, encoding?: unknown): this {
			const d = this[kDigest];
			if (!d)
				throw codedError(
					"ERR_INVALID_STATE",
					`${name} hasher already digested, create a new instance to update`,
					Error,
				);
			d.update(updateInput(data, encoding));
			return this;
		}

		digest(encoding?: unknown): Uint8Array | string {
			const d = this[kDigest];
			if (!d)
				throw codedError(
					"ERR_INVALID_STATE",
					`${name} hasher already digested, create a new instance to digest again`,
					Error,
				);
			this[kDigest] = null;
			return output(d.digest(), encoding, true) as Uint8Array | string;
		}
	};
	Object.defineProperty(cls, "name", { value: name });
	return cls as unknown as ShaClass;
}

export const SHA1 = shaClass("SHA1", "sha1", 20);
export const SHA224 = shaClass("SHA224", "sha224", 28);
export const SHA256 = shaClass("SHA256", "sha256", 32);
export const SHA384 = shaClass("SHA384", "sha384", 48);
export const SHA512 = shaClass("SHA512", "sha512", 64);
export const SHA512_256 = shaClass("SHA512_256", "sha512-256", 32);
export const MD4 = shaClass("MD4", "md4", 16);
export const MD5 = shaClass("MD5", "md5", 16);

/** Bun.sha(): SHA-512/256 one shot. */
export function sha(input: unknown, encoding?: unknown): Uint8Array | string {
	return SHA512_256.hash(input, encoding);
}

// --------------------------------------------------------------- Bun.password

type PasswordAlgorithm = "argon2id" | "argon2i" | "argon2d" | "bcrypt";

interface PasswordParams {
	algorithm: PasswordAlgorithm;
	memoryCost: number;
	timeCost: number;
	cost: number;
}

const kBcryptAlphabet = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

function bcryptEncode(bytes: Uint8Array): string {
	let out = "";
	let acc = 0;
	let bits = 0;
	for (const b of bytes) {
		acc = (acc << 8) | b;
		bits += 8;
		while (bits >= 6) {
			bits -= 6;
			out += kBcryptAlphabet[(acc >> bits) & 63];
		}
		acc &= (1 << bits) - 1;
	}
	if (bits > 0) out += kBcryptAlphabet[(acc << (6 - bits)) & 63];
	return out;
}

function bcryptDecode(text: string, length: number): Uint8Array | null {
	const out = new Uint8Array(length);
	let acc = 0;
	let bits = 0;
	let n = 0;
	for (const ch of text) {
		const v = kBcryptAlphabet.indexOf(ch);
		if (v < 0) return null;
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n < length) out[n++] = (acc >> bits) & 0xff;
		}
		acc &= (1 << bits) - 1;
	}
	return n === length ? out : null;
}

function passwordError(code: string, name: string): Error {
	return codedError(code, `Password verification failed with error "${name}"`, Error);
}

function argType(message: string): Error {
	return codedError("ERR_INVALID_ARG_TYPE", message, TypeError);
}

function passwordBytes(password: unknown, forHash: boolean): Data {
	const d = typeof password === "string" ? password : blobOrStringOrBuffer(password);
	if (d === null) throw argType("password must be a string or TypedArray");
	const len = typeof d === "string" ? d.length : d.byteLength;
	if (forHash && len === 0) throw argType("password must not be empty");
	return d;
}

function passwordParams(options: unknown, method: string): PasswordParams {
	const params: PasswordParams = { algorithm: "argon2id", memoryCost: 65536, timeCost: 2, cost: 10 };
	let alg: unknown = undefined;
	if (typeof options === "string") alg = options;
	else if (options !== undefined && options !== null && typeof options === "object") {
		const o = options as Record<string, unknown>;
		alg = o.algorithm;
		if (o.memoryCost !== undefined) {
			const m = Number(o.memoryCost);
			if (!Number.isFinite(m) || m < 8) throw argType("Memory cost must be at least 8");
			params.memoryCost = Math.trunc(m);
		}
		if (o.timeCost !== undefined) {
			const t = Number(o.timeCost);
			if (!Number.isFinite(t) || t < 1) throw argType("Time cost must be greater than 0");
			params.timeCost = Math.trunc(t);
		}
		if (o.cost !== undefined) {
			const c = Number(o.cost);
			if (!Number.isInteger(c) || c < 4 || c > 31) throw argType("Rounds must be an integer between 4 and 31");
			params.cost = c;
		}
	}
	if (alg !== undefined) {
		if (alg !== "argon2id" && alg !== "argon2i" && alg !== "argon2d" && alg !== "bcrypt") {
			throw argType(
				`Expected algorithm to be a unknown algorithm, expected one of: "bcrypt", "argon2id", "argon2d", "argon2i" (default is "argon2id") for '${method}'.`,
			);
		}
		params.algorithm = alg;
	}
	return params;
}

/** bcrypt input: passwords longer than 72 bytes are pre-hashed with SHA-512 like Bun */
function bcryptPassword(password: Data): Data {
	const bytes = typeof password === "string" ? Buffer.from(password, "utf8") : password;
	if (bytes.byteLength <= 72) return bytes;
	return native.hashOnce("sha512", bytes, -1) as Buffer;
}

function randomSalt(n: number): Uint8Array {
	const salt = new Uint8Array(n);
	native.randomFill(salt, 0, n);
	return salt;
}

function b64(bytes: Uint8Array): string {
	return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString("base64").replace(/=+$/, "");
}

function bcryptString(cost: number, salt: Uint8Array, dk: Uint8Array): string {
	return `$2b$${String(cost).padStart(2, "0")}$${bcryptEncode(salt)}${bcryptEncode(dk)}`;
}

function argon2String(alg: string, p: PasswordParams, salt: Uint8Array, out: Uint8Array): string {
	return `$${alg}$v=19$m=${p.memoryCost},t=${p.timeCost},p=1$${b64(salt)}$${b64(out)}`;
}

type Parsed =
	| { kind: "bcrypt"; cost: number; salt: Uint8Array; expected: string }
	| { kind: PasswordAlgorithm; memory: number; passes: number; lanes: number; salt: Uint8Array; expected: Uint8Array };

function parseHash(hashed: string): Parsed {
	if (hashed.startsWith("$2")) {
		if (hashed.length !== 60 || hashed[3] !== "$" || hashed[6] !== "$" || !/^\$2[abxy]\$/.test(hashed)) {
			throw passwordError("PASSWORD_INVALID_ENCODING", "InvalidEncoding");
		}
		const cost = Number(hashed.slice(4, 6));
		const salt = bcryptDecode(hashed.slice(7, 29), 16);
		if (!Number.isInteger(cost) || cost < 4 || cost > 31 || !salt)
			throw passwordError("PASSWORD_INVALID_ENCODING", "InvalidEncoding");
		return { kind: "bcrypt", cost, salt, expected: hashed.slice(29) };
	}
	const m = /^\$(argon2id|argon2i|argon2d)(?:\$v=(\d+))?\$([^$]+)\$([A-Za-z0-9+/]+)\$([A-Za-z0-9+/]+)$/.exec(hashed);
	if (!m) throw passwordError("PASSWORD_UNSUPPORTED_ALGORITHM", "UnsupportedAlgorithm");
	const params: Record<string, number> = {};
	for (const kv of m[3].split(",")) {
		const [k, v] = kv.split("=");
		if (!/^\d+$/.test(v ?? "")) throw passwordError("PASSWORD_INVALID_ENCODING", "InvalidEncoding");
		params[k] = Number(v);
	}
	if (params.m === undefined || params.t === undefined || params.p === undefined) {
		throw passwordError("PASSWORD_INVALID_ENCODING", "InvalidEncoding");
	}
	const salt = new Uint8Array(Buffer.from(m[4], "base64"));
	const expected = new Uint8Array(Buffer.from(m[5], "base64"));
	return { kind: m[1] as PasswordAlgorithm, memory: params.m, passes: params.t, lanes: params.p, salt, expected };
}

function constantTimeEqual(a: Uint8Array, b: Uint8Array): boolean {
	return a.byteLength === b.byteLength && native.timingSafeEqual(a, b);
}

function verifyHintMismatch(parsed: Parsed, hint: unknown): boolean {
	if (hint === undefined || hint === null) return false;
	return (hint === "bcrypt") !== (parsed.kind === "bcrypt");
}

function compareResult(parsed: Parsed, out: Uint8Array): boolean {
	if (parsed.kind === "bcrypt") {
		const enc = new TextEncoder();
		return constantTimeEqual(enc.encode(bcryptEncode(out)), enc.encode(parsed.expected));
	}
	return constantTimeEqual(out, parsed.expected);
}

function hashSync(password: unknown, algorithm?: unknown): string {
	const pw = passwordBytes(password, true);
	const p = passwordParams(algorithm, "hash");
	if (p.algorithm === "bcrypt") {
		const salt = randomSalt(16);
		return bcryptString(p.cost, salt, native.bcrypt(bcryptPassword(pw), salt, p.cost));
	}
	const salt = randomSalt(32);
	return argon2String(
		p.algorithm,
		p,
		salt,
		native.argon2(p.algorithm, pw, salt, null, null, 1, p.memoryCost, p.timeCost, 32),
	);
}

function hashAsync(password: unknown, algorithm?: unknown): Promise<string> {
	const { promise, resolve, reject } = Promise.withResolvers<string>();
	try {
		const pw = passwordBytes(password, true);
		const p = passwordParams(algorithm, "hash");
		const done = (format: (out: Uint8Array) => string) => (err: Error | null, out: Buffer) => {
			if (err) reject(err);
			else resolve(format(out));
		};
		if (p.algorithm === "bcrypt") {
			const salt = randomSalt(16);
			native.bcrypt(
				bcryptPassword(pw),
				salt,
				p.cost,
				done((out) => bcryptString(p.cost, salt, out)),
			);
		} else {
			const salt = randomSalt(32);
			native.argon2(
				p.algorithm,
				pw,
				salt,
				null,
				null,
				1,
				p.memoryCost,
				p.timeCost,
				32,
				done((out) => argon2String(p.algorithm, p, salt, out)),
			);
		}
	} catch (e) {
		reject(e);
	}
	return promise;
}

function verifyInputs(password: unknown, hashed: unknown): { pw: Data; hashed: string } | null {
	const pw = passwordBytes(password, false);
	if (typeof hashed !== "string" && !ArrayBuffer.isView(hashed)) throw argType("hash must be a string or TypedArray");
	const h = typeof hashed === "string" ? hashed : new TextDecoder().decode(hashed);
	if (h.length === 0 || (typeof pw === "string" ? pw.length : pw.byteLength) === 0) return null;
	return { pw, hashed: h };
}

function verifySync(password: unknown, hashed: unknown, algorithm?: unknown): boolean {
	const inputs = verifyInputs(password, hashed);
	if (!inputs) return false;
	const parsed = parseHash(inputs.hashed);
	if (verifyHintMismatch(parsed, algorithm)) return false;
	if (parsed.kind === "bcrypt")
		return compareResult(parsed, native.bcrypt(bcryptPassword(inputs.pw), parsed.salt, parsed.cost));
	if (parsed.salt.byteLength < 8 || parsed.expected.byteLength < 4) return false;
	return compareResult(
		parsed,
		native.argon2(
			parsed.kind,
			inputs.pw,
			parsed.salt,
			null,
			null,
			parsed.lanes,
			parsed.memory,
			parsed.passes,
			parsed.expected.byteLength,
		),
	);
}

function verifyAsync(password: unknown, hashed: unknown, algorithm?: unknown): Promise<boolean> {
	const { promise, resolve, reject } = Promise.withResolvers<boolean>();
	try {
		const inputs = verifyInputs(password, hashed);
		if (!inputs) {
			resolve(false);
			return promise;
		}
		const parsed = parseHash(inputs.hashed);
		if (verifyHintMismatch(parsed, algorithm)) {
			resolve(false);
			return promise;
		}
		const done = (err: Error | null, out: Buffer) => {
			if (err) reject(err);
			else resolve(compareResult(parsed, out));
		};
		if (parsed.kind === "bcrypt") {
			native.bcrypt(bcryptPassword(inputs.pw), parsed.salt, parsed.cost, done);
		} else if (parsed.salt.byteLength < 8 || parsed.expected.byteLength < 4) {
			resolve(false);
		} else {
			native.argon2(
				parsed.kind,
				inputs.pw,
				parsed.salt,
				null,
				null,
				parsed.lanes,
				parsed.memory,
				parsed.passes,
				parsed.expected.byteLength,
				done,
			);
		}
	} catch (e) {
		reject(e);
	}
	return promise;
}

export const password = {
	hash: hashAsync,
	hashSync,
	verify: verifyAsync,
	verifySync,
};
