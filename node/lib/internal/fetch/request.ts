// WHATWG Request.

import { URL } from "node:url";
import { AbortController, AbortSignal } from "internal:event-target";
import { Body, bodyStream, cloneBody, extractBody, isUsed, kBody } from "internal:fetch/body";
import type { BodyInit, BodyState } from "internal:fetch/body";
import { Headers, cloneHeaders, fillHeaders, isToken, setGuard, setRaw } from "internal:fetch/headers";
import type { HeadersInit } from "internal:fetch/headers";

export type RequestRedirect = "follow" | "error" | "manual";

export interface RequestInit {
	method?: string;
	headers?: HeadersInit;
	body?: BodyInit | null;
	signal?: AbortSignal | null;
	redirect?: RequestRedirect;
	mode?: string;
	credentials?: string;
	cache?: string;
	referrer?: string;
	referrerPolicy?: string;
	integrity?: string;
	keepalive?: boolean;
	duplex?: "half";
	priority?: string;
	window?: null;
}

interface RequestState {
	method: string;
	url: string;
	headers: Headers;
	/** the signal given by the user (fetch follows it directly) */
	userSignal: AbortSignal | null;
	signal: AbortSignal | null;
	redirect: RequestRedirect;
	mode: string;
	credentials: string;
	cache: string;
	referrer: string;
	referrerPolicy: string;
	integrity: string;
	keepalive: boolean;
	duplex: "half";
	priority: string;
}

const kState = Symbol("request state");

const NORMALIZED_METHODS: Record<string, string> = {
	delete: "DELETE",
	get: "GET",
	head: "HEAD",
	options: "OPTIONS",
	post: "POST",
	put: "PUT",
	patch: "PATCH",
};

const REDIRECTS = ["follow", "error", "manual"];
const MODES = ["navigate", "same-origin", "no-cors", "cors"];
const CREDENTIALS = ["omit", "same-origin", "include"];
const CACHES = ["default", "no-store", "reload", "no-cache", "force-cache", "only-if-cached"];
const POLICIES = [
	"",
	"no-referrer",
	"no-referrer-when-downgrade",
	"same-origin",
	"origin",
	"strict-origin",
	"origin-when-cross-origin",
	"strict-origin-when-cross-origin",
	"unsafe-url",
];

export function normalizeMethod(method: string): string {
	const m = String(method);
	if (!isToken(m)) throw new TypeError(`'${m}' is not a valid HTTP method.`);
	const upper = m.toUpperCase();
	if (upper === "CONNECT" || upper === "TRACE" || upper === "TRACK") {
		throw new TypeError(`'${m}' HTTP method is unsupported.`);
	}
	return NORMALIZED_METHODS[m.toLowerCase()] ?? m;
}

function enumValue(value: unknown, allowed: string[], name: string): string {
	const s = String(value);
	if (!allowed.includes(s)) throw new TypeError(`Request constructor: '${s}' is not a valid value for ${name}`);
	return s;
}

export class Request extends Body {
	[kState]: RequestState;

	constructor(input: string | URL | Request, init: RequestInit = {}) {
		super();
		if (arguments.length === 0) throw new TypeError("Request constructor: 1 argument required, but only 0 present.");
		if (init === null || init === undefined) init = {};
		else if (typeof init !== "object") throw new TypeError("Request constructor: init must be an object");
		let base: RequestState | null = null;
		let url: string;
		if (input instanceof Request) {
			base = input[kState];
			url = base.url;
		} else {
			let parsed: URL;
			try {
				parsed = new URL(String(input));
			} catch {
				throw new TypeError(`Failed to parse URL from ${String(input)}`);
			}
			url = parsed.href;
		}
		let method = base?.method ?? "GET";
		if (init.method !== undefined) method = normalizeMethod(init.method);
		const signal = init.signal !== undefined ? init.signal : (base?.userSignal ?? null);
		if (signal !== null && !(signal instanceof AbortSignal)) {
			throw new TypeError("Request constructor: signal must be an AbortSignal");
		}
		const headers = new Headers();
		if (init.headers !== undefined) fillHeaders(headers, init.headers);
		else if (base) fillHeaders(headers, base.headers);
		setGuard(headers, "request");
		this[kState] = {
			method,
			url,
			headers,
			userSignal: signal,
			signal: null,
			redirect:
				init.redirect !== undefined
					? (enumValue(init.redirect, REDIRECTS, "RequestRedirect") as RequestRedirect)
					: (base?.redirect ?? "follow"),
			mode: init.mode !== undefined ? enumValue(init.mode, MODES, "RequestMode") : (base?.mode ?? "cors"),
			credentials:
				init.credentials !== undefined
					? enumValue(init.credentials, CREDENTIALS, "RequestCredentials")
					: (base?.credentials ?? "include"),
			cache: init.cache !== undefined ? enumValue(init.cache, CACHES, "RequestCache") : (base?.cache ?? "default"),
			referrer: init.referrer !== undefined ? String(init.referrer) || "" : (base?.referrer ?? "about:client"),
			referrerPolicy:
				init.referrerPolicy !== undefined
					? enumValue(init.referrerPolicy, POLICIES, "ReferrerPolicy")
					: (base?.referrerPolicy ?? ""),
			integrity: init.integrity !== undefined ? String(init.integrity) : (base?.integrity ?? ""),
			keepalive: init.keepalive !== undefined ? !!init.keepalive : (base?.keepalive ?? false),
			duplex: "half",
			priority: init.priority !== undefined ? String(init.priority) : (base?.priority ?? "auto"),
		};
		Object.defineProperty(this, kState, { enumerable: false });
		if (this[kState].mode === "navigate") this[kState].mode = "same-origin";

		const hasInitBody = init.body !== undefined && init.body !== null;
		const inputBody = input instanceof Request ? input[kBody] : null;
		const hasInputBody =
			inputBody !== null && (inputBody.source !== null || inputBody.stream !== null || inputBody.native !== null);
		if ((hasInitBody || (hasInputBody && init.body === undefined)) && (method === "GET" || method === "HEAD")) {
			throw new TypeError("Request with GET/HEAD method cannot have body.");
		}
		if (hasInitBody) {
			const { state, type } = extractBody(init.body);
			this[kBody] = state;
			if (type !== null && !headers.has("content-type")) setRaw(headers, "Content-Type", type);
		} else if (hasInputBody && init.body === undefined) {
			if (isUsed(inputBody!))
				throw new TypeError("Cannot construct a Request with a Request object that has already been used.");
			// the body moves to the new request; the input becomes used
			const state: BodyState = { ...inputBody! };
			if (inputBody!.stream || inputBody!.native) {
				state.stream = bodyStream(inputBody!);
				state.native = null;
				inputBody!.used = true;
			}
			this[kBody] = state;
		}
	}

	get method(): string {
		return this[kState].method;
	}

	get url(): string {
		return this[kState].url;
	}

	get headers(): Headers {
		return this[kState].headers;
	}

	get destination(): string {
		return "";
	}

	get referrer(): string {
		return this[kState].referrer;
	}

	get referrerPolicy(): string {
		return this[kState].referrerPolicy;
	}

	get mode(): string {
		return this[kState].mode;
	}

	get credentials(): string {
		return this[kState].credentials;
	}

	get cache(): string {
		return this[kState].cache;
	}

	get redirect(): RequestRedirect {
		return this[kState].redirect;
	}

	get integrity(): string {
		return this[kState].integrity;
	}

	get keepalive(): boolean {
		return this[kState].keepalive;
	}

	get isReloadNavigation(): boolean {
		return false;
	}

	get isHistoryNavigation(): boolean {
		return false;
	}

	get duplex(): "half" {
		return this[kState].duplex;
	}

	get signal(): AbortSignal {
		const s = this[kState];
		if (s.userSignal) return s.userSignal;
		if (!s.signal) s.signal = new AbortController().signal;
		return s.signal;
	}

	clone(): Request {
		const copy = new Request(this, { body: null });
		copy[kState] = { ...this[kState], headers: cloneHeaders(this[kState].headers) };
		copy[kBody] = cloneBody(this[kBody]);
		return copy;
	}

	get [Symbol.toStringTag](): string {
		return "Request";
	}

	[Symbol.for("nodejs.util.inspect.custom")](
		depth: number,
		options: object,
		inspect: (v: unknown, o?: object) => string,
	): string {
		const s = this[kState];
		const shown = {
			method: s.method,
			url: s.url,
			headers: s.headers,
			redirect: s.redirect,
			bodyUsed: this.bodyUsed,
		};
		return `Request ${inspect(shown, options)}`;
	}
}

/** The internal fields fetch() needs. */
export function requestState(request: Request): RequestState {
	return request[kState];
}
