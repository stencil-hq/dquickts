// Shared HTTP definitions: methods, status codes, header validation and the
// Node error helpers of node:http. Adapted from Node.js lib/_http_common.js (MIT).

import { codedError, invalidArgValue } from "internal:errors";
import { setTimeout as setTimer } from "node:timers";

export const METHODS: string[] = [
	"ACL",
	"BIND",
	"CHECKOUT",
	"CONNECT",
	"COPY",
	"DELETE",
	"GET",
	"HEAD",
	"LINK",
	"LOCK",
	"M-SEARCH",
	"MERGE",
	"MKACTIVITY",
	"MKCALENDAR",
	"MKCOL",
	"MOVE",
	"NOTIFY",
	"OPTIONS",
	"PATCH",
	"POST",
	"PROPFIND",
	"PROPPATCH",
	"PURGE",
	"PUT",
	"QUERY",
	"REBIND",
	"REPORT",
	"SEARCH",
	"SOURCE",
	"SUBSCRIBE",
	"TRACE",
	"UNBIND",
	"UNLINK",
	"UNLOCK",
	"UNSUBSCRIBE",
];

export const STATUS_CODES: Record<number, string> = {
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

/** --max-http-header-size default */
export const maxHeaderSize = 16384;

const tokenRegExp = /^[\^_`a-zA-Z\-0-9!#$%&'*+.|~]+$/;
/** true when `val` is a valid RFC 7230 token (header name, method) */
export function checkIsHttpToken(val: unknown): boolean {
	return typeof val === "string" && tokenRegExp.test(val);
}

const headerCharRegex = /[^\t\x20-\x7e\x80-\xff]/;
/** true when `val` holds a character not allowed in a header value */
export function checkInvalidHeaderChar(val: unknown): boolean {
	return headerCharRegex.test(String(val));
}

export function validateHeaderName(name: unknown, label = "Header name"): asserts name is string {
	if (typeof name !== "string" || !name || !checkIsHttpToken(name)) {
		throw codedError("ERR_INVALID_HTTP_TOKEN", `${label} must be a valid HTTP token ["${String(name)}"]`, TypeError);
	}
}

export function validateHeaderValue(name: string, value: unknown): void {
	if (value === undefined) {
		throw codedError(
			"ERR_HTTP_INVALID_HEADER_VALUE",
			`Invalid value "${String(value)}" for header "${name}"`,
			TypeError,
		);
	}
	if (Array.isArray(value)) {
		for (const v of value) {
			if (checkInvalidHeaderChar(v))
				throw codedError("ERR_INVALID_CHAR", `Invalid character in header content ["${name}"]`, TypeError);
		}
		return;
	}
	if (checkInvalidHeaderChar(value)) {
		throw codedError("ERR_INVALID_CHAR", `Invalid character in header content ["${name}"]`, TypeError);
	}
}

export function headersSentError(action: string): Error {
	return codedError("ERR_HTTP_HEADERS_SENT", `Cannot ${action} headers after they are sent to the client`);
}

/** Node's ConnResetException: ECONNRESET with the given message */
export function connResetException(msg: string, cause?: unknown): Error {
	const err = new Error(msg) as Error & { code: string };
	err.code = "ECONNRESET";
	if (cause !== undefined) Object.defineProperty(err, "cause", { value: cause, configurable: true, writable: true });
	return err;
}

export function streamWriteAfterEnd(): Error {
	return codedError("ERR_STREAM_WRITE_AFTER_END", "write after end");
}

export function streamDestroyed(method: string): Error {
	return codedError("ERR_STREAM_DESTROYED", `Cannot call ${method} after a stream was destroyed`);
}

/** the msecs argument of setTimeout(): a non-negative number */
export function getTimerDuration(msecs: unknown, name: string): number {
	if (typeof msecs !== "number") {
		throw codedError(
			"ERR_INVALID_ARG_TYPE",
			`The "${name}" argument must be of type number. Received type ${typeof msecs} (${String(msecs)})`,
			TypeError,
		);
	}
	if (msecs < 0 || !Number.isFinite(msecs)) {
		throw codedError(
			"ERR_OUT_OF_RANGE",
			`The value of "${name}" is out of range. It must be a non-negative finite number. Received ${msecs}`,
			RangeError,
		);
	}
	return msecs;
}

let dateCache: string | undefined;
/** "Date: <IMF-fixdate>\r\n", cached for one second */
export function utcDateHeader(): string {
	if (!dateCache) {
		dateCache = `Date: ${new Date().toUTCString()}\r\n`;
		setTimer(() => {
			dateCache = undefined;
		}, 1000 - new Date().getMilliseconds()).unref();
	}
	return dateCache;
}

export type HeaderValue = string | number | string[];
export type OutgoingHeaders = Record<string, HeaderValue | undefined>;

/** `uniqueHeaders` option: names sent as one comma separated line */
export function parseUniqueHeadersOption(headers: unknown): Set<string> | null {
	if (!Array.isArray(headers)) return null;
	const unique = new Set<string>();
	for (const h of headers) {
		if (Array.isArray(h)) for (const x of h) unique.add(String(x).toLowerCase());
		else unique.add(String(h).toLowerCase());
	}
	return unique;
}

export function invalidHeaders(headers: unknown): Error {
	return invalidArgValue("headers", headers);
}
