// node:dns — callback API over the implementation in node:dns/promises.

import process from "node:process";
import { bind } from "internal:async-context";
import { isIP } from "internal:http/ip";
import { invalidArgType, invalidArgValue, validateFunction, validateString } from "internal:errors";
import promises, {
	Resolver as PromiseResolver,
	constants,
	getaddrinfo,
	getnameinfo,
	parseLookupOptions,
	validateHostname,
	validatePortArg,
	setDefaultResultOrder,
	getDefaultResultOrder,
	getDefaultResolver,
	type LookupAddress,
	type LookupOptions,
	type ResolveOptions,
} from "node:dns/promises";

export {
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
	constants,
	setDefaultResultOrder,
	getDefaultResultOrder,
} from "node:dns/promises";

export { promises };

type LookupCallback = (err: Error | null, address?: string | LookupAddress[], family?: number) => void;

export function lookup(
	hostname: string,
	options: LookupOptions | number | LookupCallback | undefined,
	callback?: LookupCallback,
): object {
	validateHostname(hostname);
	let cb: LookupCallback;
	let opts: unknown = options;
	if (typeof options === "function") {
		cb = options;
		opts = undefined;
	} else {
		if (options !== undefined && options !== null && typeof options !== "object" && typeof options !== "number") {
			validateFunction(callback, "callback");
			throw invalidArgType("options", "of type number or an instance of Object", options);
		}
		validateFunction(callback, "callback");
		cb = callback as LookupCallback;
	}
	const parsed = parseLookupOptions(opts);
	if (!hostname) throw invalidArgValue("hostname", hostname, "must be a non-empty string");
	const matched = isIP(hostname);
	if (matched) {
		if (parsed.all) process.nextTick(cb, null, [{ address: hostname, family: matched }]);
		else process.nextTick(cb, null, hostname, matched);
		return {};
	}
	const bound = bind(cb);
	getaddrinfo(hostname, parsed, (err, addrs) => {
		if (err) bound(err);
		else if (parsed.all) bound(null, addrs);
		else bound(null, addrs[0].address, addrs[0].family);
	});
	return {};
}

export function lookupService(
	address: string,
	port: number,
	callback: (err: Error | null, hostname?: string, service?: string) => void,
): object {
	if (arguments.length !== 3) throw codedMissingArgs();
	validateString(address, "address");
	if (isIP(address) === 0) throw invalidArgValue("address", address);
	const p = validatePortArg(port);
	validateFunction(callback, "callback");
	const bound = bind(callback);
	getnameinfo(address, p, (err, hostname, service) => {
		if (err) bound(err);
		else bound(null, hostname, service);
	});
	return {};
}

function codedMissingArgs(): Error {
	const err = new TypeError('The "address", "port", and "callback" arguments must be specified') as TypeError & {
		code: string;
	};
	err.code = "ERR_MISSING_ARGS";
	return err;
}

type NodeCallback<T> = (err: Error | null, result?: T) => void;

/** run a promise-returning resolver method and report to a Node callback outside the promise chain */
function toCallback<T>(promise: Promise<T>, callback: NodeCallback<T>): void {
	const bound = bind(callback);
	promise.then(
		(value) => process.nextTick(bound, null, value),
		(err: Error) => process.nextTick(bound, err),
	);
}

type ResolveMethod =
	| "resolveAny"
	| "resolveCaa"
	| "resolveCname"
	| "resolveMx"
	| "resolveNaptr"
	| "resolveNs"
	| "resolvePtr"
	| "resolveSoa"
	| "resolveSrv"
	| "resolveTlsa"
	| "resolveTxt"
	| "reverse";

/** callback-style Resolver: same methods as the promise Resolver */
export class Resolver {
	#inner: PromiseResolver;

	constructor(options?: { timeout?: number; tries?: number }) {
		this.#inner = new PromiseResolver(options);
	}

	/** @internal wrap an existing promise resolver (the default one) */
	static _wrap(inner: PromiseResolver): Resolver {
		const r = new Resolver();
		r.#inner = inner;
		return r;
	}

	getServers(): string[] {
		return this.#inner.getServers();
	}

	setServers(servers: string[]): void {
		this.#inner.setServers(servers);
	}

	setLocalAddress(ipv4?: string, ipv6?: string): void {
		this.#inner.setLocalAddress(ipv4, ipv6);
	}

	cancel(): void {
		this.#inner.cancel();
	}

	#call<T>(method: ResolveMethod, hostname: string, callback: unknown): object {
		validateString(hostname, method === "reverse" ? "ip" : "name");
		validateFunction(callback, "callback");
		toCallback(
			(this.#inner[method] as (h: string) => Promise<T>).call(this.#inner, hostname),
			callback as NodeCallback<T>,
		);
		return {};
	}

	resolve(hostname: string, rrtype: string | NodeCallback<unknown>, callback?: NodeCallback<unknown>): object {
		let type = "A";
		let cb = callback;
		if (typeof rrtype === "function") cb = rrtype;
		else if (typeof rrtype === "string") type = rrtype;
		else if (rrtype !== undefined) throw invalidArgType("rrtype", "of type string", rrtype);
		validateString(hostname, "name");
		validateFunction(cb, "callback");
		toCallback(this.#inner.resolve(hostname, type), cb as NodeCallback<unknown>);
		return {};
	}

	resolve4(
		hostname: string,
		options: ResolveOptions | NodeCallback<unknown>,
		callback?: NodeCallback<unknown>,
	): object {
		const cb = typeof options === "function" ? options : callback;
		validateString(hostname, "name");
		validateFunction(cb, "callback");
		toCallback(
			this.#inner.resolve4(hostname, typeof options === "object" ? options : undefined),
			cb as NodeCallback<unknown>,
		);
		return {};
	}

	resolve6(
		hostname: string,
		options: ResolveOptions | NodeCallback<unknown>,
		callback?: NodeCallback<unknown>,
	): object {
		const cb = typeof options === "function" ? options : callback;
		validateString(hostname, "name");
		validateFunction(cb, "callback");
		toCallback(
			this.#inner.resolve6(hostname, typeof options === "object" ? options : undefined),
			cb as NodeCallback<unknown>,
		);
		return {};
	}

	resolveAny(hostname: string, callback: NodeCallback<unknown>): object {
		return this.#call("resolveAny", hostname, callback);
	}

	resolveCaa(hostname: string, callback: NodeCallback<unknown>): object {
		return this.#call("resolveCaa", hostname, callback);
	}

	resolveCname(hostname: string, callback: NodeCallback<unknown>): object {
		return this.#call("resolveCname", hostname, callback);
	}

	resolveMx(hostname: string, callback: NodeCallback<unknown>): object {
		return this.#call("resolveMx", hostname, callback);
	}

	resolveNaptr(hostname: string, callback: NodeCallback<unknown>): object {
		return this.#call("resolveNaptr", hostname, callback);
	}

	resolveNs(hostname: string, callback: NodeCallback<unknown>): object {
		return this.#call("resolveNs", hostname, callback);
	}

	resolvePtr(hostname: string, callback: NodeCallback<unknown>): object {
		return this.#call("resolvePtr", hostname, callback);
	}

	resolveSoa(hostname: string, callback: NodeCallback<unknown>): object {
		return this.#call("resolveSoa", hostname, callback);
	}

	resolveSrv(hostname: string, callback: NodeCallback<unknown>): object {
		return this.#call("resolveSrv", hostname, callback);
	}

	resolveTlsa(hostname: string, callback: NodeCallback<unknown>): object {
		return this.#call("resolveTlsa", hostname, callback);
	}

	resolveTxt(hostname: string, callback: NodeCallback<unknown>): object {
		return this.#call("resolveTxt", hostname, callback);
	}

	reverse(ip: string, callback: NodeCallback<unknown>): object {
		return this.#call("reverse", ip, callback);
	}
}

const defaultResolver = Resolver._wrap(getDefaultResolver());

export function getServers(): string[] {
	return defaultResolver.getServers();
}

export function setServers(servers: string[]): void {
	defaultResolver.setServers(servers);
}

export const resolve = defaultResolver.resolve.bind(defaultResolver);
export const resolve4 = defaultResolver.resolve4.bind(defaultResolver);
export const resolve6 = defaultResolver.resolve6.bind(defaultResolver);
export const resolveAny = defaultResolver.resolveAny.bind(defaultResolver);
export const resolveCaa = defaultResolver.resolveCaa.bind(defaultResolver);
export const resolveCname = defaultResolver.resolveCname.bind(defaultResolver);
export const resolveMx = defaultResolver.resolveMx.bind(defaultResolver);
export const resolveNaptr = defaultResolver.resolveNaptr.bind(defaultResolver);
export const resolveNs = defaultResolver.resolveNs.bind(defaultResolver);
export const resolvePtr = defaultResolver.resolvePtr.bind(defaultResolver);
export const resolveSoa = defaultResolver.resolveSoa.bind(defaultResolver);
export const resolveSrv = defaultResolver.resolveSrv.bind(defaultResolver);
export const resolveTlsa = defaultResolver.resolveTlsa.bind(defaultResolver);
export const resolveTxt = defaultResolver.resolveTxt.bind(defaultResolver);
export const reverse = defaultResolver.reverse.bind(defaultResolver);

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
	promises,
	...constants,
};
