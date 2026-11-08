// WHATWG Response.

import { URL } from "node:url";
import { Body, cloneBody, emptyBody, extractBody, kBody } from "internal:fetch/body";
import type { BodyInit, BodyState } from "internal:fetch/body";
import { Headers, cloneHeaders, fillHeaders, setGuard, setRaw } from "internal:fetch/headers";
import type { HeadersInit } from "internal:fetch/headers";

export type ResponseType = "basic" | "cors" | "default" | "error" | "opaque" | "opaqueredirect";

export interface ResponseInit {
	status?: number;
	statusText?: string;
	headers?: HeadersInit;
}

export interface ResponseState {
	type: ResponseType;
	url: string;
	redirected: boolean;
	status: number;
	statusText: string;
	headers: Headers;
}

const kState = Symbol("response state");
const NULL_BODY_STATUS = new Set([101, 103, 204, 205, 304]);
const REDIRECT_STATUS = new Set([301, 302, 303, 307, 308]);
const REASON_PHRASE = /^[\t\x20-\x7e\x80-\xff]*$/;

export const STATUS_TEXT: Record<number, string> = {
	100: "Continue",
	101: "Switching Protocols",
	102: "Processing",
	103: "Early Hints",
	200: "OK",
	201: "Created",
	202: "Accepted",
	203: "Non-Authoritative Information",
	204: "No Content",
	205: "Reset Content",
	206: "Partial Content",
	207: "Multi-Status",
	208: "Already Reported",
	226: "IM Used",
	300: "Multiple Choices",
	301: "Moved Permanently",
	302: "Found",
	303: "See Other",
	304: "Not Modified",
	305: "Use Proxy",
	307: "Temporary Redirect",
	308: "Permanent Redirect",
	400: "Bad Request",
	401: "Unauthorized",
	402: "Payment Required",
	403: "Forbidden",
	404: "Not Found",
	405: "Method Not Allowed",
	406: "Not Acceptable",
	407: "Proxy Authentication Required",
	408: "Request Timeout",
	409: "Conflict",
	410: "Gone",
	411: "Length Required",
	412: "Precondition Failed",
	413: "Payload Too Large",
	414: "URI Too Long",
	415: "Unsupported Media Type",
	416: "Range Not Satisfiable",
	417: "Expectation Failed",
	418: "I'm a Teapot",
	421: "Misdirected Request",
	422: "Unprocessable Entity",
	423: "Locked",
	424: "Failed Dependency",
	425: "Too Early",
	426: "Upgrade Required",
	428: "Precondition Required",
	429: "Too Many Requests",
	431: "Request Header Fields Too Large",
	451: "Unavailable For Legal Reasons",
	500: "Internal Server Error",
	501: "Not Implemented",
	502: "Bad Gateway",
	503: "Service Unavailable",
	504: "Gateway Timeout",
	505: "HTTP Version Not Supported",
	506: "Variant Also Negotiates",
	507: "Insufficient Storage",
	508: "Loop Detected",
	509: "Bandwidth Limit Exceeded",
	510: "Not Extended",
	511: "Network Authentication Required",
};

export function isNullBodyStatus(status: number): boolean {
	return NULL_BODY_STATUS.has(status);
}

export function isRedirectStatus(status: number): boolean {
	return REDIRECT_STATUS.has(status);
}

function initResponse(
	response: Response,
	init: ResponseInit,
	body: { state: BodyState; type: string | null } | null,
): void {
	const status = init.status === undefined ? 200 : Number(init.status);
	if (!Number.isInteger(status) || status < 200 || status > 599) {
		throw new RangeError(`The status provided (${init.status}) must be 200 or in the range of 201 to 599`);
	}
	const statusText = init.statusText === undefined ? "" : String(init.statusText);
	if (!REASON_PHRASE.test(statusText)) throw new TypeError(`Invalid statusText: "${statusText}"`);
	const headers = new Headers();
	if (init.headers !== undefined && init.headers !== null) fillHeaders(headers, init.headers);
	setGuard(headers, "response");
	response[kState] = { type: "default", url: "", redirected: false, status, statusText, headers };
	if (body) {
		if (NULL_BODY_STATUS.has(status)) {
			throw new TypeError(`Response constructor: Invalid response status code ${status}`);
		}
		response[kBody] = body.state;
		if (body.type !== null && !headers.has("content-type")) setRaw(headers, "Content-Type", body.type);
	}
}

export class Response extends Body {
	[kState]: ResponseState;

	constructor(body: BodyInit | null = null, init: ResponseInit = {}) {
		super();
		if (init === null || init === undefined) init = {};
		else if (typeof init !== "object") throw new TypeError("Response constructor: init must be an object");
		Object.defineProperty(this, kState, { enumerable: false, writable: true });
		initResponse(this, init, body === null || body === undefined ? null : extractBody(body));
	}

	static error(): Response {
		const response = new Response();
		const headers = new Headers();
		setGuard(headers, "immutable");
		response[kState] = { type: "error", url: "", redirected: false, status: 0, statusText: "", headers };
		return response;
	}

	static redirect(url: string | URL, status = 302): Response {
		let parsed: URL;
		try {
			parsed = new URL(String(url));
		} catch {
			throw new TypeError(`Failed to parse URL from ${String(url)}`);
		}
		if (!REDIRECT_STATUS.has(status)) throw new RangeError(`Invalid status code: ${status}`);
		const response = new Response(null, { status });
		setRaw(response[kState].headers, "Location", parsed.href);
		setGuard(response[kState].headers, "immutable");
		return response;
	}

	static json(data: unknown, init: ResponseInit | number = {}): Response {
		const text = JSON.stringify(data);
		if (text === undefined) throw new TypeError("Value is not JSON serializable");
		const options: ResponseInit = typeof init === "number" ? { status: init } : (init ?? {});
		const response = new Response(null, options);
		const body = extractBody(text);
		if (NULL_BODY_STATUS.has(response[kState].status)) {
			throw new TypeError(`Response constructor: Invalid response status code ${response[kState].status}`);
		}
		response[kBody] = body.state;
		if (!response[kState].headers.has("content-type"))
			setRaw(response[kState].headers, "Content-Type", "application/json");
		return response;
	}

	get type(): ResponseType {
		return this[kState].type;
	}

	get url(): string {
		return this[kState].url;
	}

	get redirected(): boolean {
		return this[kState].redirected;
	}

	get status(): number {
		return this[kState].status;
	}

	get ok(): boolean {
		const status = this[kState].status;
		return status >= 200 && status <= 299;
	}

	get statusText(): string {
		return this[kState].statusText;
	}

	get headers(): Headers {
		return this[kState].headers;
	}

	clone(): Response {
		const copy = new Response();
		copy[kState] = { ...this[kState], headers: cloneHeaders(this[kState].headers) };
		copy[kBody] = cloneBody(this[kBody]);
		return copy;
	}

	get [Symbol.toStringTag](): string {
		return "Response";
	}

	[Symbol.for("nodejs.util.inspect.custom")](
		depth: number,
		options: object,
		inspect: (v: unknown, o?: object) => string,
	): string {
		const s = this[kState];
		const length = this[kBody].length;
		const shown = {
			ok: this.ok,
			url: s.url,
			status: s.status,
			statusText: s.statusText,
			headers: s.headers,
			redirected: s.redirected,
			bodyUsed: this.bodyUsed,
		};
		return `Response (${length === null ? "unknown" : `${length} bytes`}) ${inspect(shown, options)}`;
	}
}

/** A Response with the given internal state and body (network responses). */
export function createResponse(state: ResponseState, body: BodyState | null): Response {
	const response = new Response();
	response[kState] = state;
	response[kBody] = body ?? emptyBody();
	return response;
}

export function responseState(response: Response): ResponseState {
	return response[kState];
}
