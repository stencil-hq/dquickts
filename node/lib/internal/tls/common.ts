// TLS configuration shared by node:tls: the default settings, SecureContext /
// createSecureContext (Node's configSecureContext), the trust store (the
// system's root certificates: Node's bundled Mozilla list is not available,
// rootCertificates lists the certificates of the system bundle), ALPN
// conversion, checkServerIdentity and the peer certificate objects.
// Adapted from Node.js lib/tls.js and lib/internal/tls/{common,secure-context}.js (MIT).

import * as binding from "qbun:tls";
import { Buffer } from "node:buffer";
import process from "node:process";
import { domainToASCII } from "node:url";
import { isIP, isIPv4, isIPv6, parseIPv6, formatIPv6 } from "internal:http/ip";
import {
	codedError,
	invalidArgType,
	invalidArgValue,
	outOfRange,
	validateObject,
	validateString,
	validateNumber,
} from "internal:errors";

// ---------------------------------------------------------------------------
// native binding (node/tls.c)

/** an SSL_CTX */
export interface NativeSecureContext {
	setMinProto(v: number): void;
	setMaxProto(v: number): void;
	getMinProto(): number;
	getMaxProto(): number;
	setOptions(options: number): void;
	setCiphers(list: string): void;
	setCipherSuites(suites: string): void;
	addCACert(pem: string | ArrayBufferView): void;
	addRootCerts(store: NativeCertStore): void;
	setAllowPartialTrustChain(): void;
	setCert(pem: string | ArrayBufferView): void;
	setKey(pem: string | ArrayBufferView, passphrase?: string): void;
	setECDHCurve(curve: string): void;
	setSigalgs(sigalgs: string): void;
	setDHParam(dh: string | ArrayBufferView | boolean): string | undefined;
	addCRL(pem: string | ArrayBufferView): void;
	setSessionIdContext(id: string): void;
	setSessionTimeout(seconds: number): void;
	loadPKCS12(pfx: ArrayBufferView, passphrase?: string | ArrayBufferView): void;
	setTicketKeys(keys: ArrayBufferView): void;
	getTicketKeys(): Buffer;
	getCertificate(): Buffer | null;
	getIssuer(): Buffer | null;
	close(): void;
}

/** an X509_STORE */
export interface NativeCertStore {
	addFile(path: string): void;
	addPEM(pem: string | ArrayBufferView): number;
	certificates(): string[];
}

export interface NativeEvent extends Array<unknown> {
	0: "keylog" | "session";
}

/** an SSL on a memory BIO pair */
export interface NativeConnection {
	start(): number;
	receive(data: Uint8Array): number;
	readCleartext(): Buffer | null;
	writeCleartext(data: Uint8Array | string): number;
	takeEncrypted(): Buffer | null;
	shutdown(): number;
	destroy(): void;
	setVerifyMode(requestCert: boolean, rejectUnauthorized: boolean): void;
	setServername(name: string): void;
	getServername(): string | false;
	setALPNProtocols(wire: Uint8Array): void;
	setALPNCallback(cb: ((protocols: Buffer) => number | undefined) | null): void;
	getALPNNegotiatedProtocol(): string | false;
	enableKeylogCallback(): void;
	enableSessionCallbacks(): void;
	enableCertCb(): void;
	certCbDone(): number;
	setSNIContext(ctx: NativeSecureContext): void;
	takeEvents(): NativeEvent[] | undefined;
	verifyError(): (Error & { code: string }) | null;
	hasPeerCertificate(): boolean;
	getPeerCertificate(detailed?: boolean): PeerCertificate | undefined;
	getCertificate(): PeerCertificate | null;
	getProtocol(): string | null;
	getCipher(): CipherNameAndProtocol | undefined;
	getSharedSigalgs(): string[] | undefined;
	getEphemeralKeyInfo(): EphemeralKeyInfo | null;
	getFinished(): Buffer | undefined;
	getPeerFinished(): Buffer | undefined;
	getSession(): Buffer | undefined;
	setSession(session: Uint8Array): void;
	isSessionReused(): boolean;
	getTLSTicket(): Buffer | undefined;
	exportKeyingMaterial(length: number, label: string, context?: Uint8Array): Buffer;
	renegotiate(): void;
	setMaxSendFragment(size: number): boolean;
	enableTrace(): void;
}

interface TlsBinding {
	createSecureContext(minVersion: number, maxVersion: number): NativeSecureContext;
	createConnection(ctx: NativeSecureContext, isServer: boolean): NativeConnection;
	createCertStore(loadSystem: boolean): NativeCertStore;
	getSSLCiphers(): string[];
	defaultPaths(): [string, string, string, string];
	OPENSSL_VERSION: string;
	TLS1_VERSION: number;
	TLS1_1_VERSION: number;
	TLS1_2_VERSION: number;
	TLS1_3_VERSION: number;
	SSL_OP_CIPHER_SERVER_PREFERENCE: number;
	SSL_OP_NO_TICKET: number;
	HANDSHAKE_DONE: number;
	CLEAR: number;
	ENC: number;
	EOF: number;
	EVENTS: number;
	CERTCB: number;
	SECURE: number;
}

// qbun:tls exports exactly these names (node/tls.c)
export const native = binding as unknown as TlsBinding;

// ---------------------------------------------------------------------------
// types

export interface Certificate {
	C?: string;
	ST?: string;
	L?: string;
	O?: string;
	OU?: string;
	CN?: string;
	[key: string]: string | string[] | undefined;
}

export interface PeerCertificate {
	subject: Certificate;
	issuer: Certificate;
	subjectaltname?: string;
	infoAccess?: Record<string, string[]> | string;
	ca: boolean;
	modulus?: string;
	exponent?: string;
	pubkey?: Buffer;
	bits?: number;
	asn1Curve?: string;
	nistCurve?: string;
	valid_from: string;
	valid_to: string;
	validFromDate?: Date;
	validToDate?: Date;
	fingerprint: string;
	fingerprint256: string;
	fingerprint512: string;
	ext_key_usage?: string[];
	serialNumber: string;
	raw: Buffer;
	issuerCertificate?: PeerCertificate;
}

export interface CipherNameAndProtocol {
	name: string;
	standardName: string;
	version: string;
}

export interface EphemeralKeyInfo {
	type?: string;
	name?: string;
	size?: number;
}

type KeyOrCert = string | ArrayBufferView;

export interface SecureContextOptions {
	allowPartialTrustChain?: boolean;
	ca?: KeyOrCert | KeyOrCert[];
	cert?: KeyOrCert | KeyOrCert[];
	sigalgs?: string;
	ciphers?: string;
	clientCertEngine?: string;
	crl?: KeyOrCert | KeyOrCert[];
	dhparam?: KeyOrCert | "auto";
	ecdhCurve?: string;
	honorCipherOrder?: boolean;
	key?: KeyOrCert | (KeyOrCert | { pem: KeyOrCert; passphrase?: string })[];
	privateKeyEngine?: string;
	privateKeyIdentifier?: string;
	maxVersion?: string;
	minVersion?: string;
	passphrase?: string;
	pfx?: KeyOrCert | (KeyOrCert | { buf: KeyOrCert; passphrase?: string })[];
	secureOptions?: number;
	secureProtocol?: string;
	sessionIdContext?: string;
	ticketKeys?: Buffer;
	sessionTimeout?: number;
	singleUse?: boolean;
	certificateCompression?: string[];
	[key: string]: unknown;
}

// ---------------------------------------------------------------------------
// defaults (mutable through the default export of node:tls, like Node)

export const DEFAULT_CIPHERS_LIST =
	"TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256:" +
	"ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:" +
	"ECDHE-ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-SHA256:" +
	"DHE-RSA-AES128-SHA256:ECDHE-RSA-AES256-SHA384:DHE-RSA-AES256-SHA384:ECDHE-RSA-AES256-SHA256:" +
	"DHE-RSA-AES256-SHA256:HIGH:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!MD5:!PSK:!SRP:!CAMELLIA";

function initialMinVersion(): string {
	const argv = process.execArgv ?? [];
	if (argv.includes("--tls-min-v1.0")) return "TLSv1";
	if (argv.includes("--tls-min-v1.1")) return "TLSv1.1";
	if (argv.includes("--tls-min-v1.3")) return "TLSv1.3";
	return "TLSv1.2";
}

function initialMaxVersion(): string {
	const argv = process.execArgv ?? [];
	if (argv.includes("--tls-max-v1.2")) return "TLSv1.2";
	return "TLSv1.3";
}

function initialCiphers(): string {
	const argv = process.execArgv ?? [];
	for (const a of argv) {
		if (a.startsWith("--tls-cipher-list=")) return a.slice("--tls-cipher-list=".length);
	}
	return DEFAULT_CIPHERS_LIST;
}

export const defaults = {
	minVersion: initialMinVersion(),
	maxVersion: initialMaxVersion(),
	ciphers: initialCiphers(),
	ecdhCurve: "auto",
};

export const CLIENT_RENEG_LIMIT = 3;
export const CLIENT_RENEG_WINDOW = 600;

// ---------------------------------------------------------------------------
// errors

export function connResetException(message: string): Error & { code: string } {
	return codedError("ECONNRESET", message);
}

// ---------------------------------------------------------------------------
// trust store

let systemStore: NativeCertStore | null = null;
let rootStore: NativeCertStore | null = null;
let extraStore: NativeCertStore | null = null;
let extraLoaded = false;

function loadExtraCAs(store: NativeCertStore): void {
	const file = process.env.NODE_EXTRA_CA_CERTS;
	if (!file) return;
	try {
		store.addFile(file);
	} catch (err) {
		process.emitWarning?.(`Ignoring extra certs from \`${file}\`, load failed: ${(err as Error).message}`);
	}
}

function getSystemStore(): NativeCertStore {
	if (!systemStore) systemStore = native.createCertStore(true);
	return systemStore;
}

/** the store of the default trust: the system roots plus NODE_EXTRA_CA_CERTS */
export function getRootStore(): NativeCertStore {
	if (!rootStore) {
		if (process.env.NODE_EXTRA_CA_CERTS) {
			rootStore = native.createCertStore(true);
			loadExtraCAs(rootStore);
		} else {
			rootStore = getSystemStore();
		}
	}
	return rootStore;
}

function getExtraStore(): NativeCertStore | null {
	if (!extraLoaded) {
		extraLoaded = true;
		if (process.env.NODE_EXTRA_CA_CERTS) {
			extraStore = native.createCertStore(false);
			loadExtraCAs(extraStore);
		}
	}
	return extraStore;
}

let systemCerts: readonly string[] | null = null;
function systemCertificates(): readonly string[] {
	if (!systemCerts) systemCerts = Object.freeze(getSystemStore().certificates());
	return systemCerts;
}

// rootCertificates: an array filled with the system roots on first use
const rootTarget: string[] = [];
let rootFilled = false;
function fillRoots(): void {
	if (rootFilled) return;
	rootFilled = true;
	for (const pem of systemCertificates()) rootTarget.push(pem);
	Object.freeze(rootTarget);
}

export const rootCertificates: readonly string[] = new Proxy(rootTarget, {
	get(target, key, receiver) {
		fillRoots();
		return Reflect.get(target, key, receiver);
	},
	has(target, key) {
		fillRoots();
		return Reflect.has(target, key);
	},
	ownKeys(target) {
		fillRoots();
		return Reflect.ownKeys(target);
	},
	getOwnPropertyDescriptor(target, key) {
		fillRoots();
		return Reflect.getOwnPropertyDescriptor(target, key);
	},
	set(target, key, value, receiver) {
		fillRoots();
		return Reflect.set(target, key, value, receiver);
	},
	defineProperty(target, key, desc) {
		fillRoots();
		return Reflect.defineProperty(target, key, desc);
	},
	deleteProperty(target, key) {
		fillRoots();
		return Reflect.deleteProperty(target, key);
	},
	isExtensible(target) {
		fillRoots();
		return Reflect.isExtensible(target);
	},
	preventExtensions(target) {
		fillRoots();
		return Reflect.preventExtensions(target);
	},
});

let defaultCerts: readonly string[] | null = null;

/** tls.getCACertificates(type) */
export function getCACertificates(type: string = "default"): readonly string[] {
	validateString(type, "type");
	switch (type) {
		case "default":
			if (!defaultCerts) defaultCerts = Object.freeze(getRootStore().certificates());
			return defaultCerts;
		case "bundled":
		case "system":
			return systemCertificates();
		case "extra": {
			const store = getExtraStore();
			return Object.freeze(store ? store.certificates() : []);
		}
		default:
			throw invalidArgValue("type", type);
	}
}

/** tls.setDefaultCACertificates(certs): the trust of the contexts created from now on */
export function setDefaultCACertificates(certs: readonly (string | ArrayBufferView)[]): void {
	if (!Array.isArray(certs)) throw invalidArgType("certs", "an instance of Array", certs);
	for (let i = 0; i < certs.length; i++) {
		const c = certs[i];
		if (typeof c !== "string" && !ArrayBuffer.isView(c))
			throw invalidArgType(`certs[${i}]`, "of type string or an instance of ArrayBufferView", c);
	}
	const store = native.createCertStore(false);
	for (const c of certs) store.addPEM(c);
	rootStore = store;
	defaultCerts = null;
}

// ---------------------------------------------------------------------------
// ciphers

let cipherCache: string[] | null = null;

/** tls.getCiphers(): lower-case names of the supported ciphers */
export function getCiphers(): string[] {
	if (!cipherCache) {
		const seen = new Set<string>();
		for (const name of native.getSSLCiphers()) seen.add(name.toLowerCase());
		cipherCache = [...seen];
	}
	return cipherCache.slice();
}

function processCiphers(ciphers: string, name: string): { cipherList: string; cipherSuites: string } {
	const parts = (ciphers || defaults.ciphers).split(":");
	const isSuite = (c: string): boolean => c.startsWith("TLS_") || c.startsWith("!TLS_");
	const cipherList = parts.filter((c) => c.length > 0 && !isSuite(c)).join(":");
	const cipherSuites = parts.filter((c) => c.length > 0 && isSuite(c)).join(":");
	if (cipherSuites === "" && cipherList === "") throw invalidArgValue(name, ciphers);
	return { cipherList, cipherSuites };
}

// ---------------------------------------------------------------------------
// SecureContext

function toVersion(which: string, v: unknown, def: string): number {
	const s = v ?? def;
	if (s === "TLSv1") return native.TLS1_VERSION;
	if (s === "TLSv1.1") return native.TLS1_1_VERSION;
	if (s === "TLSv1.2") return native.TLS1_2_VERSION;
	if (s === "TLSv1.3") return native.TLS1_3_VERSION;
	throw codedError(
		"ERR_TLS_INVALID_PROTOCOL_VERSION",
		`${String(s)} is not a valid ${which} TLS protocol version`,
		TypeError,
	);
}

/** [min, max] of a legacy secureProtocol method name */
function methodVersions(method: string): [number, number] {
	const base = method.replace(/_(client|server)_method$/, "_method");
	switch (base) {
		case "TLS_method":
		case "SSLv23_method":
			return [0, native.TLS1_3_VERSION];
		case "TLSv1_method":
			return [native.TLS1_VERSION, native.TLS1_VERSION];
		case "TLSv1_1_method":
			return [native.TLS1_1_VERSION, native.TLS1_1_VERSION];
		case "TLSv1_2_method":
			return [native.TLS1_2_VERSION, native.TLS1_2_VERSION];
		case "SSLv2_method":
			throw codedError("ERR_TLS_INVALID_PROTOCOL_METHOD", "SSLv2 methods disabled", TypeError);
		case "SSLv3_method":
			throw codedError("ERR_TLS_INVALID_PROTOCOL_METHOD", "SSLv3 methods disabled", TypeError);
	}
	throw codedError("ERR_TLS_INVALID_PROTOCOL_METHOD", `Unknown method: ${method}`, TypeError);
}

export class SecureContext {
	context: NativeSecureContext | null;
	singleUse?: boolean;

	constructor(secureProtocol?: string, secureOptions?: number, minVersion?: string, maxVersion?: string) {
		if (secureProtocol) {
			if (minVersion != null) {
				throw codedError(
					"ERR_TLS_PROTOCOL_VERSION_CONFLICT",
					`TLS protocol version ${minVersion} conflicts with secureProtocol ${secureProtocol}`,
					TypeError,
				);
			}
			if (maxVersion != null) {
				throw codedError(
					"ERR_TLS_PROTOCOL_VERSION_CONFLICT",
					`TLS protocol version ${maxVersion} conflicts with secureProtocol ${secureProtocol}`,
					TypeError,
				);
			}
		}
		let minV: number;
		let maxV: number;
		if (secureProtocol) {
			validateString(secureProtocol, "secureProtocol");
			[minV, maxV] = methodVersions(secureProtocol);
		} else {
			minV = toVersion("minimum", minVersion, defaults.minVersion);
			maxV = toVersion("maximum", maxVersion, defaults.maxVersion);
		}
		this.context = native.createSecureContext(minV, maxV);
		if (secureOptions) {
			if (!Number.isInteger(secureOptions)) throw invalidArgType("secureOptions", "an integer", secureOptions);
			this.context.setOptions(secureOptions);
		}
	}
}

function validateKeyOrCertOption(name: string, value: unknown): asserts value is KeyOrCert {
	if (typeof value !== "string" && !ArrayBuffer.isView(value)) {
		throw invalidArgType(name, "of type string or an instance of Buffer, TypedArray, or DataView", value);
	}
}

function toBuf(v: unknown, name: string): ArrayBufferView {
	if (typeof v === "string") return Buffer.from(v, "utf8");
	if (ArrayBuffer.isView(v)) return v;
	throw invalidArgType(name, "of type string or an instance of Buffer, TypedArray, or DataView", v);
}

export function validateOpenSSLEngineOptions(options: Record<string, unknown>): void {
	if (options.clientCertEngine != null || options.privateKeyEngine != null || options.privateKeyIdentifier != null) {
		throw codedError("ERR_CRYPTO_CUSTOM_ENGINE_NOT_SUPPORTED", "Custom engines not supported by this OpenSSL");
	}
}

/** Node's configSecureContext() */
export function configSecureContext(
	context: NativeSecureContext,
	options: SecureContextOptions = {},
	name = "options",
): void {
	validateObject(options, name);
	validateOpenSSLEngineOptions(options);
	const {
		allowPartialTrustChain,
		ca,
		cert,
		certificateCompression,
		ciphers = defaults.ciphers,
		crl,
		dhparam,
		ecdhCurve = defaults.ecdhCurve || "auto",
		key,
		passphrase,
		pfx,
		sessionIdContext,
		sessionTimeout,
		sigalgs,
		ticketKeys,
	} = options;

	if (ciphers !== undefined && ciphers !== null) validateString(ciphers, `${name}.ciphers`);
	const { cipherList, cipherSuites } = processCiphers(ciphers as string, `${name}.ciphers`);
	if (cipherSuites !== "") context.setCipherSuites(cipherSuites);
	context.setCiphers(cipherList);
	if (
		cipherList === "" &&
		context.getMinProto() < native.TLS1_3_VERSION &&
		context.getMaxProto() > native.TLS1_2_VERSION
	) {
		context.setMinProto(native.TLS1_3_VERSION);
	}

	// the CAs before the certificate
	if (ca) {
		const list = Array.isArray(ca) ? ca : [ca];
		for (const c of list) {
			validateKeyOrCertOption(`${name}.ca`, c);
			context.addCACert(c);
		}
	} else {
		context.addRootCerts(getRootStore());
	}

	if (allowPartialTrustChain) context.setAllowPartialTrustChain();

	if (cert) {
		const list = Array.isArray(cert) ? cert : [cert];
		for (const c of list) {
			validateKeyOrCertOption(`${name}.cert`, c);
			context.setCert(c);
		}
	}

	// the key after the certificate
	if (key) {
		if (Array.isArray(key)) {
			for (const val of key) {
				let pem: unknown = val;
				let pass: unknown = passphrase;
				if (val !== null && typeof val === "object" && !ArrayBuffer.isView(val)) {
					if ("pem" in val && val.pem !== undefined) pem = val.pem;
					if ("passphrase" in val && val.passphrase !== undefined) pass = val.passphrase;
				}
				setKey(context, pem, pass, name);
			}
		} else {
			setKey(context, key, passphrase, name);
		}
	}

	if (certificateCompression != null) {
		if (!Array.isArray(certificateCompression)) {
			throw invalidArgType(`${name}.certificateCompression`, "an instance of Array", certificateCompression);
		}
		if (certificateCompression.length > 3) {
			throw invalidArgValue(
				`${name}.certificateCompression`,
				certificateCompression,
				"can specify at most 3 algorithms",
			);
		}
		for (let i = 0; i < certificateCompression.length; i++) {
			const algo = certificateCompression[i];
			if (algo !== "zlib" && algo !== "brotli" && algo !== "zstd") {
				throw invalidArgValue(`${name}.certificateCompression[${i}]`, algo, "must be 'zlib', 'brotli', or 'zstd'");
			}
		}
		// the OpenSSL build has no certificate compression: certificates are
		// sent uncompressed, which every peer accepts
	}

	if (sigalgs !== undefined && sigalgs !== null) {
		validateString(sigalgs, `${name}.sigalgs`);
		if (sigalgs === "") throw invalidArgValue(`${name}.sigalgs`, sigalgs);
		context.setSigalgs(sigalgs);
	}

	validateString(ecdhCurve, `${name}.ecdhCurve`);
	context.setECDHCurve(ecdhCurve);

	if (dhparam !== undefined && dhparam !== null) {
		validateKeyOrCertOption(`${name}.dhparam`, dhparam);
		const warning = context.setDHParam(dhparam === "auto" || dhparam);
		if (warning) process.emitWarning?.(warning, "SecurityWarning");
	}

	if (crl !== undefined && crl !== null) {
		const list = Array.isArray(crl) ? crl : [crl];
		for (const c of list) {
			validateKeyOrCertOption(`${name}.crl`, c);
			context.addCRL(c);
		}
	}

	if (sessionIdContext !== undefined && sessionIdContext !== null) {
		validateString(sessionIdContext, `${name}.sessionIdContext`);
		context.setSessionIdContext(sessionIdContext);
	}

	if (pfx !== undefined && pfx !== null) {
		if (Array.isArray(pfx)) {
			for (const val of pfx) {
				let raw: unknown = val;
				let pass: unknown = passphrase;
				if (val !== null && typeof val === "object" && !ArrayBuffer.isView(val)) {
					if ("buf" in val && val.buf !== undefined) raw = val.buf;
					if ("passphrase" in val && val.passphrase) pass = val.passphrase;
				}
				if (pass !== undefined && pass !== null)
					context.loadPKCS12(toBuf(raw, `${name}.pfx`), toBuf(pass, `${name}.passphrase`));
				else context.loadPKCS12(toBuf(raw, `${name}.pfx`));
			}
		} else if (passphrase) {
			context.loadPKCS12(toBuf(pfx, `${name}.pfx`), toBuf(passphrase, `${name}.passphrase`));
		} else {
			context.loadPKCS12(toBuf(pfx, `${name}.pfx`));
		}
	}

	if (ticketKeys !== undefined && ticketKeys !== null) {
		if (!ArrayBuffer.isView(ticketKeys))
			throw invalidArgType(`${name}.ticketKeys`, "an instance of Buffer, TypedArray, or DataView", ticketKeys);
		if (ticketKeys.byteLength !== 48)
			throw invalidArgValue(`${name}.ticketKeys`, ticketKeys.byteLength, "must be exactly 48 bytes");
		context.setTicketKeys(ticketKeys);
	}

	if (sessionTimeout !== undefined && sessionTimeout !== null) {
		validateNumber(sessionTimeout, `${name}.sessionTimeout`, 0, 2 ** 31 - 1);
		if (!Number.isInteger(sessionTimeout)) throw outOfRange(`${name}.sessionTimeout`, "an integer", sessionTimeout);
		context.setSessionTimeout(sessionTimeout);
	}
}

function setKey(context: NativeSecureContext, key: unknown, passphrase: unknown, name: string): void {
	validateKeyOrCertOption(`${name}.key`, key);
	if (passphrase !== undefined && passphrase !== null) validateString(passphrase, `${name}.passphrase`);
	context.setKey(key, (passphrase ?? undefined) as string | undefined);
}

/** tls.createSecureContext(options) */
export function createSecureContext(options?: SecureContextOptions): SecureContext {
	const opts: SecureContextOptions = options || {};
	const { honorCipherOrder, minVersion, maxVersion, secureProtocol, singleUse } = opts;
	let secureOptions = opts.secureOptions;
	if (honorCipherOrder) secureOptions = (secureOptions ?? 0) | native.SSL_OP_CIPHER_SERVER_PREFERENCE;
	const c = new SecureContext(secureProtocol, secureOptions, minVersion, maxVersion);
	configSecureContext(c.context as NativeSecureContext, opts);
	if (singleUse) c.singleUse = true;
	return c;
}

/** the native context of a SecureContext (or of a raw native context) */
export function nativeContextOf(ctx: unknown, name: string): NativeSecureContext {
	let inner: unknown = ctx;
	if (ctx instanceof SecureContext) inner = ctx.context;
	else if (ctx !== null && typeof ctx === "object" && "context" in ctx && ctx.context) inner = ctx.context;
	if (inner === null || typeof inner !== "object" || !("setCert" in inner) || typeof inner.setCert !== "function") {
		throw codedError("ERR_TLS_INVALID_CONTEXT", `${name} must be a SecureContext`, TypeError);
	}
	// checked above: a native context object of qbun:tls
	return inner as NativeSecureContext;
}

/** translate the native certificate object like Node's translatePeerCertificate */
export function translatePeerCertificate(c: PeerCertificate | null | undefined): PeerCertificate | null {
	if (!c) return null;
	if (c.issuerCertificate != null && c.issuerCertificate !== c) {
		c.issuerCertificate = translatePeerCertificate(c.issuerCertificate) ?? undefined;
	}
	if (typeof c.infoAccess === "string") {
		const info = c.infoAccess;
		const out: Record<string, string[]> = Object.create(null);
		info.replace(/([^\n:]*):([^\n]*)(?:\n|$)/g, (_all: string, k: string, val: string): string => {
			let v = val;
			if (v.charCodeAt(0) === 0x22) {
				try {
					v = JSON.parse(v) as string;
				} catch {}
			}
			if (k in out) out[k].push(v);
			else out[k] = [v];
			return "";
		});
		c.infoAccess = out;
	}
	if (typeof c.valid_from === "string" && c.validFromDate === undefined) {
		Object.defineProperty(c, "validFromDate", {
			value: new Date(c.valid_from),
			enumerable: true,
			configurable: true,
			writable: true,
		});
	}
	if (typeof c.valid_to === "string" && c.validToDate === undefined) {
		Object.defineProperty(c, "validToDate", {
			value: new Date(c.valid_to),
			enumerable: true,
			configurable: true,
			writable: true,
		});
	}
	return c;
}

// ---------------------------------------------------------------------------
// ALPN

function convertProtocols(protocols: readonly string[]): Buffer {
	const lens = new Array<number>(protocols.length);
	let total = 0;
	for (let i = 0; i < protocols.length; i++) {
		const len = Buffer.byteLength(protocols[i]);
		if (len === 0) throw invalidArgValue(`protocols[${i}]`, protocols[i], "must be a non-empty string");
		if (len > 255) {
			throw codedError(
				"ERR_OUT_OF_RANGE",
				`The value of "The byte length of the protocol at index ${i} exceeds the maximum length." is out of range. It must be <= 255. Received ${len}`,
				RangeError,
			);
		}
		lens[i] = len;
		total += 1 + len;
	}
	const buff = Buffer.allocUnsafe(total);
	let offset = 0;
	for (let i = 0; i < protocols.length; i++) {
		buff[offset++] = lens[i];
		buff.write(protocols[i], offset);
		offset += lens[i];
	}
	return buff;
}

function validateALPNBuffer(buffer: Buffer): void {
	let offset = 0;
	while (offset < buffer.length) {
		const len = buffer[offset];
		if (len === 0) throw invalidArgValue("ALPNProtocols", buffer, "must not contain zero-length protocol");
		if (offset + 1 + len > buffer.length) throw invalidArgValue("ALPNProtocols", buffer, "contains truncated protocol");
		offset += 1 + len;
	}
}

/** tls.convertALPNProtocols(protocols, out): out.ALPNProtocols = wire format */
export function convertALPNProtocols(protocols: unknown, out: { ALPNProtocols?: unknown }): void {
	if (Array.isArray(protocols)) {
		out.ALPNProtocols = convertProtocols(protocols as string[]);
	} else if (protocols instanceof Uint8Array) {
		const buf = Buffer.from(protocols);
		validateALPNBuffer(buf);
		out.ALPNProtocols = buf;
	} else if (ArrayBuffer.isView(protocols)) {
		const buf = Buffer.from(protocols.buffer.slice(protocols.byteOffset, protocols.byteOffset + protocols.byteLength));
		validateALPNBuffer(buf);
		out.ALPNProtocols = buf;
	}
}

// ---------------------------------------------------------------------------
// checkServerIdentity

/** Node's canonicalizeIP(): the canonical text of an IP address, undefined when invalid */
export function canonicalizeIP(ip: string): string | undefined {
	if (isIPv4(ip)) return ip;
	if (isIPv6(ip)) {
		const v = parseIPv6(ip);
		return v === null ? undefined : formatIPv6(v);
	}
	return undefined;
}

function unfqdn(host: string): string {
	return host.replace(/[.]$/, "");
}

function splitHost(host: string): string[] {
	// String#toLowerCase() is locale sensitive: lower-case A-Z only
	return unfqdn(host)
		.replace(/[A-Z]/g, (c) => String.fromCharCode(32 + c.charCodeAt(0)))
		.split(".");
}

function check(hostParts: string[], pattern: string | undefined, wildcards: boolean): boolean {
	if (!pattern) return false;
	const patternParts = splitHost(pattern);
	if (hostParts.length !== patternParts.length) return false;
	if (patternParts.includes("")) return false;
	const isBad = (s: string): boolean => /[^\u0021-\u007F]/u.test(s);
	if (patternParts.some(isBad)) return false;
	for (let i = hostParts.length - 1; i > 0; i -= 1) {
		if (hostParts[i] !== patternParts[i]) return false;
	}
	const hostSubdomain = hostParts[0];
	const patternSubdomain = patternParts[0];
	const patternSubdomainParts = patternSubdomain.split("*", 3);
	if (patternSubdomainParts.length === 1 || patternSubdomain.includes("xn--"))
		return hostSubdomain === patternSubdomain;
	if (!wildcards) return false;
	if (patternSubdomainParts.length > 2) return false;
	if (patternParts.length <= 2) return false;
	const [prefix, suffix] = patternSubdomainParts;
	if (prefix.length + suffix.length > hostSubdomain.length) return false;
	if (!hostSubdomain.startsWith(prefix)) return false;
	if (!hostSubdomain.endsWith(suffix)) return false;
	return true;
}

// eslint-disable-next-line no-control-regex
const jsonStringPattern = /^"(?:[^"\\\u0000-\u001f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"/;

function splitEscapedAltNames(altNames: string): string[] {
	const result: string[] = [];
	let currentToken = "";
	let offset = 0;
	while (offset !== altNames.length) {
		const nextSep = altNames.indexOf(",", offset);
		const nextQuote = altNames.indexOf('"', offset);
		if (nextQuote !== -1 && (nextSep === -1 || nextQuote < nextSep)) {
			currentToken += altNames.substring(offset, nextQuote);
			const match = jsonStringPattern.exec(altNames.substring(nextQuote));
			if (!match)
				throw codedError("ERR_TLS_CERT_ALTNAME_FORMAT", "Invalid subject alternative name string", SyntaxError);
			currentToken += JSON.parse(match[0]) as string;
			offset = nextQuote + match[0].length;
		} else if (nextSep !== -1) {
			currentToken += altNames.substring(offset, nextSep);
			result.push(currentToken);
			currentToken = "";
			offset = nextSep + 2;
		} else {
			currentToken += altNames.substring(offset);
			offset = altNames.length;
		}
	}
	result.push(currentToken);
	return result;
}

export interface AltNameError extends Error {
	code: "ERR_TLS_CERT_ALTNAME_INVALID";
	reason: string;
	host: string;
	cert: PeerCertificate;
}

function altNameInvalid(reason: string, host: string, cert: PeerCertificate): AltNameError {
	const err = codedError(
		"ERR_TLS_CERT_ALTNAME_INVALID",
		`Hostname/IP does not match certificate's altnames: ${reason}`,
	) as AltNameError;
	err.reason = reason;
	err.host = host;
	err.cert = cert;
	return err;
}

/** tls.checkServerIdentity(hostname, cert): undefined or an Error */
export function checkServerIdentity(hostname: string, cert: PeerCertificate): AltNameError | undefined {
	const subject = cert.subject;
	const altNames = cert.subjectaltname;
	const dnsNames: string[] = [];
	const ips: (string | undefined)[] = [];

	let host = `${hostname}`;
	const hostnameASCII = domainToASCII(host);
	host = unfqdn(host);
	const hostnameASCIIWithoutFQDN = unfqdn(hostnameASCII);
	const hostnameIsIP = isIP(host);
	const hostnameASCIIIsIP = isIP(hostnameASCIIWithoutFQDN);

	if (altNames) {
		const split = altNames.includes('"') ? splitEscapedAltNames(altNames) : altNames.split(", ");
		for (const name of split) {
			if (name.startsWith("DNS:")) dnsNames.push(name.slice(4));
			else if (name.startsWith("IP Address:")) ips.push(canonicalizeIP(name.slice(11)));
		}
	}

	if (!hostnameIsIP && hostnameASCIIIsIP) return altNameInvalid("Host is a non-canonical IP address", host, cert);

	let valid = false;
	let reason = "Unknown reason";

	if (hostnameIsIP) {
		valid = ips.includes(canonicalizeIP(host));
		if (!valid) reason = `IP: ${host} is not in the cert's list: ${ips.join(", ")}`;
	} else if (dnsNames.length > 0 || subject?.CN) {
		const hostParts = splitHost(hostnameASCIIWithoutFQDN);
		const wildcard = (pattern: string): boolean => check(hostParts, pattern, true);
		if (dnsNames.length > 0) {
			valid = dnsNames.some(wildcard);
			if (!valid) reason = `Host: ${host}. is not in the cert's altnames: ${altNames}`;
		} else {
			const cn = subject.CN;
			if (Array.isArray(cn)) valid = cn.some(wildcard);
			else if (cn) valid = wildcard(cn);
			if (!valid) reason = `Host: ${host}. is not cert's CN: ${String(cn)}`;
		}
	} else {
		reason = "Cert does not contain a DNS name";
	}

	if (!valid) return altNameInvalid(reason, host, cert);
	return undefined;
}

/** the legacy tls.parseCertString() (deprecated in Node, still exported by Bun) */
export function parseCertString(s: string): Record<string, string | string[]> {
	const out: Record<string, string | string[]> = Object.create(null);
	for (const part of s.split("\n")) {
		const sep = part.indexOf("=");
		if (sep <= 0) continue;
		const key = part.slice(0, sep);
		const value = part.slice(sep + 1);
		const prev = out[key];
		if (prev === undefined) out[key] = value;
		else if (Array.isArray(prev)) prev.push(value);
		else out[key] = [prev, value];
	}
	return out;
}
