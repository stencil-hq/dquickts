// fetch() (WHATWG Fetch, Bun flavor) and the Headers, Request, Response
// and FormData globals.
//
// http(s): goes through qbun:http (libcurl on the runtime loop); data:,
// file: (Bun reads local files) and blob: URLs are answered locally.
// Redirects are followed here (follow/error/manual, 303 and POST→GET
// rewrites), the response body is a pull-based ReadableStream mapped to
// the transfer's pause/resume, an AbortSignal stops the request or errors
// the body. Bun extensions: verbose, proxy, tls, timeout, decompress,
// keepalive, unix, protocol, maxRedirects, compress.

import * as core from "qbun:core";
import { fileURLToPath, resolveObjectURL } from "node:url";
import { file as bunFile } from "internal:bun/file";
import * as zlib from "node:zlib";
import { codedError, notSupported } from "internal:errors";
import { appendRaw, cloneHeaders, deleteRaw, setGuard, setRaw, wireEntries } from "internal:fetch/headers";
import { isBlobLike } from "internal:fetch/formdata";
import { requestState } from "internal:fetch/request";
import type { RequestInit } from "internal:fetch/request";
import { STATUS_TEXT, createResponse, isNullBodyStatus, isRedirectStatus } from "internal:fetch/response";
import type { ResponseState } from "internal:fetch/response";
import { bodyStream, emptyBody, kBody } from "internal:fetch/body";
import type { BodyState } from "internal:fetch/body";
import { NetBody, preresolve, resolveProxy, resolveTransport, startHttp } from "internal:fetch/http";
import type { ProxyOption, TLSOptions, Transport } from "internal:fetch/http";

export { Headers } from "internal:fetch/headers";
export { Request } from "internal:fetch/request";
export { Response } from "internal:fetch/response";
export { FormData } from "internal:fetch/formdata";

type CompressOption =
	| boolean
	| "gzip"
	| "deflate"
	| "br"
	| "zstd"
	| { encoding: "gzip" | "deflate" | "br" | "zstd"; level?: number };

/** RequestInit plus Bun's fetch extensions. */
export interface BunFetchRequestInit extends RequestInit {
	verbose?: boolean;
	proxy?: ProxyOption;
	tls?: TLSOptions;
	timeout?: number | boolean;
	decompress?: boolean;
	unix?: string;
	protocol?: string;
	maxRedirects?: number;
	compress?: CompressOption;
	session?: unknown;
	s3?: unknown;
}

const REQUEST_BODY_HEADERS = [
	"content-encoding",
	"content-language",
	"content-location",
	"content-type",
	"content-length",
];

/** Responses collected with an untouched body cancel their transfer. */
const unusedBodies = new FinalizationRegistry<NetBody>((body) => body.dropIfUnused());

export function fetch(input: string | URL | Request, init?: BunFetchRequestInit): Promise<Response> {
	return doFetch(input, init);
}

/** Bun: resolve the DNS of a URL's host before it is fetched. */
function preconnect(
	url: string | URL,
	options?: { dns?: boolean; tcp?: boolean; http?: boolean; https?: boolean },
): void {
	let parsed: URL;
	try {
		parsed = new URL(String(url));
	} catch {
		throw new TypeError(`fetch.preconnect: invalid URL ${String(url)}`);
	}
	if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
		throw new TypeError("fetch.preconnect: the URL must use http: or https:");
	}
	if (options && options.dns === false && !options.tcp && !options.http && !options.https) return;
	preresolve(parsed.hostname.replace(/^\[|\]$/g, ""));
}

Object.defineProperty(fetch, "preconnect", { value: preconnect, writable: true, configurable: true, enumerable: true });

async function doFetch(input: string | URL | Request, init: BunFetchRequestInit | undefined): Promise<Response> {
	if (input === undefined && init === undefined) throw new TypeError("fetch() requires at least 1 argument");
	const options: BunFetchRequestInit = init ?? {};
	const request = input instanceof Request && init === undefined ? input : new Request(input, options);
	const rs = requestState(request);
	const signal = rs.userSignal;
	if (signal?.aborted) throw signal.reason;
	const url = new URL(rs.url);
	switch (url.protocol) {
		case "http:":
		case "https:":
			return httpFetch(request, url, options, signal);
		case "data:":
			return dataFetch(url);
		case "file:":
			return fileFetch(url, rs.method);
		case "blob:":
			return blobFetch(url, rs.method);
		case "s3:":
			throw notSupported("fetch() of s3:// URLs");
		default:
			throw new TypeError(`fetch() URL must use http:, https:, data:, file: or blob: (got "${url.protocol}")`);
	}
}

// --------------------------------------------------------------- local URLs

function localResponse(url: string, body: BodyState | null, headers: Headers): Response {
	setGuard(headers, "immutable");
	const state: ResponseState = { type: "basic", url, redirected: false, status: 200, statusText: "OK", headers };
	return createResponse(state, body);
}

function bytesBody(bytes: Uint8Array): BodyState {
	return { ...emptyBody(), source: bytes, length: bytes.length };
}

function dataFetch(url: URL): Response {
	const href = url.hash ? url.href.slice(0, url.href.length - url.hash.length) : url.href;
	const comma = href.indexOf(",");
	if (comma < 0) throw new TypeError("fetch failed: invalid data: URL");
	let meta = href.slice(5, comma).replace(/^[\t\n\f\r ]+|[\t\n\f\r ]+$/g, "");
	const data = href.slice(comma + 1);
	const raw: number[] = [];
	for (let i = 0; i < data.length; i++) {
		const c = data.charCodeAt(i);
		if (c === 0x25 && i + 2 < data.length && /^[0-9a-fA-F]{2}$/.test(data.slice(i + 1, i + 3))) {
			raw.push(parseInt(data.slice(i + 1, i + 3), 16));
			i += 2;
		} else {
			raw.push(c & 0xff);
		}
	}
	let bytes = new Uint8Array(raw);
	const base64 = /;[\t ]*base64[\t ]*$/i;
	if (base64.test(meta)) {
		meta = meta.replace(base64, "");
		let text = "";
		for (const b of bytes) text += String.fromCharCode(b);
		text = text.replace(/[\t\n\f\r ]/g, "");
		let decoded: string;
		try {
			decoded = atob(text);
		} catch {
			throw new TypeError("fetch failed: invalid base64 in data: URL");
		}
		bytes = new Uint8Array(decoded.length);
		for (let i = 0; i < decoded.length; i++) bytes[i] = decoded.charCodeAt(i);
	}
	if (meta.startsWith(";")) meta = `text/plain${meta}`;
	const type = meta || "text/plain;charset=US-ASCII";
	const headers = new Headers();
	setRaw(headers, "Content-Type", type);
	return localResponse(href, bytesBody(bytes), headers);
}

function fileFetch(url: URL, method: string): Response {
	const path = fileURLToPath(url);
	const kind = core.statKind(path) as number;
	if (kind !== 1) {
		const missing = kind === 0;
		const err = codedError(
			missing ? "ENOENT" : "EISDIR",
			missing
				? `ENOENT: no such file or directory, open '${path}'`
				: `EISDIR: illegal operation on a directory, read '${path}'`,
		);
		Object.assign(err, { errno: missing ? -2 : -21, syscall: missing ? "open" : "read", path });
		throw err;
	}
	const file = bunFile(path);
	const headers = new Headers();
	if (file.type) setRaw(headers, "Content-Type", file.type);
	setRaw(headers, "Content-Length", String(file.size));
	const body: BodyState = method === "HEAD" ? emptyBody() : { ...emptyBody(), source: file, length: file.size };
	return localResponse(url.href, body, headers);
}

function blobFetch(url: URL, method: string): Response {
	if (method !== "GET") throw new TypeError("fetch failed: blob: URLs only support GET");
	const blob = resolveObjectURL(url.href);
	if (!blob) throw new TypeError(`fetch failed: blob URL not found: ${url.href}`);
	const headers = new Headers();
	if (blob.type) setRaw(headers, "Content-Type", blob.type);
	setRaw(headers, "Content-Length", String(blob.size));
	return localResponse(url.href, { ...emptyBody(), source: blob, length: blob.size }, headers);
}

// --------------------------------------------------------------------- http

interface Upload {
	bytes: Uint8Array | null;
	stream: ReadableStream<Uint8Array> | null;
	length: number | null;
}

const ZLIB_SYNC: Record<string, string> = {
	gzip: "gzipSync",
	deflate: "deflateSync",
	br: "brotliCompressSync",
	zstd: "zstdCompressSync",
};

/** Bun's `compress` option: compress a buffered body. */
function compressBody(bytes: Uint8Array, option: CompressOption, headers: Headers): Uint8Array {
	if (option === false || headers.has("content-encoding")) return bytes;
	const encoding = option === true ? "gzip" : typeof option === "string" ? option : option.encoding;
	const fn = ZLIB_SYNC[encoding];
	if (!fn) throw new TypeError(`fetch: unsupported compress encoding "${encoding}"`);
	const level = typeof option === "object" ? option.level : undefined;
	let opts: object | undefined;
	if (level !== undefined) {
		opts =
			encoding === "br"
				? { params: { [zlib.constants.BROTLI_PARAM_QUALITY]: level } }
				: encoding === "zstd"
					? { params: { [zlib.constants.ZSTD_c_compressionLevel]: level } }
					: { level };
	}
	const out = zlib[fn](bytes, opts);
	setRaw(headers, "Content-Encoding", encoding);
	return out;
}

async function prepareUpload(
	state: BodyState | null,
	compress: CompressOption | undefined,
	headers: Headers,
): Promise<Upload | null> {
	if (!state) return null;
	const source = state.source;
	let bytes: Uint8Array | null = null;
	if (!state.stream && !state.native && source) {
		bytes = source instanceof Uint8Array ? source : new Uint8Array(await source.arrayBuffer());
	}
	if (bytes) {
		if (compress !== undefined) bytes = compressBody(bytes, compress, headers);
		return { bytes, stream: null, length: bytes.length };
	}
	const stream = bodyStream(state);
	if (!stream) return null;
	if (stream.locked) throw new TypeError("ReadableStream is locked");
	state.used = true;
	return { bytes: null, stream, length: state.length };
}

/** The header list given to qbun:http (null removes a libcurl default). */
function nativeHeaders(headers: Headers, hasBody: boolean): { list: (string | null)[]; length: number | null } {
	const list: (string | null)[] = [];
	let length: number | null = null;
	let hasAgent = false;
	let hasType = false;
	let hasExpect = false;
	for (const [name, value] of wireEntries(headers)) {
		const lower = name.toLowerCase();
		if (lower === "content-length") {
			const n = Number(value);
			if (Number.isSafeInteger(n) && n >= 0) length = n;
			continue;
		}
		if (lower === "transfer-encoding") continue;
		if (lower === "user-agent") hasAgent = true;
		else if (lower === "content-type") hasType = true;
		else if (lower === "expect") hasExpect = true;
		list.push(name, value);
	}
	if (!hasAgent) list.push("User-Agent", `Bun/${process.versions.bun}`);
	if (hasBody && !hasType) list.push("Content-Type", null);
	if (!hasExpect) list.push("Expect", null);
	return { list, length };
}

function abortReason(signal: AbortSignal): unknown {
	return signal.reason ?? new DOMException("The operation was aborted.", "AbortError");
}

async function httpFetch(
	request: Request,
	url: URL,
	init: BunFetchRequestInit,
	signal: AbortSignal | null,
): Promise<Response> {
	const rs = requestState(request);
	const transport: Transport = await resolveTransport(init);
	const headers = cloneHeaders(rs.headers, "none");
	let method = rs.method;
	let upload = await prepareUpload(
		request[kBody].source || request[kBody].stream || request[kBody].native ? request[kBody] : null,
		init.compress,
		headers,
	);
	const maxRedirects = init.maxRedirects === undefined ? 20 : Math.max(0, Number(init.maxRedirects) || 0);
	let redirects = 0;
	let current = url;

	for (;;) {
		if (signal?.aborted) throw abortReason(signal);
		const proxy = transport.unix !== null ? { proxy: "", headers: null } : resolveProxy(current, init.proxy);
		transport.proxy = proxy.proxy;
		transport.proxyHeaders = proxy.headers;
		// POST/PUT/PATCH without a body send "Content-Length: 0"
		let body: Uint8Array | ReadableStream<Uint8Array> | null = upload ? (upload.bytes ?? upload.stream) : null;
		if (!body && (method === "POST" || method === "PUT" || method === "PATCH")) body = new Uint8Array(0);
		const { list, length } = nativeHeaders(headers, body !== null);
		const exchange = startHttp({
			url: current.href,
			method,
			headers: list,
			body,
			bodyLength: upload?.stream ? (upload.length ?? length) : null,
			transport,
		});
		const onAbort = (): void => exchange.abort(abortReason(signal!));
		signal?.addEventListener("abort", onAbort, { once: true });
		exchange.body.whenSettled(() => signal?.removeEventListener("abort", onAbort));

		const head = await exchange.head;
		const status = head.status;
		let location: string | null = null;
		if (isRedirectStatus(status)) {
			for (let i = 0; i < head.headers.length; i += 2) {
				if (head.headers[i].toLowerCase() === "location") {
					location = head.headers[i + 1];
					break;
				}
			}
		}
		if (location !== null && rs.redirect !== "manual") {
			exchange.body.cancel();
			if (rs.redirect === "error") {
				throw codedError("UnexpectedRedirect", `Unexpected redirect to ${location}`, TypeError);
			}
			if (++redirects > maxRedirects) {
				throw codedError(
					"TooManyRedirects",
					"The response redirected too many times. For more information, pass `verbose: true` in the second argument to fetch()",
					TypeError,
				);
			}
			let next: URL;
			try {
				next = new URL(location, head.url || current.href);
			} catch {
				throw codedError("InvalidRedirectURL", `Invalid redirect location: ${location}`, TypeError);
			}
			if (next.protocol !== "http:" && next.protocol !== "https:") {
				throw codedError("UnsupportedRedirectProtocol", `Redirect to a non-HTTP(S) URL: ${next.href}`, TypeError);
			}
			if (
				(status === 303 && method !== "GET" && method !== "HEAD") ||
				((status === 301 || status === 302) && method === "POST")
			) {
				method = "GET";
				upload = null;
				for (const name of REQUEST_BODY_HEADERS) deleteRaw(headers, name);
			} else if (upload?.stream) {
				throw codedError("RedirectStreamBody", "Cannot follow a redirect with a streamed request body", TypeError);
			}
			if (next.origin !== current.origin) {
				deleteRaw(headers, "authorization");
				deleteRaw(headers, "proxy-authorization");
				deleteRaw(headers, "cookie");
			}
			current = next;
			continue;
		}

		const responseHeaders = new Headers();
		for (let i = 0; i + 1 < head.headers.length; i += 2)
			appendRaw(responseHeaders, head.headers[i], head.headers[i + 1]);
		setGuard(responseHeaders, "immutable");
		const state: ResponseState = {
			type: "basic",
			url: head.url || current.href,
			redirected: redirects > 0,
			status,
			statusText: head.statusText || STATUS_TEXT[status] || "",
			headers: responseHeaders,
		};
		if (method === "HEAD" || isNullBodyStatus(status)) {
			// nothing to read: let the transfer finish on its own
			exchange.body.collect().catch(() => {});
			return createResponse(state, null);
		}
		let bodyLength: number | null = null;
		const contentLength = responseHeaders.get("content-length");
		if (contentLength !== null && (!transport.decompress || !responseHeaders.has("content-encoding"))) {
			const n = Number(contentLength);
			if (Number.isSafeInteger(n) && n >= 0) bodyLength = n;
		}
		const response = createResponse(state, { ...emptyBody(), native: exchange.body, length: bodyLength });
		unusedBodies.register(response, exchange.body);
		return response;
	}
}

export default { fetch, Headers, Request, Response, FormData };

/** Used by Bun APIs that accept "Blob-like" bodies. */
export { isBlobLike };
