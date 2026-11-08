// The legacy Stream base class (Node's lib/internal/streams/legacy.js).

import EE from "node:events";

type Listener = (...args: unknown[]) => void;
type EmitterLike = {
	on(event: string | symbol, fn: Listener): unknown;
	prependListener?(event: string | symbol, fn: Listener): unknown;
	_events?: Record<string | symbol, Listener | Listener[] | undefined>;
};
// Stream is the duck-typed object `pipe` works with (any EventEmitter-like source/destination).
type PipeEnd = EE & {
	writable?: boolean;
	readable?: boolean;
	write(chunk: unknown): boolean;
	end(): void;
	pause?(): void;
	resume?(): void;
	destroy?(): void;
	_isStdio?: boolean;
};

export interface Stream extends EE {
	pipe<T>(dest: T, options?: { end?: boolean }): T;
}

/** Plain function constructor so `Stream.call(this)` works for legacy subclasses. */
export const Stream = function Stream(this: EE, opts?: { captureRejections?: boolean }): void {
	(EE as unknown as { init(this: EE, o?: object): void }).init.call(this, opts);
} as unknown as { new (opts?: object): Stream; (this: unknown, opts?: object): void; prototype: Stream };

Object.setPrototypeOf(Stream.prototype, EE.prototype);
Object.setPrototypeOf(Stream, EE);

Stream.prototype.pipe = function pipe<T>(this: PipeEnd, destArg: T, options?: { end?: boolean }): T {
	const source = this;
	const dest = destArg as unknown as PipeEnd;

	function ondata(chunk: unknown) {
		if (dest.writable && dest.write(chunk) === false && source.pause) source.pause();
	}
	source.on("data", ondata);

	function ondrain() {
		if (source.readable && source.resume) source.resume();
	}
	dest.on("drain", ondrain);

	// If the 'end' option is not supplied, dest.end() will be called when
	// source gets the 'end' or 'close' events. Only dest.end() once.
	if (!dest._isStdio && (!options || options.end !== false)) {
		source.on("end", onend);
		source.on("close", onclose);
	}

	let didOnEnd = false;
	function onend() {
		if (didOnEnd) return;
		didOnEnd = true;
		dest.end();
	}

	function onclose() {
		if (didOnEnd) return;
		didOnEnd = true;
		if (typeof dest.destroy === "function") dest.destroy();
	}

	// Don't leave dangling pipes when there are errors.
	function onerror(this: EE, er: unknown) {
		cleanup();
		// If we removed the last error handler, trigger an unhandled error event.
		if (this.listenerCount?.("error") === 0) this.emit("error", er);
	}

	prependListener(source, "error", onerror as Listener);
	prependListener(dest, "error", onerror as Listener);

	function cleanup() {
		source.removeListener("data", ondata);
		dest.removeListener("drain", ondrain);
		source.removeListener("end", onend);
		source.removeListener("close", onclose);
		source.removeListener("error", onerror as Listener);
		dest.removeListener("error", onerror as Listener);
		source.removeListener("end", cleanup);
		source.removeListener("close", cleanup);
		dest.removeListener("close", cleanup);
	}

	source.on("end", cleanup);
	source.on("close", cleanup);
	dest.on("close", cleanup);
	dest.emit("pipe", source);

	// Allow for unix-like usage: A.pipe(B).pipe(C)
	return destArg;
};

Stream.prototype.eventNames = function eventNames(this: EE): (string | symbol)[] {
	const events = (this as unknown as { _events: Record<string | symbol, unknown> })._events;
	const names: (string | symbol)[] = [];
	for (const key of Reflect.ownKeys(events)) {
		const v = events[key];
		if (typeof v === "function" || (Array.isArray(v) && v.length > 0)) names.push(key);
	}
	return names;
};

export function prependListener(emitter: EmitterLike, event: string | symbol, fn: Listener): unknown {
	// Not cacheable: some libraries bundle their own event emitter implementation.
	if (typeof emitter.prependListener === "function") return emitter.prependListener(event, fn);

	// Hack for emitters without prependListener(): put our handler first.
	const events = emitter._events;
	const existing = events?.[event];
	if (!events || !existing) emitter.on(event, fn);
	else if (Array.isArray(existing)) existing.unshift(fn);
	else events[event] = [fn, existing];
	return undefined;
}
