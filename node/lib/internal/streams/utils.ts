// Duck-typed stream predicates and shared symbols (Node's lib/internal/streams/utils.js).

// Symbol.for so readable-stream and core can read each other's private state.
export const kIsDestroyed: unique symbol = Symbol.for("nodejs.stream.destroyed") as never;
export const kIsErrored: unique symbol = Symbol.for("nodejs.stream.errored") as never;
export const kIsReadable: unique symbol = Symbol.for("nodejs.stream.readable") as never;
export const kIsWritable: unique symbol = Symbol.for("nodejs.stream.writable") as never;
export const kIsDisturbed: unique symbol = Symbol.for("nodejs.stream.disturbed") as never;

export const kOnConstructed: unique symbol = Symbol("kOnConstructed") as never;

export const kIsClosedPromise: unique symbol = Symbol.for("nodejs.webstream.isClosedPromise") as never;
export const kControllerErrorFunction: unique symbol = Symbol.for("nodejs.webstream.controllerErrorFunction") as never;

export const kState: unique symbol = Symbol("kState") as never;
export const kObjectMode = 1 << 0;
export const kErrorEmitted = 1 << 1;
export const kAutoDestroy = 1 << 2;
export const kEmitClose = 1 << 3;
export const kDestroyed = 1 << 4;
export const kClosed = 1 << 5;
export const kCloseEmitted = 1 << 6;
export const kErrored = 1 << 7;
export const kConstructed = 1 << 8;

/** Fields of a Readable/Writable state object these predicates probe. */
export interface StateLike {
	readable?: boolean;
	writable?: boolean;
	destroyed?: boolean;
	errored?: unknown;
	errorEmitted?: boolean;
	ended?: boolean;
	endEmitted?: boolean;
	finished?: boolean;
	closed?: boolean;
	length?: number;
	autoDestroy?: boolean;
	emitClose?: boolean;
	[key: string | symbol]: unknown;
}

/** Any value probed as a (Node or web) stream; every field is optional. */
export interface StreamLike {
	_readableState?: StateLike | null;
	_writableState?: StateLike | null;
	readable?: unknown;
	writable?: unknown;
	destroyed?: boolean;
	closed?: unknown;
	readableEnded?: boolean;
	writableEnded?: boolean;
	writableFinished?: boolean;
	readableErrored?: unknown;
	writableErrored?: unknown;
	readableDidRead?: boolean;
	readableAborted?: boolean;
	req?: { upgradeOrConnect?: unknown };
	pipe?: unknown;
	on?: unknown;
	write?: unknown;
	pause?: unknown;
	resume?: unknown;
	pipeThrough?: unknown;
	getReader?: unknown;
	getWriter?: unknown;
	cancel?: unknown;
	abort?: unknown;
	[key: string | symbol]: unknown;
}

type Maybe = StreamLike | null | undefined;

export function isReadableNodeStream(obj: Maybe, strict = false): boolean {
	return !!(
		obj &&
		typeof obj.pipe === "function" &&
		typeof obj.on === "function" &&
		(!strict || (typeof obj.pause === "function" && typeof obj.resume === "function")) &&
		(!obj._writableState || obj._readableState?.readable !== false) && // Duplex
		(!obj._writableState || obj._readableState) // Writable has .pipe.
	);
}

export function isWritableNodeStream(obj: Maybe): boolean {
	return !!(
		obj &&
		typeof obj.write === "function" &&
		typeof obj.on === "function" &&
		(!obj._readableState || obj._writableState?.writable !== false) // Duplex
	);
}

export function isDuplexNodeStream(obj: Maybe): boolean {
	return !!(
		obj &&
		typeof obj.pipe === "function" &&
		obj._readableState &&
		typeof obj.on === "function" &&
		typeof obj.write === "function"
	);
}

export function isNodeStream(obj: Maybe): boolean {
	return !!(
		obj &&
		(obj._readableState ||
			obj._writableState ||
			(typeof obj.write === "function" && typeof obj.on === "function") ||
			(typeof obj.pipe === "function" && typeof obj.on === "function"))
	);
}

export function isReadableStream(obj: Maybe): boolean {
	return !!(
		obj &&
		!isNodeStream(obj) &&
		typeof obj.pipeThrough === "function" &&
		typeof obj.getReader === "function" &&
		typeof obj.cancel === "function"
	);
}

export function isWritableStream(obj: Maybe): boolean {
	return !!(obj && !isNodeStream(obj) && typeof obj.getWriter === "function" && typeof obj.abort === "function");
}

export function isTransformStream(obj: Maybe): boolean {
	return !!(obj && !isNodeStream(obj) && typeof obj.readable === "object" && typeof obj.writable === "object");
}

export function isWebStream(obj: Maybe): boolean {
	return isReadableStream(obj) || isWritableStream(obj) || isTransformStream(obj);
}

export function isIterable(obj: unknown, isAsync?: boolean): boolean {
	if (obj == null) return false;
	const o = obj as Record<symbol, unknown>;
	if (isAsync === true) return typeof o[Symbol.asyncIterator] === "function";
	if (isAsync === false) return typeof o[Symbol.iterator] === "function";
	return typeof o[Symbol.asyncIterator] === "function" || typeof o[Symbol.iterator] === "function";
}

export function isDestroyed(stream: Maybe): boolean | null {
	if (!isNodeStream(stream)) return null;
	const s = stream as StreamLike;
	const state = s._writableState || s._readableState;
	return !!(s.destroyed || s[kIsDestroyed] || state?.destroyed);
}

// Have been end():d.
export function isWritableEnded(stream: Maybe): boolean | null {
	if (!isWritableNodeStream(stream)) return null;
	const s = stream as StreamLike;
	if (s.writableEnded === true) return true;
	const wState = s._writableState;
	if (wState?.errored) return false;
	if (typeof wState?.ended !== "boolean") return null;
	return wState.ended;
}

// Have emitted 'finish'.
export function isWritableFinished(stream: Maybe, strict?: boolean): boolean | null {
	if (!isWritableNodeStream(stream)) return null;
	const s = stream as StreamLike;
	if (s.writableFinished === true) return true;
	const wState = s._writableState;
	if (wState?.errored) return false;
	if (typeof wState?.finished !== "boolean") return null;
	return !!(wState.finished || (strict === false && wState.ended === true && wState.length === 0));
}

// Have been push(null):d.
export function isReadableEnded(stream: Maybe): boolean | null {
	if (!isReadableNodeStream(stream)) return null;
	const s = stream as StreamLike;
	if (s.readableEnded === true) return true;
	const rState = s._readableState;
	if (!rState || rState.errored) return false;
	if (typeof rState.ended !== "boolean") return null;
	return rState.ended;
}

// Have emitted 'end'.
export function isReadableFinished(stream: Maybe, strict?: boolean): boolean | null {
	if (!isReadableNodeStream(stream)) return null;
	const rState = (stream as StreamLike)._readableState;
	if (rState?.errored) return false;
	if (typeof rState?.endEmitted !== "boolean") return null;
	return !!(rState.endEmitted || (strict === false && rState.ended === true && rState.length === 0));
}

export function isReadable(stream: Maybe): boolean | null {
	if (stream && stream[kIsReadable] != null) return stream[kIsReadable] as boolean;
	if (typeof stream?.readable !== "boolean") return null;
	if (isDestroyed(stream)) return false;
	return isReadableNodeStream(stream) && stream.readable && !isReadableFinished(stream);
}

export function isWritable(stream: Maybe): boolean | null {
	if (stream && stream[kIsWritable] != null) return stream[kIsWritable] as boolean;
	if (typeof stream?.writable !== "boolean") return null;
	if (isDestroyed(stream)) return false;
	return isWritableNodeStream(stream) && stream.writable && !isWritableEnded(stream);
}

export function isFinished(stream: Maybe, opts?: { readable?: boolean; writable?: boolean }): boolean | null {
	if (!isNodeStream(stream)) return null;
	if (isDestroyed(stream)) return true;
	if (opts?.readable !== false && isReadable(stream)) return false;
	if (opts?.writable !== false && isWritable(stream)) return false;
	return true;
}

export function isWritableErrored(stream: Maybe): unknown {
	if (!isNodeStream(stream)) return null;
	const s = stream as StreamLike;
	if (s.writableErrored) return s.writableErrored;
	return s._writableState?.errored ?? null;
}

export function isReadableErrored(stream: Maybe): unknown {
	if (!isNodeStream(stream)) return null;
	const s = stream as StreamLike;
	if (s.readableErrored) return s.readableErrored;
	return s._readableState?.errored ?? null;
}

export function isClosed(stream: Maybe): boolean | null {
	if (!isNodeStream(stream)) return null;
	const s = stream as StreamLike;
	if (typeof s.closed === "boolean") return s.closed;
	const wState = s._writableState;
	const rState = s._readableState;
	if (typeof wState?.closed === "boolean" || typeof rState?.closed === "boolean") {
		return !!(wState?.closed || rState?.closed);
	}
	if (typeof s._closed === "boolean" && isOutgoingMessage(s)) return s._closed;
	return null;
}

function isOutgoingMessage(stream: StreamLike): boolean {
	return (
		typeof stream._closed === "boolean" &&
		typeof stream._defaultKeepAlive === "boolean" &&
		typeof stream._removedConnection === "boolean" &&
		typeof stream._removedContLen === "boolean"
	);
}

export function isServerResponse(stream: StreamLike): boolean {
	return typeof stream._sent100 === "boolean" && isOutgoingMessage(stream);
}

export function isServerRequest(stream: StreamLike): boolean {
	return (
		typeof stream._consuming === "boolean" &&
		typeof stream._dumped === "boolean" &&
		stream.req?.upgradeOrConnect === undefined
	);
}

export function willEmitClose(stream: Maybe): boolean | null {
	if (!isNodeStream(stream)) return null;
	const s = stream as StreamLike;
	const state = s._writableState || s._readableState;
	return (!state && isServerResponse(s)) || !!(state?.autoDestroy && state.emitClose && state.closed === false);
}

export function isDisturbed(stream: Maybe): boolean {
	return !!(stream && (stream[kIsDisturbed] ?? (stream.readableDidRead || stream.readableAborted)));
}

export function isErrored(stream: Maybe): boolean {
	return !!(
		stream &&
		(stream[kIsErrored] ??
			stream.readableErrored ??
			stream.writableErrored ??
			stream._readableState?.errorEmitted ??
			stream._writableState?.errorEmitted ??
			stream._readableState?.errored ??
			stream._writableState?.errored)
	);
}
