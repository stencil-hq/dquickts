// node:crypto X.509 (port of Node's lib/internal/crypto/x509.js and certificate.js): X509Certificate,
// parsePKCS12 and the legacy SPKAC Certificate class.
import { native, type NativeX509, type PKey, type X509Info } from "internal:crypto/native";
import {
	errType,
	errValue,
	isArrayBufferView,
	toBytes,
	validateBoolean,
	validateObject,
	validateString,
} from "internal:crypto/util";
import { isKeyObject, kHandle, PrivateKeyObject, PublicKeyObject, type KeyObject } from "internal:crypto/keys";

const kNative = Symbol("kNative");
const kInfo = Symbol("kInfo");
const kFromNative = Symbol("kFromNative");

interface CheckFlags {
	subject: "default" | "always" | "never";
	wildcards: boolean;
	partialWildcards: boolean;
	multiLabelWildcards: boolean;
	singleLabelSubdomains: boolean;
}

function getFlags(options: unknown = {}): CheckFlags {
	validateObject(options, "options");
	const {
		subject = "default",
		wildcards = true,
		partialWildcards = true,
		multiLabelWildcards = false,
		singleLabelSubdomains = false,
	} = options;
	if (subject !== "default" && subject !== "always" && subject !== "never") throw errValue("options.subject", subject);
	validateBoolean(wildcards, "options.wildcards");
	validateBoolean(partialWildcards, "options.partialWildcards");
	validateBoolean(multiLabelWildcards, "options.multiLabelWildcards");
	validateBoolean(singleLabelSubdomains, "options.singleLabelSubdomains");
	return { subject, wildcards, partialWildcards, multiLabelWildcards, singleLabelSubdomains };
}

/** "DNS:a, IP Address:1.2.3.4" → [["DNS", "a"], ["IP Address", "1.2.3.4"]] */
function altNames(san: string | undefined): [string, string][] {
	if (!san) return [];
	const out: [string, string][] = [];
	for (const part of san.split(/, (?=[A-Za-z][A-Za-z ]*:)/)) {
		const i = part.indexOf(":");
		if (i > 0) out.push([part.slice(0, i), part.slice(i + 1)]);
	}
	return out;
}

function subjectValues(info: X509Info, key: string): string[] {
	const v = info.subjectObject[key];
	if (v === undefined) return [];
	return Array.isArray(v) ? v : [v];
}

/** OpenSSL-style host name matching of a certificate name pattern. */
function hostMatches(pattern: string, host: string, flags: CheckFlags): boolean {
	const p = pattern.toLowerCase().replace(/\.$/, "");
	const h = host.toLowerCase().replace(/\.$/, "");
	if (!p || !h) return false;
	if (p === h) return true;
	if (flags.singleLabelSubdomains && h.startsWith(".")) {
		const rest = p.endsWith(h) ? p.slice(0, p.length - h.length) : "";
		return rest.length > 0 && !rest.includes(".");
	}
	const star = p.indexOf("*");
	if (star < 0 || !flags.wildcards) return false;
	const firstDot = p.indexOf(".");
	// the wildcard must be in the leftmost label, which is not an IDN A-label, with at least two labels after it
	if (firstDot < 0 || star > firstDot || p.indexOf("*", star + 1) >= 0) return false;
	if (p.slice(firstDot + 1).split(".").length < 2) return false;
	const label = p.slice(0, firstDot);
	if (label !== "*" && (!flags.partialWildcards || label.startsWith("xn--"))) return false;
	const suffix = p.slice(firstDot);
	if (!h.endsWith(suffix)) return false;
	const hostLabel = h.slice(0, h.length - suffix.length);
	if (!hostLabel) return false;
	if (hostLabel.includes(".") && !flags.multiLabelWildcards) return false;
	const prefix = label.slice(0, star);
	const after = label.slice(star + 1);
	return hostLabel.length >= prefix.length + after.length && hostLabel.startsWith(prefix) && hostLabel.endsWith(after);
}

function parseIPv4(s: string): number[] | null {
	const parts = s.split(".");
	if (parts.length !== 4) return null;
	const out: number[] = [];
	for (const p of parts) {
		if (!/^\d{1,3}$/.test(p) || Number(p) > 255) return null;
		out.push(Number(p));
	}
	return out;
}

function parseIPv6(s: string): number[] | null {
	let str = s;
	const tail: number[] = [];
	const lastColon = str.lastIndexOf(":");
	if (str.includes(".")) {
		const v4 = parseIPv4(str.slice(lastColon + 1));
		if (!v4) return null;
		tail.push((v4[0] << 8) | v4[1], (v4[2] << 8) | v4[3]);
		str = `${str.slice(0, lastColon)}:0:0`;
	}
	const halves = str.split("::");
	if (halves.length > 2) return null;
	const head = halves[0] ? halves[0].split(":") : [];
	const rest = halves.length === 2 && halves[1] ? halves[1].split(":") : [];
	const total = head.length + rest.length;
	if ((halves.length === 1 && total !== 8) || total > 8) return null;
	const groups = [...head, ...new Array<string>(8 - total).fill("0"), ...rest];
	const words: number[] = [];
	for (const g of groups) {
		if (!/^[0-9a-fA-F]{1,4}$/.test(g)) return null;
		words.push(parseInt(g, 16));
	}
	if (tail.length) words.splice(6, 2, tail[0], tail[1]);
	const bytes: number[] = [];
	for (const w of words) bytes.push(w >> 8, w & 0xff);
	return bytes;
}

function parseIP(s: string): number[] | null {
	return parseIPv4(s) ?? (s.includes(":") ? parseIPv6(s) : null);
}

function legacyInfoAccess(text: string | undefined): Record<string, string[]> | undefined {
	if (!text) return undefined;
	const out: Record<string, string[]> = Object.create(null);
	for (const line of text.split("\n")) {
		const i = line.indexOf(":");
		if (i < 0) continue;
		const key = line.slice(0, i);
		(out[key] ??= []).push(line.slice(i + 1));
	}
	return out;
}

const kNistCurves: Record<string, string> = {
	prime256v1: "P-256",
	secp384r1: "P-384",
	secp521r1: "P-521",
	secp224r1: "P-224",
	prime192v1: "P-192",
};

export class X509Certificate {
	declare [kNative]: NativeX509;
	declare [kInfo]: X509Info | undefined;

	constructor(buffer: unknown) {
		if ((buffer as Record<symbol, unknown> | null)?.[kFromNative]) {
			this[kNative] = (buffer as Record<symbol, NativeX509>)[kFromNative];
		} else {
			const data = typeof buffer === "string" ? Buffer.from(buffer) : buffer;
			if (!isArrayBufferView(data))
				throw errType("buffer", "of type string or an instance of Buffer, TypedArray, or DataView", buffer);
			this[kNative] = native.x509Parse(data);
		}
		this[kInfo] = undefined;
	}

	private get info(): X509Info {
		return (this[kInfo] ??= this[kNative].info());
	}

	get subject(): string {
		return this.info.subject;
	}
	get subjectAltName(): string | undefined {
		return this.info.subjectAltName;
	}
	get issuer(): string {
		return this.info.issuer;
	}
	get issuerCertificate(): undefined {
		return undefined;
	}
	get infoAccess(): string | undefined {
		return this.info.infoAccess;
	}
	get validFrom(): string {
		return this.info.validFrom;
	}
	get validTo(): string {
		return this.info.validTo;
	}
	get validFromDate(): Date {
		return new Date(this.info.validFromDate);
	}
	get validToDate(): Date {
		return new Date(this.info.validToDate);
	}
	get fingerprint(): string {
		return this.info.fingerprint;
	}
	get fingerprint256(): string {
		return this.info.fingerprint256;
	}
	get fingerprint512(): string {
		return this.info.fingerprint512;
	}
	get keyUsage(): string[] | undefined {
		return this.info.keyUsage?.slice();
	}
	get serialNumber(): string {
		return this.info.serialNumber;
	}
	get raw(): Buffer {
		return this.info.raw;
	}
	get publicKey(): KeyObject {
		return new PublicKeyObject(this[kNative].publicKey());
	}
	get ca(): boolean {
		return this.info.ca;
	}
	get signatureAlgorithm(): string | undefined {
		return this.info.signatureAlgorithm;
	}
	get signatureAlgorithmOid(): string {
		return this.info.signatureAlgorithmOid;
	}
	get version(): number {
		return this.info.version;
	}
	get tbs(): Buffer {
		return Buffer.from(this.info.tbs);
	}
	get signature(): { algorithm: { oid: string; parameters?: Buffer }; value: Buffer } {
		const s = this.info.signature;
		const algorithm: { oid: string; parameters?: Buffer } = { oid: s.algorithm.oid };
		if (s.algorithm.parameters) algorithm.parameters = Buffer.from(s.algorithm.parameters);
		return { algorithm, value: Buffer.from(s.value) };
	}
	get extensions(): { oid: string; critical: boolean; value: Buffer }[] {
		return this.info.extensions.map((e) => ({ oid: e.oid, critical: e.critical, value: Buffer.from(e.value) }));
	}
	get subjectName(): { raw: Buffer; rdns: { oid: string; value?: string }[][] } {
		const n = this.info.subjectName;
		return { raw: Buffer.from(n.raw), rdns: n.rdns.map((rdn) => rdn.map((a) => ({ ...a }))) };
	}
	get issuerName(): { raw: Buffer; rdns: { oid: string; value?: string }[][] } {
		const n = this.info.issuerName;
		return { raw: Buffer.from(n.raw), rdns: n.rdns.map((rdn) => rdn.map((a) => ({ ...a }))) };
	}

	toString(): string {
		return this.info.pem;
	}

	toJSON(): string {
		return this.info.pem;
	}

	checkHost(name: unknown, options?: unknown): string | undefined {
		validateString(name, "name");
		const flags = getFlags(options);
		const dns = altNames(this.info.subjectAltName)
			.filter(([k]) => k === "DNS")
			.map(([, v]) => v);
		for (const pattern of dns) if (hostMatches(pattern, name, flags)) return pattern;
		if (flags.subject === "always" || (flags.subject === "default" && dns.length === 0)) {
			for (const cn of subjectValues(this.info, "CN")) if (hostMatches(cn, name, flags)) return cn;
		}
		return undefined;
	}

	checkEmail(email: unknown, options?: unknown): string | undefined {
		validateString(email, "email");
		const flags = getFlags(options);
		const matches = (candidate: string): boolean => {
			const at = candidate.lastIndexOf("@");
			const at2 = email.lastIndexOf("@");
			if (at < 0 || at2 < 0) return candidate === email;
			return (
				candidate.slice(0, at) === email.slice(0, at2) &&
				candidate.slice(at + 1).toLowerCase() === email.slice(at2 + 1).toLowerCase()
			);
		};
		const emails = altNames(this.info.subjectAltName)
			.filter(([k]) => k === "email")
			.map(([, v]) => v);
		for (const e of emails) if (matches(e)) return e;
		if (flags.subject === "always" || (flags.subject === "default" && emails.length === 0)) {
			for (const e of subjectValues(this.info, "emailAddress")) if (matches(e)) return e;
		}
		return undefined;
	}

	checkIP(ip: unknown): string | undefined {
		validateString(ip, "ip");
		const want = parseIP(ip);
		if (!want) throw errValue("ip", ip);
		for (const [k, v] of altNames(this.info.subjectAltName)) {
			if (k !== "IP Address") continue;
			const got = parseIP(v.trim());
			if (got && got.length === want.length && got.every((b, i) => b === want[i])) return ip;
		}
		return undefined;
	}

	checkIssued(otherCert: unknown): boolean {
		if (!(otherCert instanceof X509Certificate))
			throw errType("otherCert", "an instance of X509Certificate", otherCert);
		return this[kNative].checkIssued(otherCert[kNative]);
	}

	checkPrivateKey(pkey: unknown): boolean {
		if (!isKeyObject(pkey)) throw errType("pkey", "an instance of KeyObject", pkey);
		if (pkey.type !== "private") throw errValue("pkey", pkey);
		return this[kNative].checkPrivateKey(pkey[kHandle] as PKey);
	}

	verify(pkey: unknown): boolean {
		if (!isKeyObject(pkey)) throw errType("pkey", "an instance of KeyObject", pkey);
		if (pkey.type !== "public") throw errValue("pkey", pkey);
		return this[kNative].verify(pkey[kHandle] as PKey);
	}

	toLegacyObject(): Record<string, unknown> {
		const info = this.info;
		const out: Record<string, unknown> = {
			subject: { ...info.subjectObject },
			issuer: { ...info.issuerObject },
		};
		if (info.subjectAltName !== undefined) out.subjectaltname = info.subjectAltName;
		const ia = legacyInfoAccess(info.infoAccess);
		if (ia) out.infoAccess = ia;
		out.ca = info.ca;
		const pkey = this[kNative].publicKey();
		const type = pkey.asymmetricKeyType;
		if (type === "rsa" || type === "rsa-pss") {
			const jwk = pkey.jwk();
			const n = jwk.n as Buffer;
			const e = jwk.e as Buffer;
			out.modulus = Buffer.from(n).toString("hex").toUpperCase();
			out.bits = pkey.bits;
			out.exponent = `0x${BigInt(`0x${Buffer.from(e).toString("hex")}`).toString(16)}`;
			out.pubkey = pkey.export("der", "spki", null, null);
		} else if (type === "ec") {
			out.bits = pkey.bits;
			out.pubkey = pkey.ecPoint("uncompressed");
			const curve = pkey.curve();
			if (curve) {
				out.asn1Curve = curve;
				if (kNistCurves[curve]) out.nistCurve = kNistCurves[curve];
			}
		}
		out.valid_from = info.validFrom;
		out.valid_to = info.validTo;
		out.fingerprint = info.fingerprint;
		out.fingerprint256 = info.fingerprint256;
		out.fingerprint512 = info.fingerprint512;
		if (info.keyUsage) out.ext_key_usage = info.keyUsage.slice();
		out.serialNumber = info.serialNumber;
		out.raw = info.raw;
		return out;
	}

	get [Symbol.toStringTag](): string {
		return "X509Certificate";
	}
}

function fromNative(x: NativeX509): X509Certificate {
	return new X509Certificate({ [kFromNative]: x });
}

export function parsePKCS12(
	bundle: unknown,
	options?: unknown,
): {
	privateKey: KeyObject | null;
	certificate: X509Certificate | null;
	additionalCertificates: X509Certificate[];
} {
	const der = toBytes(bundle, "bundle");
	if (typeof bundle === "string")
		throw errType("bundle", "an instance of ArrayBuffer, Buffer, TypedArray, or DataView", bundle);
	let passphrase: ArrayBuffer | SharedArrayBuffer | ArrayBufferView | null = null;
	if (options !== undefined) {
		validateObject(options, "options");
		if (options.passphrase !== undefined && options.passphrase !== null)
			passphrase = toBytes(options.passphrase, "options.passphrase");
	}
	const r = native.parsePKCS12(der, passphrase);
	return {
		privateKey: r.key ? new PrivateKeyObject(r.key) : null,
		certificate: r.cert ? fromNative(r.cert) : null,
		additionalCertificates: r.ca.map(fromNative),
	};
}

// ------------------------------------------------------------ SPKAC

function verifySpkac(spkac: unknown, encoding?: unknown): boolean {
	return native.spkacVerify(toBytes(spkac, "spkac", encoding));
}

function exportPublicKey(spkac: unknown, encoding?: unknown): Buffer | string {
	return native.spkacPublicKey(toBytes(spkac, "spkac", encoding)) ?? "";
}

function exportChallenge(spkac: unknown, encoding?: unknown): Buffer | string {
	return native.spkacChallenge(toBytes(spkac, "spkac", encoding)) ?? "";
}

export class Certificate {
	static verifySpkac = verifySpkac;
	static exportPublicKey = exportPublicKey;
	static exportChallenge = exportChallenge;
	verifySpkac = verifySpkac;
	exportPublicKey = exportPublicKey;
	exportChallenge = exportChallenge;
}
