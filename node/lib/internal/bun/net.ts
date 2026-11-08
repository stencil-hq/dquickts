// internal:bun/net — Bun.serve, Bun.connect, Bun.listen, Bun.udpSocket, Bun.dns.

import * as core from "qbun:core";
import { notSupported } from "internal:errors";
import { serve as serveImpl } from "internal:http/bun-serve";
import { connect as connectImpl, listen as listenImpl } from "internal:http/bun-socket";
import { lookup as dnsLookup } from "node:dns/promises";
import type { ServeOptions, BunServer } from "internal:http/bun-serve";
import type { SocketOptions, BunSocket, BunListener } from "internal:http/bun-socket";

export function serve(options: ServeOptions): BunServer {
	return serveImpl(options);
}

export function connect(options: SocketOptions): Promise<BunSocket> {
	return connectImpl(options);
}

export function listen(options: SocketOptions): BunListener {
	return listenImpl(options);
}

/** UDP needs a datagram binding qbun:net does not have */
export function udpSocket(_options?: unknown): Promise<never> {
	return Promise.reject(notSupported("Bun.udpSocket"));
}

// ---------------------------------------------------------------------------
// Bun.dns

interface DNSLookup {
	address: string;
	family: 4 | 6;
	ttl: number;
}

interface DnsLookupOptions {
	family?: 4 | 6 | 0 | "IPv4" | "IPv6" | "any";
	socketType?: "udp" | "tcp";
	flags?: number;
	port?: number;
	backend?: "libc" | "c-ares" | "system" | "getaddrinfo";
}

interface CacheEntry {
	expires: number;
	result: Promise<DNSLookup[]>;
	settled: boolean;
}

const CACHE_TTL_MS = 30_000;
const CACHE_MAX = 256;
const dnsCache = new Map<string, CacheEntry>();
const stats = { cacheHitsCompleted: 0, cacheHitsInflight: 0, cacheMisses: 0, size: 0, errors: 0, totalCount: 0 };

function familyOf(family: DnsLookupOptions["family"]): 0 | 4 | 6 {
	switch (family) {
		case 4:
		case "IPv4":
			return 4;
		case 6:
		case "IPv6":
			return 6;
	}
	return 0;
}

function lookup(hostname: string, options: DnsLookupOptions = {}): Promise<DNSLookup[]> {
	if (typeof hostname !== "string" || hostname.length === 0) {
		return Promise.reject(new TypeError("Expected hostname to be a non-empty string"));
	}
	stats.totalCount++;
	const family = familyOf(options.family);
	const flags = options.flags ?? 0;
	const key = `${hostname}\0${family}\0${flags}`;
	const now = Date.now();
	const hit = dnsCache.get(key);
	if (hit && hit.expires > now) {
		if (hit.settled) stats.cacheHitsCompleted++;
		else stats.cacheHitsInflight++;
		return hit.result;
	}
	stats.cacheMisses++;
	const entry: CacheEntry = { expires: now + CACHE_TTL_MS, settled: false, result: Promise.resolve([]) };
	entry.result = dnsLookup(hostname, { family, all: true, hints: flags })
		.then(
			(list) => {
				entry.settled = true;
				return list.map((a) => ({ address: a.address, family: a.family, ttl: 0 }));
			},
			(err: unknown) => {
				entry.settled = true;
				stats.errors++;
				dnsCache.delete(key);
				stats.size = dnsCache.size;
				throw err;
			},
		);
	if (dnsCache.size >= CACHE_MAX) {
		const oldest = dnsCache.keys().next().value;
		if (oldest !== undefined) dnsCache.delete(oldest);
	}
	dnsCache.set(key, entry);
	stats.size = dnsCache.size;
	return entry.result;
}

function prefetch(hostname: string, _port?: number): void {
	lookup(hostname).catch(() => {});
}

function getCacheStats(): typeof stats {
	return { ...stats };
}

const darwin = (core.info() as { platform: string }).platform === "darwin";

export const dns = {
	lookup,
	prefetch,
	getCacheStats,
	ADDRCONFIG: darwin ? 0x400 : 0x20,
	ALL: darwin ? 0x100 : 0x10,
	V4MAPPED: darwin ? 0x800 : 0x8,
};
