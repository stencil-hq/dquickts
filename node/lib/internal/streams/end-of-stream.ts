// stream.finished(): wait for a stream to end, finish, error or close
// (Node's lib/internal/streams/end-of-stream.js, from mafintosh/end-of-stream).

import { bind } from "internal:async-context";
import { addAbortListener } from "node:events";
import {
	AbortError,
	ERR_INVALID_ARG_TYPE,
	ERR_STREAM_PREMATURE_CLOSE,
	kEmptyObject,
	nextTick,
	once,
	validateAbortSignal,
	validateBoolean,
	validateFunction,
	validateObject,
} from "internal:streams/shared";
import {
	isClosed,
	isNodeStream,
	isReadable,
	isReadableErrored,
	isReadableFinished,
	isReadableNodeStream,
	isReadableStream,
	isWritable,
	isWritableErrored,
	isWritableFinished,
	isWritableNodeStream,
	isWritableStream,
	kIsClosedPromise,
	type StreamLike,
	willEmitClose as _willEmitClose,
} from "internal:streams/utils";

export interface FinishedOptions {
	error?: boolean;
	readable?: boolean;
	writable?: boolean;
	signal?: AbortSignal;
	cleanup?: boolean;
	[kEosNodeSynchronousCallback]?: boolean;
}

type EosCallback = (this: unknown, err?: unknown) => void;
type Listener = (...args: unknown[]) => void;

/** The emitter surface eos() listens on. */
interface EosStream extends StreamLike {
	on(event: string, fn: Listener): unknown;
	removeListener(event: string, fn: Listener): unknown;
	req?: EosStream & { on(event: string, fn: Listener): unknown };
	aborted?: unknown;
	destroyed?: boolean;
	setHeader?: unknown;
	abort?: unknown;
}

function isRequest(stream: EosStream): boolean {
	return !!(stream.setHeader && typeof stream.abort === "function");
}

const nop = () => {};

function getEosErrored(stream: StreamLike): unknown {
	const errored = isWritableErrored(stream) || isReadableErrored(stream);
	return (typeof errored !== "boolean" && errored) || null;
}

function getEosOnCloseError(
	stream: StreamLike,
	readable: boolean,
	readableFinished: boolean | null,
	writable: boolean,
	writableFinished: boolean | null,
): unknown {
	const errored = getEosErrored(stream);
	if (errored) return errored;

	if (readable && !readableFinished && isReadableNodeStream(stream, true)) {
		if (!isReadableFinished(stream, false)) return new ERR_STREAM_PREMATURE_CLOSE();
	}
	if (writable && !writableFinished) {
		if (!isWritableFinished(stream, false)) return new ERR_STREAM_PREMATURE_CLOSE();
	}
	return null;
}

// Internal only: if eos() can settle immediately, invoke the callback before
// returning cleanup. Callers must tolerate cleanup yet to be assigned.
export const kEosNodeSynchronousCallback: unique symbol = Symbol("kEosNodeSynchronousCallback") as never;

export function eos(
	stream: unknown,
	options: FinishedOptions | EosCallback | null | undefined,
	callback?: EosCallback,
): () => void {
	let opts: FinishedOptions;
	if (arguments.length === 2) {
		callback = options as EosCallback;
		opts = kEmptyObject;
	} else if (options == null) {
		opts = kEmptyObject;
	} else {
		validateObject(options, "options");
		opts = options as FinishedOptions;
	}
	validateFunction(callback, "callback");
	validateAbortSignal(opts.signal, "options.signal");

	if (isReadableStream(stream as StreamLike) || isWritableStream(stream as StreamLike)) {
		return eosWeb(stream as StreamLike, opts, callback);
	}

	if (!isNodeStream(stream as StreamLike)) {
		throw new ERR_INVALID_ARG_TYPE("stream", ["ReadableStream", "WritableStream", "Stream"], stream);
	}
	const s = stream as EosStream;
	let cb: EosCallback = callback;

	const readable = opts.readable ?? isReadableNodeStream(s);
	const writable = opts.writable ?? isWritableNodeStream(s);

	let willEmitClose =
		!!_willEmitClose(s) && isReadableNodeStream(s) === readable && isWritableNodeStream(s) === writable;
	let writableFinished = isWritableFinished(s, false);
	let readableFinished = isReadableFinished(s, false);

	const wState = s._writableState;
	const rState = s._readableState;

	// undefined: to be determined; null: no error; otherwise the error
	let immediateResult: unknown;
	if (isClosed(s)) {
		immediateResult = getEosOnCloseError(s, readable, readableFinished, writable, writableFinished);
	} else if (wState?.errorEmitted || rState?.errorEmitted) {
		if (!willEmitClose) immediateResult = getEosErrored(s);
	} else if (
		!readable &&
		(!willEmitClose || isReadable(s)) &&
		(writableFinished || isWritable(s) === false) &&
		(wState == null || wState.pendingcb === undefined || wState.pendingcb === 0)
	) {
		immediateResult = getEosErrored(s);
	} else if (!writable && (!willEmitClose || isWritable(s)) && (readableFinished || isReadable(s) === false)) {
		immediateResult = getEosErrored(s);
	} else if (rState && s.req && s.aborted) {
		immediateResult = getEosErrored(s);
	}
	let cleanup = () => {
		cb = nop;
	};
	if (immediateResult !== undefined) {
		if (opts.error !== false) {
			s.on("error", nop);
			cleanup = () => {
				cb = nop;
				s.removeListener("error", nop);
			};
		}
	} else if (opts.signal?.aborted) {
		immediateResult = new AbortError(undefined, { cause: opts.signal.reason });
	}
	if (immediateResult !== undefined && opts[kEosNodeSynchronousCallback]) {
		Reflect.apply(cb, s, immediateResult === null ? [] : [immediateResult]);
		return cleanup;
	}

	cb = bind(cb);

	if (immediateResult !== undefined) {
		nextTick(() => Reflect.apply(cb, s, immediateResult === null ? [] : [immediateResult]));
		return cleanup;
	}

	cb = once(cb) as EosCallback;

	const onlegacyfinish = () => {
		if (!s.writable) onfinish();
	};

	const onfinish = () => {
		writableFinished = true;
		// Stream should not be destroyed here. If it is that means that user
		// space is doing something differently and we cannot trust willEmitClose.
		if (s.destroyed) willEmitClose = false;
		if (willEmitClose && (!s.readable || readable)) return;
		if (!readable || readableFinished) cb.call(s);
	};

	const onend = () => {
		readableFinished = true;
		if (s.destroyed) willEmitClose = false;
		if (willEmitClose && (!s.writable || writable)) return;
		if (!writable || writableFinished) cb.call(s);
	};

	const onerror = (err: unknown) => {
		cb.call(s, err);
	};

	const onclose = () => {
		const error = getEosOnCloseError(s, readable, readableFinished, writable, writableFinished);
		if (error === null) cb.call(s);
		else cb.call(s, error);
	};

	const onrequest = () => {
		(s.req as EosStream).on("finish", onfinish);
	};

	if (isRequest(s)) {
		s.on("complete", onfinish);
		if (!willEmitClose) s.on("abort", onclose);
		if (s.req) onrequest();
		else s.on("request", onrequest);
	} else if (writable && !wState) {
		// legacy streams
		s.on("end", onlegacyfinish);
		s.on("close", onlegacyfinish);
	}

	// Not all streams will emit 'close' after 'aborted'.
	if (!willEmitClose && typeof s.aborted === "boolean") s.on("aborted", onclose);

	s.on("end", onend);
	s.on("finish", onfinish);
	if (opts.error !== false) s.on("error", onerror as Listener);
	s.on("close", onclose);

	cleanup = () => {
		cb = nop;
		s.removeListener("aborted", onclose);
		s.removeListener("complete", onfinish);
		s.removeListener("abort", onclose);
		s.removeListener("request", onrequest);
		if (s.req) s.req.removeListener("finish", onfinish);
		s.removeListener("end", onlegacyfinish);
		s.removeListener("close", onlegacyfinish);
		s.removeListener("finish", onfinish);
		s.removeListener("end", onend);
		s.removeListener("error", onerror as Listener);
		s.removeListener("close", onclose);
	};

	if (opts.signal) {
		const signal = opts.signal;
		const abort = () => {
			// Keep it because cleanup removes it.
			const endCallback = cb;
			cleanup();
			endCallback.call(s, new AbortError(undefined, { cause: signal.reason }));
		};
		const disposable = addAbortListener(signal, abort);
		const originalCallback = cb;
		cb = once((...args: unknown[]) => {
			disposable[Symbol.dispose]();
			Reflect.apply(originalCallback, s, args);
		}) as EosCallback;
	}

	return cleanup;
}

function eosWeb(stream: StreamLike, options: FinishedOptions, callback: EosCallback): () => void {
	let cb = once(bind(callback)) as EosCallback;

	let isAborted = false;
	if (options.signal) {
		const signal = options.signal;
		const abort = () => {
			isAborted = true;
			cb.call(stream, new AbortError(undefined, { cause: signal.reason }));
		};
		if (signal.aborted) {
			nextTick(abort);
		} else {
			const disposable = addAbortListener(signal, abort);
			const originalCallback = cb;
			cb = once((...args: unknown[]) => {
				disposable[Symbol.dispose]();
				Reflect.apply(originalCallback, stream, args);
			}) as EosCallback;
		}
	}
	const resolverFn = (...args: unknown[]) => {
		if (!isAborted) nextTick(() => Reflect.apply(cb, stream, args));
	};
	const closed = stream[kIsClosedPromise] as { promise: Promise<unknown> };
	closed.promise.then(resolverFn, resolverFn);
	return nop;
}

export function finished(stream: unknown, opts?: FinishedOptions | null): Promise<void> {
	let autoCleanup = false;
	if (opts === null) opts = kEmptyObject;
	if (opts?.cleanup) {
		validateBoolean(opts.cleanup, "cleanup");
		autoCleanup = opts.cleanup;
	}
	const { promise, resolve, reject } = Promise.withResolvers<void>();
	const cleanup = eos(stream, opts, (err) => {
		if (autoCleanup) cleanup();
		if (err) reject(err);
		else resolve();
	});
	return promise;
}
