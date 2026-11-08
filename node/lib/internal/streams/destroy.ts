// destroy()/construct() machinery shared by Readable/Writable/Duplex
// (Node's lib/internal/streams/destroy.js).

import { AbortError, aggregateTwoErrors, ERR_MULTIPLE_CALLBACK, nextTick } from "internal:streams/shared";
import {
	isDestroyed,
	isFinished,
	isServerRequest,
	kAutoDestroy,
	kCloseEmitted,
	kClosed,
	kConstructed,
	kDestroyed,
	kEmitClose,
	kErrorEmitted,
	kErrored,
	kIsDestroyed,
	kState,
	type StreamLike,
} from "internal:streams/utils";

type Callback = (err?: unknown) => void;

/** A Readable/Writable state object: flag bits in [kState] plus accessor properties. */
interface State {
	[kState]: number;
	errored: unknown;
	destroyed: boolean;
	[key: string]: unknown;
}

/** The parts of a Node stream (`this` of Readable/Writable/Duplex methods) used here. */
interface Self {
	_readableState?: State | null;
	_writableState?: State | null;
	_destroy(err: Error | null, cb: Callback): void;
	_construct?: (cb: Callback) => void;
	destroy(err?: unknown, cb?: Callback): unknown;
	emit(event: string | symbol, ...args: unknown[]): boolean;
	once(event: string | symbol, fn: (this: Self, ...args: unknown[]) => void): unknown;
	listenerCount(event: string | symbol): number;
	destroyed?: boolean;
	[key: string | symbol]: unknown;
}

const kDestroy = Symbol("kDestroy");
const kConstruct = Symbol("kConstruct");

function checkError(err: unknown, w: State | null | undefined, r: State | null | undefined): void {
	if (err) {
		if (w && !w.errored) w.errored = err;
		if (r && !r.errored) r.errored = err;
	}
}

// Backwards compat. cb() is undocumented and unused in core but
// unfortunately might be used by modules.
export function destroy(this: Self, err?: unknown, cb?: Callback): Self {
	const r = this._readableState;
	const w = this._writableState;
	// With duplex streams we use the writable side for state.
	const s = (w || r) as State;

	if ((w && (w[kState] & kDestroyed) !== 0) || (r && (r[kState] & kDestroyed) !== 0)) {
		if (typeof cb === "function") cb();
		return this;
	}

	// We set destroyed to true before firing error callbacks in order
	// to make it re-entrance safe in case destroy() is called within callbacks
	checkError(err, w, r);

	if (w) w[kState] |= kDestroyed;
	if (r) r[kState] |= kDestroyed;

	// If still constructing then defer calling _destroy.
	if ((s[kState] & kConstructed) === 0) {
		this.once(kDestroy, function (this: Self, er: unknown) {
			_destroy(this, aggregateTwoErrors(er, err), cb);
		});
	} else {
		_destroy(this, err, cb);
	}
	return this;
}

function _destroy(self: Self, err: unknown, cb: Callback | undefined): void {
	let called = false;

	function onDestroy(err?: unknown) {
		if (called) return;
		called = true;

		const r = self._readableState;
		const w = self._writableState;

		checkError(err, w, r);

		if (w) w[kState] |= kClosed;
		if (r) r[kState] |= kClosed;

		if (typeof cb === "function") cb(err);

		if (err) nextTick(emitErrorCloseNT as (...a: unknown[]) => void, self, err);
		else nextTick(emitCloseNT as (...a: unknown[]) => void, self);
	}
	try {
		self._destroy((err as Error) || null, onDestroy);
	} catch (e) {
		onDestroy(e);
	}
}

function emitErrorCloseNT(self: Self, err: unknown): void {
	emitErrorNT(self, err);
	emitCloseNT(self);
}

function emitCloseNT(self: Self): void {
	const r = self._readableState;
	const w = self._writableState;

	if (w) w[kState] |= kCloseEmitted;
	if (r) r[kState] |= kCloseEmitted;

	if ((w && (w[kState] & kEmitClose) !== 0) || (r && (r[kState] & kEmitClose) !== 0)) {
		self.emit("close");
	}
}

function emitErrorNT(self: Self, err: unknown): void {
	const r = self._readableState;
	const w = self._writableState;

	if ((w && (w[kState] & kErrorEmitted) !== 0) || (r && (r[kState] & kErrorEmitted) !== 0)) return;

	if (w) w[kState] |= kErrorEmitted;
	if (r) r[kState] |= kErrorEmitted;

	self.emit("error", err);
}

export function undestroy(this: Self): void {
	const r = this._readableState;
	const w = this._writableState;

	if (r) {
		r.constructed = true;
		r.closed = false;
		r.closeEmitted = false;
		r.destroyed = false;
		r.errored = null;
		r.errorEmitted = false;
		r.reading = false;
		r.ended = r.readable === false;
		r.endEmitted = r.readable === false;
	}

	if (w) {
		w.constructed = true;
		w.destroyed = false;
		w.closed = false;
		w.closeEmitted = false;
		w.errored = null;
		w.errorEmitted = false;
		w.finalCalled = false;
		w.prefinished = false;
		w.ended = w.writable === false;
		w.ending = w.writable === false;
		w.finished = w.writable === false;
	}
}

export function errorOrDestroy(stream: Self, err: unknown, sync?: boolean): void {
	// We have tests that rely on errors being emitted in the same tick, so
	// changing this is semver major. With autoDestroy the error is emitted
	// on the next tick.
	const r = stream._readableState;
	const w = stream._writableState;

	if (
		(w && (w[kState] !== undefined ? (w[kState] & kDestroyed) !== 0 : w.destroyed)) ||
		(r && (r[kState] !== undefined ? (r[kState] & kDestroyed) !== 0 : r.destroyed))
	) {
		return;
	}

	if ((r && (r[kState] & kAutoDestroy) !== 0) || (w && (w[kState] & kAutoDestroy) !== 0)) {
		stream.destroy(err);
	} else if (err) {
		if (w && (w[kState] & kErrored) === 0) w.errored = err;
		if (r && (r[kState] & kErrored) === 0) r.errored = err;
		if (sync) nextTick(emitErrorNT as (...a: unknown[]) => void, stream, err);
		else emitErrorNT(stream, err);
	}
}

export function construct(stream: Self, cb: () => void): void {
	if (typeof stream._construct !== "function") return;

	const r = stream._readableState;
	const w = stream._writableState;

	if (r) r[kState] &= ~kConstructed;
	if (w) w[kState] &= ~kConstructed;

	stream.once(kConstruct, cb);

	if (stream.listenerCount(kConstruct) > 1) {
		// Duplex
		return;
	}

	nextTick(constructNT as (...a: unknown[]) => void, stream);
}

function constructNT(stream: Self): void {
	let called = false;

	function onConstruct(err?: unknown) {
		if (called) {
			errorOrDestroy(stream, err ?? new ERR_MULTIPLE_CALLBACK());
			return;
		}
		called = true;

		const r = stream._readableState;
		const w = stream._writableState;
		const s = (w || r) as State;

		if (r) r[kState] |= kConstructed;
		if (w) w[kState] |= kConstructed;

		if (s.destroyed) stream.emit(kDestroy, err);
		else if (err) errorOrDestroy(stream, err, true);
		else stream.emit(kConstruct);
	}

	try {
		(stream._construct as (cb: Callback) => void)((err) => {
			nextTick(onConstruct as (...a: unknown[]) => void, err);
		});
	} catch (err) {
		nextTick(onConstruct as (...a: unknown[]) => void, err);
	}
}

interface RequestLike {
	setHeader?: unknown;
	abort(): void;
}

function isRequest(stream: unknown): stream is RequestLike {
	const s = stream as { setHeader?: unknown; abort?: unknown } | null | undefined;
	return !!(s?.setHeader && typeof s.abort === "function");
}

function emitCloseLegacy(stream: Self): void {
	stream.emit("close");
}

function emitErrorCloseLegacy(stream: Self, err: unknown): void {
	stream.emit("error", err);
	nextTick(emitCloseLegacy as (...a: unknown[]) => void, stream);
}

/** Normalize destroy for legacy streams and requests. */
export function destroyer(stream: StreamLike | null | undefined, err?: unknown): void {
	if (!stream || isDestroyed(stream)) return;

	if (!err && !isFinished(stream)) err = new AbortError();

	const s = stream as unknown as Self & { socket?: unknown; req?: unknown; close?: unknown };
	if (isServerRequest(stream)) {
		s.socket = null;
		s.destroy(err);
	} else if (isRequest(s)) {
		s.abort();
	} else if (isRequest(s.req)) {
		s.req.abort();
	} else if (typeof s.destroy === "function") {
		s.destroy(err);
	} else if (typeof s.close === "function") {
		(s.close as () => void)();
	} else if (err) {
		nextTick(emitErrorCloseLegacy as (...a: unknown[]) => void, s, err);
	} else {
		nextTick(emitCloseLegacy as (...a: unknown[]) => void, s);
	}

	if (!s.destroyed) s[kIsDestroyed] = true;
}
