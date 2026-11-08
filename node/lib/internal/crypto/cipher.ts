// node:crypto ciphers (port of Node's lib/internal/crypto/cipher.js): Cipheriv, Decipheriv, getCipherInfo,
// getCiphers and the RSA publicEncrypt / privateDecrypt / privateEncrypt / publicDecrypt functions.
import { Transform } from "node:stream";
import { StringDecoder } from "node:string_decoder";
import { native, type CipherInfo, type NativeCipher } from "internal:crypto/native";
import {
		cachedResult, errState, errType, filterDuplicateStrings, getStringOption, getUIntOption,
	isArrayBufferView, normalizeEncoding, toBytes, validateInt32, validateObject, validateString,
} from "internal:crypto/util";
import { prepareSecretKey, preparePrivateKey, preparePublicOrPrivateKey } from "internal:crypto/keys";
import { codedError } from "internal:errors";

const kHandle = Symbol("kHandle");
const kFinal = Symbol("kFinal");
const kDecoder = Symbol("kDecoder");

interface Decoder {
	write(buf: Buffer): string;
	end(buf?: Buffer): string;
	readonly encoding: string;
}

function decoderFor(current: Decoder | null, encoding: string): Decoder {
	const enc = normalizeEncoding(encoding);
	if (enc === undefined) throw codedError("ERR_UNKNOWN_ENCODING", `Unknown encoding: ${encoding}`, TypeError);
	if (current) {
		if (normalizeEncoding(current.encoding) !== enc) {
			throw codedError("ERR_CRYPTO_INVALID_STATE", "Cannot change encoding", Error);
		}
		return current;
	}
	return new StringDecoder(enc);
}

class CipherBase extends Transform {
	declare [kHandle]: NativeCipher;
	declare [kFinal]: boolean;
	declare [kDecoder]: Decoder | null;

	constructor(cipher: unknown, key: unknown, iv: unknown, options: unknown, encrypt: boolean) {
		super(options);
		validateString(cipher, "cipher");
		const encoding = getStringOption(options, "encoding");
		const keyBytes = prepareSecretKey(key, encoding);
		const ivBytes = iv === null ? null : toBytes(iv, "iv");
		const authTagLength = getUIntOption(options, "authTagLength");
		this[kHandle] = native.cipherNew(cipher, keyBytes, ivBytes, encrypt, authTagLength);
		this[kFinal] = false;
		this[kDecoder] = null;
	}

	_transform(chunk: Uint8Array | string, encoding: string, callback: () => void): void {
		(this as unknown as { push(c: Buffer | string): void }).push(this.update(chunk, encoding));
		callback();
	}

	_flush(callback: (err?: Error) => void): void {
		try {
			(this as unknown as { push(c: Buffer | string): void }).push(this.final());
		} catch (e) {
			callback(e as Error);
			return;
		}
		callback();
	}

	update(data: unknown, inputEncoding?: unknown, outputEncoding?: unknown): Buffer | string {
		let bytes: ArrayBufferView | ArrayBuffer | SharedArrayBuffer;
		if (typeof data === "string") {
			bytes = toBytes(data, "data", inputEncoding);
		} else if (!isArrayBufferView(data)) {
			throw errType("data", "of type string or an instance of Buffer, TypedArray, or DataView", data);
		} else {
			bytes = data;
		}
		if (this[kFinal]) throw codedError("ERR_CRYPTO_INVALID_STATE", "Trying to add data in unsupported state", Error);
		const ret = this[kHandle].update(bytes);
		if (outputEncoding && outputEncoding !== "buffer") {
			this[kDecoder] = decoderFor(this[kDecoder], String(outputEncoding));
			return this[kDecoder].write(ret);
		}
		return ret;
	}

	final(outputEncoding?: unknown): Buffer | string {
		if (this[kFinal]) throw codedError("ERR_CRYPTO_INVALID_STATE", "Unsupported state", Error);
		this[kFinal] = true;
		const ret = this[kHandle].final();
		if (outputEncoding && outputEncoding !== "buffer") {
			this[kDecoder] = decoderFor(this[kDecoder], String(outputEncoding));
			return this[kDecoder].end(ret);
		}
		return ret;
	}

	setAutoPadding(ap?: unknown): this {
		if (this[kFinal] || !this[kHandle].setAutoPadding(ap === undefined ? true : !!ap)) throw errState("setAutoPadding");
		return this;
	}

	setAAD(aadbuf: unknown, options?: unknown): this {
		const encoding = getStringOption(options, "encoding");
		const plaintextLength = getUIntOption(options, "plaintextLength");
		const aad = toBytes(aadbuf, "aadbuf", encoding);
		if (this[kFinal] || !this[kHandle].setAAD(aad, plaintextLength)) throw errState("setAAD");
		return this;
	}
}

export class Cipheriv extends CipherBase {
	constructor(cipher: unknown, key: unknown, iv: unknown, options?: unknown) {
		super(cipher, key, iv, options, true);
	}

	getAuthTag(): Buffer {
		const ret = this[kHandle].getAuthTag();
		if (ret === undefined) throw errState("getAuthTag");
		return ret;
	}
}

export class Decipheriv extends CipherBase {
	constructor(cipher: unknown, key: unknown, iv: unknown, options?: unknown) {
		super(cipher, key, iv, options, false);
	}

	setAuthTag(tagbuf: unknown, encoding?: unknown): this {
		const tag = toBytes(tagbuf, "buffer", encoding);
		if (this[kFinal] || !this[kHandle].setAuthTag(tag)) throw errState("setAuthTag");
		return this;
	}
}

export function createCipheriv(cipher: unknown, key: unknown, iv: unknown, options?: unknown): Cipheriv {
	return new Cipheriv(cipher, key, iv, options);
}

export function createDecipheriv(cipher: unknown, key: unknown, iv: unknown, options?: unknown): Decipheriv {
	return new Decipheriv(cipher, key, iv, options);
}

export const getCiphers = cachedResult(() => filterDuplicateStrings(native.getCiphers(), true));

export function getCipherInfo(nameOrNid: unknown, options?: unknown): CipherInfo | undefined {
	if (typeof nameOrNid !== "string" && typeof nameOrNid !== "number") {
		throw errType("nameOrNid", "of type string or number", nameOrNid);
	}
	if (typeof nameOrNid === "number") validateInt32(nameOrNid, "nameOrNid");
	let keyLength = -1;
	let ivLength = -1;
	if (options !== undefined) {
		validateObject(options, "options");
		if (options.keyLength !== undefined) {
			validateInt32(options.keyLength, "options.keyLength");
			keyLength = options.keyLength;
		}
		if (options.ivLength !== undefined) {
			validateInt32(options.ivLength, "options.ivLength");
			ivLength = options.ivLength;
		}
	}
	return native.cipherInfo(nameOrNid, keyLength, ivLength);
}

// ------------------------------------------------------------- RSA crypt

const RSA_PKCS1_PADDING = 1;
const RSA_PKCS1_OAEP_PADDING = 4;

function rsaFunctionFor(op: number, defaultPadding: number, keyType: "public" | "private") {
	return (options: unknown, buffer: unknown): Buffer => {
		const pkey = keyType === "private" ? preparePrivateKey(options) : preparePublicOrPrivateKey(options);
		const opts = (
			options !== null && typeof options === "object" && !isArrayBufferView(options) ? options : {}
		) as Record<string, unknown>;
		const padding = (opts.padding as number) || defaultPadding;
		const { oaepHash, encoding } = opts;
		let label: ArrayBufferView | ArrayBuffer | SharedArrayBuffer | null = null;
		if (oaepHash !== undefined) validateString(oaepHash, "key.oaepHash");
		if (opts.oaepLabel !== undefined) label = toBytes(opts.oaepLabel, "key.oaepLabel", encoding);
		const data = toBytes(buffer, "buffer", encoding);
		return native.pkeyCrypt(op, pkey, data, padding, (oaepHash as string | undefined) ?? null, label);
	};
}

export const publicEncrypt = rsaFunctionFor(0, RSA_PKCS1_OAEP_PADDING, "public");
export const privateDecrypt = rsaFunctionFor(1, RSA_PKCS1_OAEP_PADDING, "private");
export const privateEncrypt = rsaFunctionFor(2, RSA_PKCS1_PADDING, "private");
export const publicDecrypt = rsaFunctionFor(3, RSA_PKCS1_PADDING, "public");
