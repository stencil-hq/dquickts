// Web Crypto API: the global `crypto` (Crypto), CryptoKey and SubtleCrypto (port of Node's
// lib/internal/crypto/webcrypto.js + the CryptoKey parts of keys.js).
//
// SubtleCrypto algorithms live in `internal:crypto/subtle`.
import * as subtleModule from "internal:crypto/subtle";
import * as native from "qbun:crypto";
import { codedError, invalidArgType } from "internal:errors";
import type { KeyObject } from "internal:crypto/keys";

type KeyType = "secret" | "public" | "private";

/** A CryptoKey [[algorithm]] slot (KeyAlgorithm and its subtypes). */
export interface KeyAlgorithm {
	name: string;
	length?: number;
	hash?: { name: string };
	namedCurve?: string;
	modulusLength?: number;
	publicExponent?: Uint8Array;
	[member: string]: unknown;
}

interface KeySlots {
	readonly type: KeyType;
	readonly extractable: boolean;
	/** Null-prototype internal copy; never handed to user code. */
	readonly algorithm: KeyAlgorithm;
	/** Canonical-order usages (internal copy). */
	readonly usages: readonly string[];
	readonly keyObject: KeyObject;
	/** Cached objects returned by the public `algorithm` / `usages` getters. */
	publicAlgorithm: KeyAlgorithm | undefined;
	publicUsages: string[] | undefined;
}

type SubtleMethod =
	| "digest"
	| "generateKey"
	| "deriveKey"
	| "deriveBits"
	| "importKey"
	| "exportKey"
	| "wrapKey"
	| "unwrapKey"
	| "sign"
	| "verify"
	| "encrypt"
	| "decrypt"
	| "getPublicKey"
	| "encapsulateBits"
	| "encapsulateKey"
	| "decapsulateBits"
	| "decapsulateKey";

interface RandomNatives {
	randomFill(target: ArrayBufferView, offset: number, size: number): void;
}

type InspectFn = (value: unknown, options: object) => string;

const kInspect = Symbol.for("nodejs.util.inspect.custom");

// Canonical usage order (Node's kCanonicalUsageOrder): CryptoKey#usages lists usages in this order.
const kUsageOrder: readonly string[] = [
	"encrypt",
	"decrypt",
	"sign",
	"verify",
	"deriveKey",
	"deriveBits",
	"wrapKey",
	"unwrapKey",
	"encapsulateKey",
	"encapsulateBits",
	"decapsulateKey",
	"decapsulateBits",
];

function illegalConstructor(): Error {
	return codedError("ERR_ILLEGAL_CONSTRUCTOR", "Illegal constructor", TypeError);
}

function invalidThis(type: string): Error {
	return codedError("ERR_INVALID_THIS", `Value of "this" must be of type ${type}`, TypeError);
}

function randomFill(target: ArrayBufferView): void {
	(native as unknown as RandomNatives).randomFill(target, 0, target.byteLength);
}

// --- CryptoKey ----------------------------------------------------------------------------------

function cloneInternalAlgorithm(raw: KeyAlgorithm): KeyAlgorithm {
	const cloned = Object.assign(Object.create(null) as KeyAlgorithm, raw);
	if (cloned.hash !== undefined) cloned.hash = Object.assign(Object.create(null) as { name: string }, cloned.hash);
	if (cloned.publicExponent !== undefined) cloned.publicExponent = new Uint8Array(cloned.publicExponent);
	return cloned;
}

function clonePublicAlgorithm(raw: KeyAlgorithm): KeyAlgorithm {
	const cloned: KeyAlgorithm = { ...raw };
	if (cloned.hash !== undefined) cloned.hash = { ...cloned.hash };
	if (cloned.publicExponent !== undefined) cloned.publicExponent = new Uint8Array(cloned.publicExponent);
	return cloned;
}

let allowKeyConstruct = false;
let slotsOf: (key: unknown) => KeySlots | undefined;
let makeKey: (slots: KeySlots) => CryptoKey;

function keySlots(key: unknown): KeySlots {
	const slots = slotsOf(key);
	if (slots === undefined) throw invalidThis("CryptoKey");
	return slots;
}

export class CryptoKey {
	#slots: KeySlots | undefined;

	constructor() {
		if (!allowKeyConstruct) throw illegalConstructor();
	}

	static {
		slotsOf = (key: unknown): KeySlots | undefined => {
			if (key === null || typeof key !== "object" || !(#slots in key)) return undefined;
			return (key as CryptoKey).#slots;
		};
		makeKey = (slots: KeySlots): CryptoKey => {
			allowKeyConstruct = true;
			let key: CryptoKey;
			try {
				key = new CryptoKey();
			} finally {
				allowKeyConstruct = false;
			}
			key.#slots = slots;
			return key;
		};
	}

	get type(): KeyType {
		return keySlots(this).type;
	}

	get extractable(): boolean {
		return keySlots(this).extractable;
	}

	get algorithm(): KeyAlgorithm {
		const slots = keySlots(this);
		slots.publicAlgorithm ??= clonePublicAlgorithm(slots.algorithm);
		return slots.publicAlgorithm;
	}

	get usages(): string[] {
		const slots = keySlots(this);
		slots.publicUsages ??= [...slots.usages];
		return slots.publicUsages;
	}

	[kInspect](depth: number, options: { depth?: number | null }, inspect: InspectFn): unknown {
		const slots = slotsOf(this);
		// The prototype itself (and depth exhaustion) falls back to default inspection.
		if (depth < 0 || slots === undefined) return this;
		const opts = { ...options, depth: options.depth == null ? null : options.depth - 1 };
		return `CryptoKey ${inspect(
			{
				type: slots.type,
				extractable: slots.extractable,
				algorithm: clonePublicAlgorithm(slots.algorithm),
				usages: [...slots.usages],
			},
			opts,
		)}`;
	}
}

Object.defineProperties(CryptoKey.prototype, {
	type: { enumerable: true },
	extractable: { enumerable: true },
	algorithm: { enumerable: true },
	usages: { enumerable: true },
	[Symbol.toStringTag]: { value: "CryptoKey", configurable: true, enumerable: false, writable: false },
});

export function isCryptoKey(value: unknown): value is CryptoKey {
	return slotsOf(value) !== undefined;
}

/** Create a CryptoKey wrapping `keyObject`. Usages are not validated (callers do); unknown ones are dropped. */
export function createCryptoKey(
	keyObject: KeyObject,
	algorithm: KeyAlgorithm,
	extractable: boolean,
	usages: Iterable<string>,
): CryptoKey {
	const requested = new Set(usages);
	return makeKey({
		type: keyObject.type as KeyType,
		extractable: !!extractable,
		algorithm: cloneInternalAlgorithm(algorithm),
		usages: kUsageOrder.filter((usage) => requested.has(usage)),
		keyObject,
		publicAlgorithm: undefined,
		publicUsages: undefined,
	});
}

/** The KeyObject held by a CryptoKey (KeyObject.from). */
export function getCryptoKeyObject(key: CryptoKey): KeyObject {
	const slots = slotsOf(key);
	if (slots === undefined) throw invalidArgType("key", "an instance of CryptoKey", key);
	return slots.keyObject;
}

/** Node's KeyObject#toCryptoKey: validates algorithm/usages like importKey with a KeyObject. */
export function keyObjectToCryptoKey(
	keyObject: KeyObject,
	algorithm: unknown,
	extractable: unknown,
	usages: unknown,
): CryptoKey {
	return subtleModule.keyObjectToCryptoKey(keyObject, algorithm, extractable, usages);
}

// Internal-slot accessors for internal:crypto/subtle (callers have already checked isCryptoKey).
export function getCryptoKeyType(key: CryptoKey): KeyType {
	return keySlots(key).type;
}

export function getCryptoKeyExtractable(key: CryptoKey): boolean {
	return keySlots(key).extractable;
}

export function getCryptoKeyAlgorithm(key: CryptoKey): KeyAlgorithm {
	return keySlots(key).algorithm;
}

export function getCryptoKeyUsages(key: CryptoKey): readonly string[] {
	return keySlots(key).usages;
}

export function hasCryptoKeyUsage(key: CryptoKey, usage: string): boolean {
	return keySlots(key).usages.includes(usage);
}

// --- SubtleCrypto -------------------------------------------------------------------------------

// WebCrypto methods return promises, including for synchronous validation failures.
function callSubtle(method: SubtleMethod, receiver: unknown, args: ArrayLike<unknown>): Promise<unknown> {
	try {
		if (receiver !== subtle) throw invalidThis("SubtleCrypto");
		return Promise.resolve(subtleModule[method](args));
	} catch (err) {
		return Promise.reject(err);
	}
}

export class SubtleCrypto {
	constructor() {
		throw illegalConstructor();
	}

	// Parameters give each method Node's `length`; the implementations read `arguments`.
	static supports(operation: unknown, algorithm: unknown, lengthOrAdditionalAlgorithm: unknown = null): boolean {
		return subtleModule.supports(arguments);
	}
}

function encrypt(this: unknown, algorithm: unknown, key: unknown, data: unknown): Promise<unknown> {
	return callSubtle("encrypt", this, arguments);
}
function decrypt(this: unknown, algorithm: unknown, key: unknown, data: unknown): Promise<unknown> {
	return callSubtle("decrypt", this, arguments);
}
function sign(this: unknown, algorithm: unknown, key: unknown, data: unknown): Promise<unknown> {
	return callSubtle("sign", this, arguments);
}
function verify(this: unknown, algorithm: unknown, key: unknown, signature: unknown, data: unknown): Promise<unknown> {
	return callSubtle("verify", this, arguments);
}
function digest(this: unknown, algorithm: unknown, data: unknown): Promise<unknown> {
	return callSubtle("digest", this, arguments);
}
function generateKey(this: unknown, algorithm: unknown, extractable: unknown, keyUsages: unknown): Promise<unknown> {
	return callSubtle("generateKey", this, arguments);
}
function deriveKey(
	this: unknown,
	algorithm: unknown,
	baseKey: unknown,
	derivedKeyType: unknown,
	extractable: unknown,
	keyUsages: unknown,
): Promise<unknown> {
	return callSubtle("deriveKey", this, arguments);
}
function deriveBits(this: unknown, algorithm: unknown, baseKey: unknown, length: unknown = null): Promise<unknown> {
	return callSubtle("deriveBits", this, arguments);
}
function importKey(
	this: unknown,
	format: unknown,
	keyData: unknown,
	algorithm: unknown,
	extractable: unknown,
	keyUsages: unknown,
): Promise<unknown> {
	return callSubtle("importKey", this, arguments);
}
function exportKey(this: unknown, format: unknown, key: unknown): Promise<unknown> {
	return callSubtle("exportKey", this, arguments);
}
function wrapKey(
	this: unknown,
	format: unknown,
	key: unknown,
	wrappingKey: unknown,
	wrapAlgorithm: unknown,
): Promise<unknown> {
	return callSubtle("wrapKey", this, arguments);
}
function unwrapKey(
	this: unknown,
	format: unknown,
	wrappedKey: unknown,
	unwrappingKey: unknown,
	unwrapAlgorithm: unknown,
	unwrappedKeyAlgorithm: unknown,
	extractable: unknown,
	keyUsages: unknown,
): Promise<unknown> {
	return callSubtle("unwrapKey", this, arguments);
}
function getPublicKey(this: unknown, key: unknown, keyUsages: unknown): Promise<unknown> {
	return callSubtle("getPublicKey", this, arguments);
}
function encapsulateBits(this: unknown, encapsulationAlgorithm: unknown, encapsulationKey: unknown): Promise<unknown> {
	return callSubtle("encapsulateBits", this, arguments);
}
function encapsulateKey(
	this: unknown,
	encapsulationAlgorithm: unknown,
	encapsulationKey: unknown,
	sharedKeyAlgorithm: unknown,
	extractable: unknown,
	keyUsages: unknown,
): Promise<unknown> {
	return callSubtle("encapsulateKey", this, arguments);
}
function decapsulateBits(
	this: unknown,
	decapsulationAlgorithm: unknown,
	decapsulationKey: unknown,
	ciphertext: unknown,
): Promise<unknown> {
	return callSubtle("decapsulateBits", this, arguments);
}
function decapsulateKey(
	this: unknown,
	decapsulationAlgorithm: unknown,
	decapsulationKey: unknown,
	ciphertext: unknown,
	sharedKeyAlgorithm: unknown,
	extractable: unknown,
	keyUsages: unknown,
): Promise<unknown> {
	return callSubtle("decapsulateKey", this, arguments);
}

function methodDescriptor(value: unknown): PropertyDescriptor {
	return { value, enumerable: true, configurable: true, writable: true };
}

Object.defineProperties(SubtleCrypto.prototype, {
	[Symbol.toStringTag]: { value: "SubtleCrypto", configurable: true, enumerable: false, writable: false },
	encrypt: methodDescriptor(encrypt),
	decrypt: methodDescriptor(decrypt),
	sign: methodDescriptor(sign),
	verify: methodDescriptor(verify),
	digest: methodDescriptor(digest),
	generateKey: methodDescriptor(generateKey),
	deriveKey: methodDescriptor(deriveKey),
	deriveBits: methodDescriptor(deriveBits),
	importKey: methodDescriptor(importKey),
	exportKey: methodDescriptor(exportKey),
	wrapKey: methodDescriptor(wrapKey),
	unwrapKey: methodDescriptor(unwrapKey),
	getPublicKey: methodDescriptor(getPublicKey),
	encapsulateBits: methodDescriptor(encapsulateBits),
	encapsulateKey: methodDescriptor(encapsulateKey),
	decapsulateBits: methodDescriptor(decapsulateBits),
	decapsulateKey: methodDescriptor(decapsulateKey),
});
Object.defineProperty(SubtleCrypto, "supports", { enumerable: true });

const subtle = Object.create(SubtleCrypto.prototype) as SubtleCrypto;

// --- Crypto -------------------------------------------------------------------------------------

export class Crypto {
	constructor() {
		throw illegalConstructor();
	}

	get subtle(): SubtleCrypto {
		if (this !== crypto) throw invalidThis("Crypto");
		return subtle;
	}
}

export const crypto = Object.create(Crypto.prototype) as Crypto;

const typedArrayTag = Object.getOwnPropertyDescriptor(
	Object.getPrototypeOf(Uint8Array.prototype) as object,
	Symbol.toStringTag,
)?.get as (this: unknown) => string | undefined;

const kIntegerTypedArrays: Record<string, true> = {
	Int8Array: true,
	Uint8Array: true,
	Uint8ClampedArray: true,
	Int16Array: true,
	Uint16Array: true,
	Int32Array: true,
	Uint32Array: true,
	BigInt64Array: true,
	BigUint64Array: true,
};

// Web Crypto alternative to randomFillSync(): integer typed arrays only, at most 65536 bytes.
function getRandomValues(this: unknown, array: unknown): unknown {
	if (this !== crypto) throw invalidThis("Crypto");
	const prefix = "Failed to execute 'getRandomValues' on 'Crypto'";
	if (arguments.length < 1) {
		throw codedError(
			"ERR_MISSING_ARGS",
			`${prefix}: 1 argument required, but only ${arguments.length} present.`,
			TypeError,
		);
	}
	if (!ArrayBuffer.isView(array)) {
		throw codedError("ERR_INVALID_ARG_TYPE", `${prefix}: 1st argument is not an ArrayBufferView.`, TypeError);
	}
	if (Object.prototype.toString.call(array.buffer) === "[object SharedArrayBuffer]") {
		throw codedError(
			"ERR_INVALID_ARG_TYPE",
			`${prefix}: 1st argument is a view on a SharedArrayBuffer, which is not allowed.`,
			TypeError,
		);
	}
	const tag = typedArrayTag.call(array);
	if (tag === undefined || kIntegerTypedArrays[tag] !== true) {
		// The Web Crypto API and web platform tests expect a TypeMismatchError DOMException here.
		throw new DOMException("The data argument must be an integer-type TypedArray", "TypeMismatchError");
	}
	if (array.byteLength > 65536) {
		throw new DOMException("The requested length exceeds 65,536 bytes", "QuotaExceededError");
	}
	randomFill(array);
	return array;
}

// RFC 4122 version 4 UUIDs. Random data is generated for kUuidBatch UUIDs at a time.
const kUuidBatch = 128;
let uuidData: Uint8Array | undefined;
let uuidIndex = 0;
let hexBytes: string[] | undefined;

function randomUUID(this: unknown): string {
	if (this !== crypto) throw invalidThis("Crypto");
	if (hexBytes === undefined) {
		hexBytes = new Array<string>(256);
		for (let i = 0; i < 256; i++) hexBytes[i] = i.toString(16).padStart(2, "0");
	}
	if (uuidData === undefined) uuidData = new Uint8Array(16 * kUuidBatch);
	if (uuidIndex === 0) randomFill(uuidData);
	const b = uuidData;
	const o = uuidIndex * 16;
	uuidIndex = (uuidIndex + 1) % kUuidBatch;
	const h = hexBytes;
	return (
		`${h[b[o]]}${h[b[o + 1]]}${h[b[o + 2]]}${h[b[o + 3]]}-${h[b[o + 4]]}${h[b[o + 5]]}-` +
		`${h[(b[o + 6] & 0x0f) | 0x40]}${h[b[o + 7]]}-${h[(b[o + 8] & 0x3f) | 0x80]}${h[b[o + 9]]}-` +
		`${h[b[o + 10]]}${h[b[o + 11]]}${h[b[o + 12]]}${h[b[o + 13]]}${h[b[o + 14]]}${h[b[o + 15]]}`
	);
}

Object.defineProperties(Crypto.prototype, {
	[Symbol.toStringTag]: { value: "Crypto", configurable: true, enumerable: false, writable: false },
	subtle: { enumerable: true },
	getRandomValues: methodDescriptor(getRandomValues),
	randomUUID: methodDescriptor(randomUUID),
});
