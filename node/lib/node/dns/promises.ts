// node:dns/promises — name resolution.
//
// lookup()/lookupService() use the system resolver (getaddrinfo/getnameinfo
// of qbun:net, like Node). resolve4/resolve6 also go through getaddrinfo so
// they honour the system configuration; the other record types are queried
// with a small DNS client speaking DNS over TCP to the configured servers
// (/etc/resolv.conf, or Resolver#setServers()).

import * as binding from "qbun:net";
import * as core from "qbun:core";
import { isIP, formatIPv6, parseIPv6 } from "internal:http/ip";
import { codedError, invalidArgType, invalidArgValue, validateNumber, validateString } from "internal:errors";

interface DnsBinding {
	getaddrinfo(
		host: string,
		family: 0 | 4 | 6,
		flags: number,
		cb: (err: Error | null, addresses: { address: string; family: 4 | 6 }[]) => void,
	): void;
	getnameinfo(
		address: string,
		port: number,
		cb: (err: Error | null, hostname?: string, service?: string) => void,
	): void;
}

const native = binding as unknown as DnsBinding;

type SystemError = Error & { code?: string; errno?: number; syscall?: string; hostname?: string };

// ---------------------------------------------------------------------------
// constants

const darwin = process.platform === "darwin";
/** getaddrinfo() hint flags of the platform */
export const ADDRCONFIG = darwin ? 0x400 : 0x20;
export const V4MAPPED = darwin ? 0x800 : 0x8;
export const ALL = darwin ? 0x100 : 0x10;

export const NODATA = "ENODATA";
export const FORMERR = "EFORMERR";
export const SERVFAIL = "ESERVFAIL";
export const NOTFOUND = "ENOTFOUND";
export const NOTIMP = "ENOTIMP";
export const REFUSED = "EREFUSED";
export const BADQUERY = "EBADQUERY";
export const BADNAME = "EBADNAME";
export const BADFAMILY = "EBADFAMILY";
export const BADRESP = "EBADRESP";
export const CONNREFUSED = "ECONNREFUSED";
export const TIMEOUT = "ETIMEOUT";
export const EOF = "EOF";
export const FILE = "EFILE";
export const NOMEM = "ENOMEM";
export const DESTRUCTION = "EDESTRUCTION";
export const BADSTR = "EBADSTR";
export const BADFLAGS = "EBADFLAGS";
export const NONAME = "ENONAME";
export const BADHINTS = "EBADHINTS";
export const NOTINITIALIZED = "ENOTINITIALIZED";
export const LOADIPHLPAPI = "ELOADIPHLPAPI";
export const ADDRGETNETWORKPARAMS = "EADDRGETNETWORKPARAMS";
export const CANCELLED = "ECANCELLED";

export const constants = {
	ADDRCONFIG,
	V4MAPPED,
	ALL,
	NODATA,
	FORMERR,
	SERVFAIL,
	NOTFOUND,
	NOTIMP,
	REFUSED,
	BADQUERY,
	BADNAME,
	BADFAMILY,
	BADRESP,
	CONNREFUSED,
	TIMEOUT,
	EOF,
	FILE,
	NOMEM,
	DESTRUCTION,
	BADSTR,
	BADFLAGS,
	NONAME,
	BADHINTS,
	NOTINITIALIZED,
	LOADIPHLPAPI,
	ADDRGETNETWORKPARAMS,
	CANCELLED,
};

// ---------------------------------------------------------------------------
// errors

/** Node's DNSException: "<syscall> <CODE> <hostname>" */
export function dnsException(code: string, syscall: string, hostname?: string, errno?: number): SystemError {
	const err = new Error(`${syscall} ${code}${hostname ? ` ${hostname}` : ""}`) as SystemError;
	err.errno = errno;
	err.code = code;
	err.syscall = syscall;
	if (hostname) err.hostname = hostname;
	return err;
}

/** a getaddrinfo/getnameinfo error of the binding as Node reports it */
function fromNative(err: Error, syscall: string, hostname: string): SystemError {
	const e = err as SystemError;
	let code = e.code ?? "EAI_FAIL";
	if (code === "EAI_NODATA" || code === "EAI_NONAME") code = "ENOTFOUND";
	return dnsException(code, syscall, hostname, e.errno);
}

// ---------------------------------------------------------------------------
// result order

export type ResultOrder = "ipv4first" | "ipv6first" | "verbatim";
const validOrders: ResultOrder[] = ["verbatim", "ipv4first", "ipv6first"];
let defaultOrder: ResultOrder = "verbatim";

function validateOrder(value: unknown, name: string): asserts value is ResultOrder {
	if (!validOrders.includes(value as ResultOrder)) {
		throw invalidArgValue(name, value, "must be one of: 'verbatim', 'ipv4first', 'ipv6first'");
	}
}

export function setDefaultResultOrder(order: ResultOrder): void {
	validateOrder(order, "dnsOrder");
	defaultOrder = order;
}

export function getDefaultResultOrder(): ResultOrder {
	return defaultOrder;
}

// ---------------------------------------------------------------------------
// lookup

export interface LookupOptions {
	family?: number | "IPv4" | "IPv6";
	hints?: number;
	all?: boolean;
	verbatim?: boolean;
	order?: ResultOrder;
}

export interface LookupAddress {
	address: string;
	family: 4 | 6;
}

export interface ParsedLookup {
	family: 0 | 4 | 6;
	hints: number;
	all: boolean;
	order: ResultOrder;
}

/** validate lookup() options the way Node does */
export function parseLookupOptions(options: unknown): ParsedLookup {
	const out: ParsedLookup = { family: 0, hints: 0, all: false, order: defaultOrder };
	if (options === undefined || options === null) return out;
	if (typeof options === "number") {
		if (options !== 0 && options !== 4 && options !== 6)
			throw invalidArgValue("family", options, "must be one of: 0, 4, 6");
		out.family = (options + 0) as 0 | 4 | 6;
		return out;
	}
	if (typeof options !== "object") throw invalidArgType("options", "of type number or an instance of Object", options);
	const o = options as LookupOptions;
	if (o.hints != null) {
		validateNumber(o.hints, "options.hints");
		const hints = o.hints >>> 0;
		if ((hints & ~(ADDRCONFIG | ALL | V4MAPPED)) !== 0) throw invalidArgValue("hints", hints);
		out.hints = hints;
	}
	if (o.family != null) {
		if (o.family === "IPv4") out.family = 4;
		else if (o.family === "IPv6") out.family = 6;
		else if (o.family === 0 || o.family === 4 || o.family === 6) out.family = (o.family + 0) as 0 | 4 | 6;
		else throw invalidArgValue("options.family", o.family, "must be one of: 0, 4, 6");
	}
	if (o.all != null) {
		if (typeof o.all !== "boolean") throw invalidArgType("options.all", "of type boolean", o.all);
		out.all = o.all;
	}
	if (o.verbatim != null) {
		if (typeof o.verbatim !== "boolean") throw invalidArgType("options.verbatim", "of type boolean", o.verbatim);
		out.order = o.verbatim ? "verbatim" : "ipv4first";
	}
	if (o.order != null) {
		validateOrder(o.order, "options.order");
		out.order = o.order;
	}
	return out;
}

const inflight = new Map<string, ((err: Error | null, addrs: LookupAddress[]) => void)[]>();

/** getaddrinfo() with Node's ordering; shares concurrent identical lookups */
export function getaddrinfo(
	hostname: string,
	opts: ParsedLookup,
	cb: (err: Error | null, addrs: LookupAddress[]) => void,
): void {
	const key = `${hostname}\0${opts.family}\0${opts.hints}\0${opts.order}`;
	const waiting = inflight.get(key);
	if (waiting) {
		waiting.push(cb);
		return;
	}
	const list = [cb];
	inflight.set(key, list);
	const finish = (err: Error | null, addrs: LookupAddress[]) => {
		inflight.delete(key);
		for (const w of list) w(err, addrs);
	};
	try {
		native.getaddrinfo(hostname, opts.family, opts.hints, (err, addresses) => {
			if (err) {
				finish(fromNative(err, "getaddrinfo", hostname), []);
				return;
			}
			const seen = new Set<string>();
			let result: LookupAddress[] = [];
			for (const a of addresses) {
				if (seen.has(a.address)) continue;
				seen.add(a.address);
				result.push({ address: a.address, family: a.family });
			}
			if (opts.order === "ipv4first")
				result = [...result.filter((a) => a.family === 4), ...result.filter((a) => a.family === 6)];
			else if (opts.order === "ipv6first")
				result = [...result.filter((a) => a.family === 6), ...result.filter((a) => a.family === 4)];
			if (result.length === 0) {
				finish(dnsException("ENOTFOUND", "getaddrinfo", hostname), []);
				return;
			}
			finish(null, result);
		});
	} catch (e) {
		inflight.delete(key);
		throw e;
	}
}

export function validateHostname(hostname: unknown): void {
	if (hostname) {
		validateString(hostname, "hostname");
		if (hostname.includes("\0")) throw invalidArgValue("hostname", hostname, "must be a string without null bytes");
	}
}

export function lookup(hostname: string, options?: LookupOptions | number): Promise<LookupAddress | LookupAddress[]> {
	validateHostname(hostname);
	const opts = parseLookupOptions(options);
	if (!hostname) throw invalidArgValue("hostname", hostname, "must be a non-empty string");
	const matched = isIP(hostname);
	if (matched) {
		const addr: LookupAddress = { address: hostname, family: matched };
		return Promise.resolve(opts.all ? [addr] : addr);
	}
	const { promise, resolve, reject } = Promise.withResolvers<LookupAddress | LookupAddress[]>();
	getaddrinfo(hostname, opts, (err, addrs) => {
		if (err) reject(err);
		else resolve(opts.all ? addrs : addrs[0]);
	});
	return promise;
}

export function validatePortArg(port: unknown): number {
	if (
		(typeof port !== "number" && typeof port !== "string") ||
		(typeof port === "string" && port.trim().length === 0) ||
		+port !== +port >>> 0 ||
		+port > 0xffff
	) {
		throw codedError(
			"ERR_SOCKET_BAD_PORT",
			`Port should be >= 0 and < 65536. Received ${typeof port === "string" ? `type string ('${port}')` : `type ${typeof port} (${String(port)})`}.`,
			RangeError,
		);
	}
	return +port | 0;
}

export function getnameinfo(
	address: string,
	port: number,
	cb: (err: Error | null, hostname: string, service: string) => void,
): void {
	native.getnameinfo(address, port, (err, hostname, service) => {
		if (err) cb(fromNative(err, "getnameinfo", address), "", "");
		else cb(null, hostname ?? "", service ?? "");
	});
}

export function lookupService(address: string, port: number): Promise<{ hostname: string; service: string }> {
	validateString(address, "address");
	if (isIP(address) === 0) throw invalidArgValue("address", address);
	const p = validatePortArg(port);
	const { promise, resolve, reject } = Promise.withResolvers<{ hostname: string; service: string }>();
	getnameinfo(address, p, (err, hostname, service) => {
		if (err) reject(err);
		else resolve({ hostname, service });
	});
	return promise;
}

// ---------------------------------------------------------------------------
// DNS message encoding / decoding (RFC 1035)

const TYPES: Record<string, number> = {
	A: 1,
	NS: 2,
	CNAME: 5,
	SOA: 6,
	PTR: 12,
	MX: 15,
	TXT: 16,
	AAAA: 28,
	SRV: 33,
	NAPTR: 35,
	TLSA: 52,
	ANY: 255,
	CAA: 257,
};
const TYPE_NAMES: Record<number, string> = {};
for (const name in TYPES) TYPE_NAMES[TYPES[name]] = name;

const RCODE_ERRORS: Record<number, string> = { 1: FORMERR, 2: SERVFAIL, 3: NOTFOUND, 4: NOTIMP, 5: REFUSED };

function encodeQuery(id: number, name: string, type: number): Uint8Array {
	const labels = name
		.replace(/\.$/, "")
		.split(".")
		.filter((l) => l.length > 0);
	const parts: number[] = [id >> 8, id & 255, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0];
	for (const label of labels) {
		const bytes = Buffer.from(label, "utf8");
		if (bytes.length > 63) throw dnsException(BADNAME, "query", name);
		parts.push(bytes.length);
		for (const b of bytes) parts.push(b);
	}
	parts.push(0, type >> 8, type & 255, 0, 1);
	return Uint8Array.from(parts);
}

class Reader {
	constructor(
		readonly buf: Uint8Array,
		public pos = 0,
	) {}

	u8(): number {
		if (this.pos >= this.buf.length) throw new RangeError("truncated");
		return this.buf[this.pos++];
	}

	u16(): number {
		return (this.u8() << 8) | this.u8();
	}

	u32(): number {
		return ((this.u16() << 16) >>> 0) + this.u16();
	}

	bytes(n: number): Uint8Array {
		if (this.pos + n > this.buf.length) throw new RangeError("truncated");
		const out = this.buf.subarray(this.pos, this.pos + n);
		this.pos += n;
		return out;
	}

	name(): string {
		const labels: string[] = [];
		let pos = this.pos;
		let jumped = false;
		for (let guard = 0; guard < 128; guard++) {
			if (pos >= this.buf.length) throw new RangeError("truncated");
			const len = this.buf[pos];
			if (len === 0) {
				pos++;
				break;
			}
			if ((len & 0xc0) === 0xc0) {
				const ptr = ((len & 0x3f) << 8) | this.buf[pos + 1];
				if (!jumped) this.pos = pos + 2;
				jumped = true;
				pos = ptr;
				continue;
			}
			labels.push(Buffer.from(this.buf.subarray(pos + 1, pos + 1 + len)).toString("utf8"));
			pos += 1 + len;
		}
		if (!jumped) this.pos = pos;
		return labels.join(".");
	}

	string(): string {
		const len = this.u8();
		return Buffer.from(this.bytes(len)).toString("utf8");
	}
}

export type AnyRecord = Record<string, unknown> & { type: string };

interface Answer {
	type: number;
	ttl: number;
	data: Record<string, unknown> | string | string[];
}

function decodeRecord(r: Reader, type: number, len: number): Answer["data"] {
	const end = r.pos + len;
	switch (type) {
		case 1: {
			const b = r.bytes(4);
			return `${b[0]}.${b[1]}.${b[2]}.${b[3]}`;
		}
		case 28: {
			const b = r.bytes(16);
			let v = 0n;
			for (const x of b) v = (v << 8n) | BigInt(x);
			return formatIPv6(v);
		}
		case 2:
		case 5:
		case 12:
			return r.name();
		case 15:
			return { priority: r.u16(), exchange: r.name() };
		case 16: {
			const entries: string[] = [];
			while (r.pos < end) entries.push(r.string());
			return entries;
		}
		case 33:
			return { priority: r.u16(), weight: r.u16(), port: r.u16(), name: r.name() };
		case 35:
			return {
				order: r.u16(),
				preference: r.u16(),
				flags: r.string(),
				service: r.string(),
				regexp: r.string(),
				replacement: r.name(),
			};
		case 6:
			return {
				nsname: r.name(),
				hostmaster: r.name(),
				serial: r.u32(),
				refresh: r.u32(),
				retry: r.u32(),
				expire: r.u32(),
				minttl: r.u32(),
			};
		case 257: {
			const flags = r.u8();
			const tag = r.string();
			const value = Buffer.from(r.bytes(end - r.pos)).toString("utf8");
			return { critical: flags & 0x80 ? 128 : 0, [tag]: value };
		}
		case 52: {
			const certUsage = r.u8();
			const selector = r.u8();
			const match = r.u8();
			const data = r.bytes(end - r.pos);
			return { certUsage, selector, match, data: data.slice().buffer };
		}
	}
	r.pos = end;
	return "";
}

function decodeResponse(buf: Uint8Array, id: number, syscall: string, hostname: string): Answer[] {
	const r = new Reader(buf);
	try {
		if (r.u16() !== id) throw dnsException(BADRESP, syscall, hostname);
		const flags = r.u16();
		const rcode = flags & 0xf;
		const qd = r.u16();
		const an = r.u16();
		r.u16();
		r.u16();
		if (rcode !== 0) throw dnsException(RCODE_ERRORS[rcode] ?? BADRESP, syscall, hostname);
		for (let i = 0; i < qd; i++) {
			r.name();
			r.u16();
			r.u16();
		}
		const answers: Answer[] = [];
		for (let i = 0; i < an; i++) {
			r.name();
			const type = r.u16();
			r.u16();
			const ttl = r.u32();
			const len = r.u16();
			const start = r.pos;
			const data = decodeRecord(r, type, len);
			r.pos = start + len;
			answers.push({ type, ttl, data });
		}
		return answers;
	} catch (e) {
		if (e instanceof RangeError) throw dnsException(BADRESP, syscall, hostname);
		throw e;
	}
}

// ---------------------------------------------------------------------------
// servers

const DNS_PORT = 53;
type Server = [family: 4 | 6, address: string, port: number];

function systemServers(): Server[] {
	const servers: Server[] = [];
	try {
		const text = core.readText("/etc/resolv.conf") as string;
		for (const line of text.split("\n")) {
			const m = /^\s*nameserver\s+(\S+)/.exec(line);
			if (!m) continue;
			const addr = m[1].replace(/%.*$/, "");
			const family = isIP(addr);
			if (family) servers.push([family, addr, DNS_PORT]);
		}
	} catch {}
	if (servers.length === 0) servers.push([4, "127.0.0.1", DNS_PORT]);
	return servers;
}

function parseServers(servers: unknown): Server[] {
	if (!Array.isArray(servers)) throw invalidArgType("servers", "an instance of Array", servers);
	const out: Server[] = [];
	servers.forEach((serv: unknown, index: number) => {
		validateString(serv, `servers[${index}]`);
		const v = isIP(serv);
		if (v !== 0) {
			out.push([v, serv, DNS_PORT]);
			return;
		}
		const bracket = /^\[([^\]]+)\](?::(\d+))?$/.exec(serv);
		if (bracket && isIP(bracket[1])) {
			const port = parseInt(bracket[2] ?? "", 10) || DNS_PORT;
			validatePortArg(port);
			out.push([isIP(bracket[1]) as 4 | 6, bracket[1], port]);
			return;
		}
		const split = /^([^:]+):(\d+)$/.exec(serv);
		if (split && isIP(split[1])) {
			const port = parseInt(split[2], 10) || DNS_PORT;
			validatePortArg(port);
			out.push([isIP(split[1]) as 4 | 6, split[1], port]);
			return;
		}
		throw codedError("ERR_INVALID_IP_ADDRESS", `Invalid IP address: ${serv}`, TypeError);
	});
	return out;
}

function formatServer([family, address, port]: Server): string {
	if (port === DNS_PORT) return address;
	return family === 6 ? `[${address}]:${port}` : `${address}:${port}`;
}

// ---------------------------------------------------------------------------
// Resolver

interface NetSocket {
	connect(port: number, host: string): NetSocket;
	on(event: string, fn: (...args: unknown[]) => void): NetSocket;
	write(data: Uint8Array): boolean;
	destroy(err?: Error): void;
	setTimeout(ms: number, cb?: () => void): NetSocket;
	unref(): NetSocket;
}

interface NetModule {
	Socket: new () => NetSocket;
}

export interface ResolverOptions {
	timeout?: number;
	tries?: number;
	maxTimeout?: number;
}

export interface ResolveOptions {
	ttl?: boolean;
}

export interface RecordWithTtl {
	address: string;
	ttl: number;
}

const kQuery = Symbol("query");

export class Resolver {
	#servers: Server[] | null = null;
	#timeout: number;
	#tries: number;
	#pending = new Set<{ socket: NetSocket; fail: (err: Error) => void }>();

	constructor(options: ResolverOptions = {}) {
		const timeout = options.timeout ?? -1;
		validateNumber(timeout, "options.timeout", -1, 2 ** 31 - 1);
		const tries = options.tries ?? 4;
		validateNumber(tries, "options.tries", 1, 2 ** 31 - 1);
		this.#timeout = timeout === -1 ? 5000 : timeout;
		this.#tries = tries;
	}

	get #serverList(): Server[] {
		if (!this.#servers) this.#servers = systemServers();
		return this.#servers;
	}

	getServers(): string[] {
		return this.#serverList.map(formatServer);
	}

	setServers(servers: string[]): void {
		this.#servers = parseServers(servers);
	}

	setLocalAddress(ipv4?: string, ipv6?: string): void {
		if (ipv4 !== undefined) {
			validateString(ipv4, "ipv4");
			if (isIP(ipv4) !== 4) throw codedError("ERR_INVALID_ARG_VALUE", "Invalid IP address.", TypeError);
		}
		if (ipv6 !== undefined) {
			validateString(ipv6, "ipv6");
			if (isIP(ipv6) !== 6) throw codedError("ERR_INVALID_ARG_VALUE", "Invalid IP address.", TypeError);
		}
	}

	cancel(): void {
		for (const p of this.#pending) p.fail(dnsException(CANCELLED, "query"));
		this.#pending.clear();
	}

	/** one DNS query over TCP, trying each server `tries` times */
	[kQuery](name: string, type: string, syscall: string): Promise<Answer[]> {
		// node:net imports node:dns, whose top-level resolver needs this module initialized.
		const net = require("node:net") as NetModule;
		const servers = this.#serverList;
		const qtype = TYPES[type];
		const { promise, resolve, reject } = Promise.withResolvers<Answer[]>();
		let attempt = 0;
		let lastCode = TIMEOUT;
		const tryNext = () => {
			if (attempt >= servers.length * this.#tries) {
				reject(dnsException(lastCode, syscall, name));
				return;
			}
			const [, address, port] = servers[attempt % servers.length];
			attempt++;
			const id = (Math.random() * 0x10000) | 0;
			let query: Uint8Array;
			try {
				query = encodeQuery(id, name, qtype);
			} catch (e) {
				reject(e);
				return;
			}
			const socket = new net.Socket();
			let chunks = Buffer.alloc(0);
			let settled = false;
			const entry = {
				socket,
				fail: (err: Error) => {
					if (settled) return;
					settled = true;
					socket.destroy();
					reject(err);
				},
			};
			this.#pending.add(entry);
			const done = (fn: () => void) => {
				if (settled) return;
				settled = true;
				this.#pending.delete(entry);
				socket.destroy();
				fn();
			};
			socket.setTimeout(this.#timeout, () => {
				lastCode = TIMEOUT;
				done(tryNext);
			});
			socket.on("error", (err: unknown) => {
				lastCode = (err as SystemError).code === "ECONNREFUSED" ? CONNREFUSED : TIMEOUT;
				done(tryNext);
			});
			socket.on("close", () => {
				lastCode = BADRESP;
				done(tryNext);
			});
			socket.on("connect", () => {
				const framed = new Uint8Array(query.length + 2);
				framed[0] = query.length >> 8;
				framed[1] = query.length & 255;
				framed.set(query, 2);
				socket.write(framed);
			});
			socket.on("data", (chunk: unknown) => {
				chunks = Buffer.concat([chunks, chunk as Buffer]);
				if (chunks.length < 2) return;
				const len = (chunks[0] << 8) | chunks[1];
				if (chunks.length < len + 2) return;
				const msg = chunks.subarray(2, 2 + len);
				done(() => {
					try {
						resolve(decodeResponse(msg, id, syscall, name));
					} catch (e) {
						reject(e);
					}
				});
			});
			socket.connect(port, address);
		};
		tryNext();
		return promise;
	}

	async #records(name: string, type: string): Promise<Answer[]> {
		validateString(name, "name");
		const syscall = `query${type[0]}${type.slice(1).toLowerCase()}`;
		const answers = await this[kQuery](name, type, syscall);
		const wanted = TYPES[type];
		const list = type === "ANY" ? answers : answers.filter((a) => a.type === wanted);
		if (list.length === 0) throw dnsException(NODATA, syscall, name);
		return list;
	}

	async #addresses(name: string, family: 4 | 6, options?: ResolveOptions): Promise<string[] | RecordWithTtl[]> {
		validateString(name, "name");
		const syscall = family === 4 ? "queryA" : "queryAaaa";
		const ttl = Boolean(options?.ttl);
		const { promise, resolve, reject } = Promise.withResolvers<string[] | RecordWithTtl[]>();
		const ip = isIP(name);
		if (ip) {
			if (ip !== family) reject(dnsException(NODATA, syscall, name));
			else resolve(ttl ? [{ address: name, ttl: 0 }] : [name]);
			return promise;
		}
		getaddrinfo(name, { family, hints: 0, all: true, order: "verbatim" }, (err, addrs) => {
			if (err) {
				const code = (err as SystemError).code ?? NOTFOUND;
				reject(dnsException(code === "EAI_AGAIN" ? TIMEOUT : code, syscall, name));
				return;
			}
			const list = addrs.filter((a) => a.family === family);
			if (list.length === 0) reject(dnsException(NODATA, syscall, name));
			else resolve(ttl ? list.map((a) => ({ address: a.address, ttl: 0 })) : list.map((a) => a.address));
		});
		return promise;
	}

	resolve4(hostname: string, options?: ResolveOptions): Promise<string[] | RecordWithTtl[]> {
		return this.#addresses(hostname, 4, options);
	}

	resolve6(hostname: string, options?: ResolveOptions): Promise<string[] | RecordWithTtl[]> {
		return this.#addresses(hostname, 6, options);
	}

	async resolveAny(hostname: string): Promise<AnyRecord[]> {
		const answers = await this.#records(hostname, "ANY");
		const out: AnyRecord[] = [];
		for (const a of answers) {
			const type = TYPE_NAMES[a.type];
			if (!type) continue;
			switch (a.type) {
				case 1:
				case 28:
					out.push({ address: a.data as string, ttl: a.ttl, type });
					break;
				case 2:
				case 5:
				case 12:
					out.push({ value: a.data as string, type });
					break;
				case 16:
					out.push({ entries: a.data as string[], type });
					break;
				default:
					out.push({ ...(a.data as Record<string, unknown>), type });
			}
		}
		return out;
	}

	async resolveCaa(hostname: string): Promise<Record<string, unknown>[]> {
		return (await this.#records(hostname, "CAA")).map((a) => a.data as Record<string, unknown>);
	}

	async resolveCname(hostname: string): Promise<string[]> {
		return (await this.#records(hostname, "CNAME")).map((a) => a.data as string);
	}

	async resolveMx(hostname: string): Promise<{ priority: number; exchange: string }[]> {
		return (await this.#records(hostname, "MX")).map((a) => a.data as { priority: number; exchange: string });
	}

	async resolveNaptr(hostname: string): Promise<Record<string, unknown>[]> {
		return (await this.#records(hostname, "NAPTR")).map((a) => a.data as Record<string, unknown>);
	}

	async resolveNs(hostname: string): Promise<string[]> {
		return (await this.#records(hostname, "NS")).map((a) => a.data as string);
	}

	async resolvePtr(hostname: string): Promise<string[]> {
		return (await this.#records(hostname, "PTR")).map((a) => a.data as string);
	}

	async resolveSoa(hostname: string): Promise<Record<string, unknown>> {
		return (await this.#records(hostname, "SOA"))[0].data as Record<string, unknown>;
	}

	async resolveSrv(hostname: string): Promise<{ priority: number; weight: number; port: number; name: string }[]> {
		return (await this.#records(hostname, "SRV")).map(
			(a) => a.data as { priority: number; weight: number; port: number; name: string },
		);
	}

	async resolveTlsa(hostname: string): Promise<Record<string, unknown>[]> {
		return (await this.#records(hostname, "TLSA")).map((a) => a.data as Record<string, unknown>);
	}

	async resolveTxt(hostname: string): Promise<string[][]> {
		return (await this.#records(hostname, "TXT")).map((a) => a.data as string[]);
	}

	resolve(hostname: string, rrtype = "A"): Promise<unknown> {
		validateString(rrtype, "rrtype");
		switch (rrtype) {
			case "A":
				return this.resolve4(hostname);
			case "AAAA":
				return this.resolve6(hostname);
			case "ANY":
				return this.resolveAny(hostname);
			case "CAA":
				return this.resolveCaa(hostname);
			case "CNAME":
				return this.resolveCname(hostname);
			case "MX":
				return this.resolveMx(hostname);
			case "NAPTR":
				return this.resolveNaptr(hostname);
			case "NS":
				return this.resolveNs(hostname);
			case "PTR":
				return this.resolvePtr(hostname);
			case "SOA":
				return this.resolveSoa(hostname);
			case "SRV":
				return this.resolveSrv(hostname);
			case "TLSA":
				return this.resolveTlsa(hostname);
			case "TXT":
				return this.resolveTxt(hostname);
		}
		throw invalidArgValue("rrtype", rrtype);
	}

	async reverse(ip: string): Promise<string[]> {
		validateString(ip, "ip");
		const family = isIP(ip);
		if (!family) throw codedError("ERR_INVALID_ARG_VALUE", `The argument 'ip' is invalid. Received '${ip}'`, TypeError);
		let name: string;
		if (family === 4) {
			name = `${ip.split(".").reverse().join(".")}.in-addr.arpa`;
		} else {
			const v = parseIPv6(ip) as bigint;
			name = `${v.toString(16).padStart(32, "0").split("").reverse().join(".")}.ip6.arpa`;
		}
		const answers = await this[kQuery](name, "PTR", "getHostByAddr");
		const list = answers.filter((a) => a.type === 12).map((a) => a.data as string);
		if (list.length === 0) throw dnsException(NOTFOUND, "getHostByAddr", ip);
		return list;
	}
}

const defaultResolver = new Resolver();

export function getServers(): string[] {
	return defaultResolver.getServers();
}

export function setServers(servers: string[]): void {
	defaultResolver.setServers(servers);
}

export function getDefaultResolver(): Resolver {
	return defaultResolver;
}

export const resolve = (hostname: string, rrtype?: string): Promise<unknown> =>
	defaultResolver.resolve(hostname, rrtype);
export const resolve4 = (hostname: string, options?: ResolveOptions): Promise<string[] | RecordWithTtl[]> =>
	defaultResolver.resolve4(hostname, options);
export const resolve6 = (hostname: string, options?: ResolveOptions): Promise<string[] | RecordWithTtl[]> =>
	defaultResolver.resolve6(hostname, options);
export const resolveAny = (hostname: string): Promise<AnyRecord[]> => defaultResolver.resolveAny(hostname);
export const resolveCaa = (hostname: string): Promise<Record<string, unknown>[]> =>
	defaultResolver.resolveCaa(hostname);
export const resolveCname = (hostname: string): Promise<string[]> => defaultResolver.resolveCname(hostname);
export const resolveMx = (hostname: string): Promise<{ priority: number; exchange: string }[]> =>
	defaultResolver.resolveMx(hostname);
export const resolveNaptr = (hostname: string): Promise<Record<string, unknown>[]> =>
	defaultResolver.resolveNaptr(hostname);
export const resolveNs = (hostname: string): Promise<string[]> => defaultResolver.resolveNs(hostname);
export const resolvePtr = (hostname: string): Promise<string[]> => defaultResolver.resolvePtr(hostname);
export const resolveSoa = (hostname: string): Promise<Record<string, unknown>> => defaultResolver.resolveSoa(hostname);
export const resolveSrv = (
	hostname: string,
): Promise<{ priority: number; weight: number; port: number; name: string }[]> => defaultResolver.resolveSrv(hostname);
export const resolveTlsa = (hostname: string): Promise<Record<string, unknown>[]> =>
	defaultResolver.resolveTlsa(hostname);
export const resolveTxt = (hostname: string): Promise<string[][]> => defaultResolver.resolveTxt(hostname);
export const reverse = (ip: string): Promise<string[]> => defaultResolver.reverse(ip);

export default {
	lookup,
	lookupService,
	Resolver,
	getServers,
	setServers,
	setDefaultResultOrder,
	getDefaultResultOrder,
	resolve,
	resolve4,
	resolve6,
	resolveAny,
	resolveCaa,
	resolveCname,
	resolveMx,
	resolveNaptr,
	resolveNs,
	resolvePtr,
	resolveSoa,
	resolveSrv,
	resolveTlsa,
	resolveTxt,
	reverse,
	constants,
	...constants,
};
