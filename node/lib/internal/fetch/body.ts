// The Body mixin of Request and Response: extraction of a BodyInit, the
// lazily created ReadableStream, consumption (text/json/bytes/...), clone.

import { Blob } from "node:buffer";
import { URLSearchParams } from "node:url";
import { TextDecoder, TextEncoder } from "internal:encoding";
import { codedError } from "internal:errors";
import { Headers, contentType } from "internal:fetch/headers";
import { FormData, isBlobLike, parseMultipart, parseUrlEncoded, serializeFormData } from "internal:fetch/formdata";
import type { BlobLike } from "internal:fetch/formdata";

export type BodyInit =
	| string
	| ArrayBuffer
	| ArrayBufferView
	| Blob
	| BlobLike
	| URLSearchParams
	| FormData
	| ReadableStream<Uint8Array>
	| AsyncIterable<unknown>;

/** A body produced by the network (or another native source). */
export interface NativeBody {
	/** Create the ReadableStream of the body (called once). */
	stream(): ReadableStream<Uint8Array>;
	/** The whole body (when no stream was created). */
	collect(): Promise<Uint8Array>;
	/** Drop the body: the source stops. */
	cancel(reason?: unknown): void;
}

export interface BodyState {
	/** a whole body known in advance (re-readable, for clone and redirects) */
	source: Uint8Array | BlobLike | null;
	stream: ReadableStream<Uint8Array> | null;
	native: NativeBody | null;
	/** byte length when known */
	length: number | null;
	used: boolean;
}

export const kBody = Symbol("body");
const kDisturbed = Symbol.for("nodejs.stream.disturbed");
const kImageEncode = Symbol.for("qbun.image.encodeSync");

interface EncodableImage {
	[kImageEncode](): { data: Uint8Array; type: string };
}

function isEncodableImage(value: unknown): value is EncodableImage {
	return (
		value !== null && typeof value === "object" && kImageEncode in value && typeof value[kImageEncode] === "function"
	);
}
const encoder = new TextEncoder();
const decoder = new TextDecoder();

export function emptyBody(): BodyState {
	return { source: null, stream: null, native: null, length: null, used: false };
}

export function isReadableStream(value: unknown): value is ReadableStream<Uint8Array> {
	return (
		value !== null &&
		typeof value === "object" &&
		typeof (value as ReadableStream).getReader === "function" &&
		typeof (value as ReadableStream).tee === "function"
	);
}

function isAsyncIterable(value: unknown): value is AsyncIterable<unknown> {
	return (
		value !== null &&
		typeof value === "object" &&
		typeof (value as AsyncIterable<unknown>)[Symbol.asyncIterator] === "function"
	);
}

function isGenerator(value: unknown): value is Iterator<unknown> & Iterable<unknown> {
	return (
		value !== null &&
		typeof value === "object" &&
		Object.prototype.toString.call(value) === "[object Generator]" &&
		typeof (value as Iterator<unknown>).next === "function"
	);
}

/** A chunk of a streamed body as bytes (strings are UTF-8). */
export function chunkBytes(chunk: unknown): Uint8Array {
	if (chunk instanceof Uint8Array) return chunk;
	if (typeof chunk === "string") return encoder.encode(chunk);
	if (
		chunk instanceof ArrayBuffer ||
		(typeof SharedArrayBuffer !== "undefined" && chunk instanceof SharedArrayBuffer)
	) {
		return new Uint8Array(chunk);
	}
	if (ArrayBuffer.isView(chunk)) return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
	throw new TypeError("ReadableStream body chunks must be Uint8Array, ArrayBuffer, ArrayBufferView or string");
}

/** A byte stream reading an (async) iterator. */
function iteratorStream(iterator: AsyncIterator<unknown> | Iterator<unknown>): ReadableStream<Uint8Array> {
	return new ReadableStream<Uint8Array>(
		{
			async pull(controller) {
				const r = await iterator.next();
				if (r.done) controller.close();
				else controller.enqueue(chunkBytes(r.value));
			},
			async cancel(reason) {
				await iterator.return?.(reason);
			},
		},
		{ highWaterMark: 0 },
	);
}

function copyBytes(view: ArrayBuffer | ArrayBufferView): Uint8Array {
	if (ArrayBuffer.isView(view)) return new Uint8Array(view.buffer, view.byteOffset, view.byteLength).slice();
	return new Uint8Array(view as ArrayBuffer).slice();
}

/** Extract a BodyInit (Fetch "extract a body"): the state and its content type. */
export function extractBody(init: unknown): { state: BodyState; type: string | null } {
	const state = emptyBody();
	let type: string | null = null;
	if (isEncodableImage(init)) {
		// Bun.Image: encoded now, in its own format
		const image = init[kImageEncode]();
		state.source = image.data;
		if (image.type) type = image.type;
	} else if (typeof init === "string") {
		state.source = encoder.encode(init);
		type = "text/plain;charset=UTF-8";
	} else if (
		init instanceof ArrayBuffer ||
		(typeof SharedArrayBuffer !== "undefined" && init instanceof SharedArrayBuffer)
	) {
		state.source = copyBytes(init as ArrayBuffer);
	} else if (ArrayBuffer.isView(init)) {
		state.source = copyBytes(init);
	} else if (init instanceof URLSearchParams) {
		state.source = encoder.encode(init.toString());
		type = "application/x-www-form-urlencoded;charset=UTF-8";
	} else if (init instanceof FormData) {
		const form = serializeFormData(init);
		if (form.blob) state.source = form.blob;
		else state.stream = form.stream;
		if (form.length !== null) state.length = form.length;
		type = form.type;
	} else if (isBlobLike(init)) {
		state.source = init;
		if (init.type) type = init.type;
	} else if (isReadableStream(init)) {
		if (init.locked || (init as unknown as Record<symbol, unknown>)[kDisturbed] === true) {
			throw new TypeError("ReadableStream is locked or disturbed");
		}
		state.stream = init;
	} else if (isAsyncIterable(init)) {
		state.stream = iteratorStream(init[Symbol.asyncIterator]());
	} else if (isGenerator(init)) {
		state.stream = iteratorStream(init);
	} else {
		state.source = encoder.encode(String(init));
		type = "text/plain;charset=UTF-8";
	}
	if (state.source) state.length = state.source instanceof Uint8Array ? state.source.length : state.source.size;
	return { state, type };
}

function sourceStream(source: Uint8Array | BlobLike): ReadableStream<Uint8Array> {
	if (!(source instanceof Uint8Array)) return source.stream();
	let offset = 0;
	return new ReadableStream<Uint8Array>(
		{
			pull(controller) {
				if (offset >= source.length) {
					controller.close();
					return;
				}
				const end = Math.min(offset + 65536, source.length);
				controller.enqueue(source.slice(offset, end));
				offset = end;
				if (offset >= source.length) controller.close();
			},
		},
		{ highWaterMark: 0 },
	);
}

/** The ReadableStream of a body, created on first use (null for no body). */
export function bodyStream(state: BodyState): ReadableStream<Uint8Array> | null {
	if (state.stream) return state.stream;
	if (state.native) {
		state.stream = state.native.stream();
		state.native = null;
	} else if (state.source) {
		state.stream = sourceStream(state.source);
	}
	return state.stream;
}

export function isUsed(state: BodyState): boolean {
	if (state.used) return true;
	const stream = state.stream as (ReadableStream<Uint8Array> & Record<symbol, unknown>) | null;
	return stream !== null && stream[kDisturbed] === true;
}

export function bodyUsedError(): Error {
	return codedError("ERR_BODY_ALREADY_USED", "Body already used", TypeError);
}

/** Read a whole stream. */
export async function readAll(stream: ReadableStream<Uint8Array>): Promise<Uint8Array> {
	if (stream.locked) throw new TypeError("ReadableStream is locked");
	const reader = stream.getReader();
	const chunks: Uint8Array[] = [];
	let total = 0;
	try {
		for (;;) {
			const { done, value } = await reader.read();
			if (done) break;
			const bytes = chunkBytes(value);
			chunks.push(bytes);
			total += bytes.length;
		}
	} finally {
		reader.releaseLock();
	}
	return concatBytes(chunks, total);
}

export function concatBytes(chunks: Uint8Array[], total: number): Uint8Array {
	if (chunks.length === 1 && chunks[0].byteOffset === 0 && chunks[0].byteLength === chunks[0].buffer.byteLength)
		return chunks[0];
	const out = new Uint8Array(total);
	let offset = 0;
	for (const chunk of chunks) {
		out.set(chunk, offset);
		offset += chunk.length;
	}
	return out;
}

/** Consume the body: its bytes (the body becomes used). */
export async function consumeBody(state: BodyState): Promise<Uint8Array> {
	if (isUsed(state)) throw bodyUsedError();
	if (state.stream?.locked) throw new TypeError("ReadableStream is locked");
	state.used = true;
	if (state.stream) return readAll(state.stream);
	if (state.native) {
		const native = state.native;
		state.native = null;
		return native.collect();
	}
	const source = state.source;
	if (!source) return new Uint8Array(0);
	if (source instanceof Uint8Array) return source.slice();
	return new Uint8Array(await source.arrayBuffer());
}

/** A copy of the body for clone(): streams are teed. */
export function cloneBody(state: BodyState): BodyState {
	if (isUsed(state)) throw bodyUsedError();
	if (state.stream || state.native) {
		const stream = bodyStream(state)!;
		if (stream.locked) throw new TypeError("ReadableStream is locked");
		const [a, b] = stream.tee();
		state.stream = a;
		return { source: null, stream: b, native: null, length: state.length, used: false };
	}
	return { source: state.source, stream: null, native: null, length: state.length, used: false };
}

/** Cancel a body that will not be read. */
export function cancelBody(state: BodyState, reason?: unknown): void {
	if (state.native) {
		state.native.cancel(reason);
		state.native = null;
	} else if (state.stream && !state.stream.locked) {
		state.stream.cancel(reason).catch(() => {});
	}
}

export function decodeText(bytes: Uint8Array): string {
	return decoder.decode(bytes);
}

function exactArrayBuffer(bytes: Uint8Array): ArrayBuffer {
	if (bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength && bytes.buffer instanceof ArrayBuffer) {
		return bytes.buffer;
	}
	return bytes.slice().buffer as ArrayBuffer;
}

/** The base of Request and Response (the Body mixin). */
export class Body {
	[kBody]: BodyState = emptyBody();

	constructor() {
		Object.defineProperty(this, kBody, { enumerable: false, writable: true });
	}

	/** Defined by Request and Response. */
	get headers(): Headers {
		throw new TypeError("Illegal invocation");
	}

	get body(): ReadableStream<Uint8Array> | null {
		return bodyStream(this[kBody]);
	}

	get bodyUsed(): boolean {
		return isUsed(this[kBody]);
	}

	async arrayBuffer(): Promise<ArrayBuffer> {
		return exactArrayBuffer(await consumeBody(this[kBody]));
	}

	async bytes(): Promise<Uint8Array> {
		return consumeBody(this[kBody]);
	}

	async text(): Promise<string> {
		return decoder.decode(await consumeBody(this[kBody]));
	}

	async json(): Promise<unknown> {
		return JSON.parse(decoder.decode(await consumeBody(this[kBody])));
	}

	async blob(): Promise<Blob> {
		const bytes = await consumeBody(this[kBody]);
		return new Blob([bytes], { type: contentType(this.headers) });
	}

	async formData(): Promise<FormData> {
		const type = contentType(this.headers);
		const bytes = await consumeBody(this[kBody]);
		if (/^multipart\/form-data/i.test(type)) return parseMultipart(bytes, type);
		if (/^application\/x-www-form-urlencoded/i.test(type)) return parseUrlEncoded(decoder.decode(bytes));
		throw new TypeError(`Failed to parse body as FormData: unsupported content type "${type}"`);
	}
}
