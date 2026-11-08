// node:tls — TLS sockets and servers on OpenSSL (qbun:tls).
//
// The default trust is the system's root store (OpenSSL's default paths plus
// the system bundle, e.g. /etc/ssl/cert.pem): Node's bundled Mozilla list is
// not shipped, so rootCertificates lists the system certificates.

import {
	CLIENT_RENEG_LIMIT,
	CLIENT_RENEG_WINDOW,
	SecureContext,
	checkServerIdentity,
	convertALPNProtocols,
	createSecureContext,
	defaults,
	getCACertificates,
	getCiphers,
	parseCertString,
	rootCertificates,
	setDefaultCACertificates,
} from "internal:tls/common";
import { Server, TLSSocket, connect, createServer } from "internal:tls/wrap";

export type {
	Certificate,
	CipherNameAndProtocol,
	EphemeralKeyInfo,
	PeerCertificate,
	SecureContextOptions,
} from "internal:tls/common";
export type { ConnectionOptions, TLSSocketOptions, TlsServerOptions } from "internal:tls/wrap";

export let DEFAULT_CIPHERS: string = defaults.ciphers;
export let DEFAULT_ECDH_CURVE: string = defaults.ecdhCurve;
export let DEFAULT_MIN_VERSION: string = defaults.minVersion;
export let DEFAULT_MAX_VERSION: string = defaults.maxVersion;

export {
	CLIENT_RENEG_LIMIT,
	CLIENT_RENEG_WINDOW,
	SecureContext,
	Server,
	TLSSocket,
	checkServerIdentity,
	connect,
	convertALPNProtocols,
	createSecureContext,
	createServer,
	getCACertificates,
	getCiphers,
	parseCertString,
	rootCertificates,
	setDefaultCACertificates,
};

const tls = {
	CLIENT_RENEG_LIMIT,
	CLIENT_RENEG_WINDOW,
	SecureContext,
	Server,
	TLSSocket,
	checkServerIdentity,
	connect,
	convertALPNProtocols,
	createSecureContext,
	createServer,
	getCACertificates,
	getCiphers,
	parseCertString,
	setDefaultCACertificates,
	// assignments (require("tls").DEFAULT_MIN_VERSION = "TLSv1.3") reach the
	// settings the contexts are created with, like Node's mutable exports
	get DEFAULT_CIPHERS(): string {
		return DEFAULT_CIPHERS;
	},
	set DEFAULT_CIPHERS(value: string) {
		DEFAULT_CIPHERS = value;
		defaults.ciphers = value;
	},
	get DEFAULT_ECDH_CURVE(): string {
		return DEFAULT_ECDH_CURVE;
	},
	set DEFAULT_ECDH_CURVE(value: string) {
		DEFAULT_ECDH_CURVE = value;
		defaults.ecdhCurve = value;
	},
	get DEFAULT_MIN_VERSION(): string {
		return DEFAULT_MIN_VERSION;
	},
	set DEFAULT_MIN_VERSION(value: string) {
		DEFAULT_MIN_VERSION = value;
		defaults.minVersion = value;
	},
	get DEFAULT_MAX_VERSION(): string {
		return DEFAULT_MAX_VERSION;
	},
	set DEFAULT_MAX_VERSION(value: string) {
		DEFAULT_MAX_VERSION = value;
		defaults.maxVersion = value;
	},
	rootCertificates,
};
Object.defineProperty(tls, "rootCertificates", { configurable: false, writable: false });

export default tls;
