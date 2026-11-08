// Port of Node's lib/internal/webstreams/writablestream.js (transfer support dropped).

import {
	ERR_ILLEGAL_CONSTRUCTOR,
	ERR_INVALID_ARG_TYPE,
	ERR_INVALID_ARG_VALUE,
	ERR_INVALID_STATE,
	ERR_INVALID_THIS,
	customInspectSymbol as kInspect,
	kEmptyObject,
	kEnumerableProperty,
	kValidateObjectAllowObjects,
	kValidateObjectAllowObjectsAndNull,
	validateObject,
} from "internal:streams/shared";
import {
	Queue,
	createPromiseCallback1Param,
	createPromiseCallbackNoParams,
	createRawCallback2Params,
	customInspect,
	defaultSizeAlgorithm,
	dequeueValue,
	enqueueValueWithSize,
	extractHighWaterMark,
	extractSizeAlgorithm,
	getNonWritablePropertyDescriptor,
	isBrandCheck,
	isPromisePending,
	kDeferredStart,
	kEmptyQueue,
	kState,
	kType,
	nonOpCallback,
	nonOpCancel,
	peekQueueValue,
	rejectedHandledRecord,
	resetQueue,
	resolvedRecord,
	setPromiseHandled,
	thenAlgorithmResult,
} from "internal:webstreams/util";
import { kControllerErrorFunction, kIsClosedPromise, kIsErrored, kIsWritable } from "internal:streams/utils";

type Resolve = (value?: unknown) => void;
type Reject = (reason?: unknown) => void;
/** A { promise, resolve, reject } record; the nil request has all fields undefined. */
export interface PromiseRequest {
	promise: Promise<unknown> | undefined;
	resolve: Resolve | undefined;
	reject: Reject | undefined;
}
interface SettledRequest {
	promise: Promise<unknown>;
	resolve: Resolve;
	reject: Reject;
}
interface PendingAbortRequest {
	abort: PromiseRequest;
	reason: unknown;
	wasAlreadyErroring: boolean;
}
/** Shared request record used by pipeTo (see writableStreamDefaultWriterWriteWithRequest). */
export interface PipeWriteRequest {
	failed: boolean;
	failure: unknown;
	pending: number;
	resolve?: Resolve;
	reject?: Reject;
}

type StartAlgorithm = () => unknown;
type WriteAlgorithm = (chunk: unknown, controller: WritableStreamDefaultController) => unknown;
type CloseAlgorithm = (controller?: WritableStreamDefaultController) => Promise<unknown>;
type AbortAlgorithm = (reason: unknown, controller?: WritableStreamDefaultController) => Promise<unknown>;
type SizeAlgorithm = (chunk: unknown) => unknown;

interface ControllerState {
	abortAlgorithm: AbortAlgorithm | undefined;
	closeAlgorithm: CloseAlgorithm | undefined;
	highWaterMark: number;
	queue: Queue;
	queueTotalSize: number;
	abortController: AbortController;
	sizeAlgorithm: SizeAlgorithm | undefined;
	started: boolean;
	stream: WritableStream;
	transformStream: unknown;
	writeAlgorithm: WriteAlgorithm | undefined;
	writeFulfilled: (() => void) | undefined;
	writeRejected: ((error: unknown) => void) | undefined;
}

interface WriterState {
	stream: WritableStream | undefined;
	close: PromiseRequest | undefined;
	ready: PromiseRequest | undefined;
}

interface UnderlyingSink {
	start?: (controller: WritableStreamDefaultController) => unknown;
	write?: (chunk: unknown, controller: WritableStreamDefaultController) => unknown;
	close?: () => unknown;
	abort?: (reason: unknown) => unknown;
	type?: unknown;
}

interface QueuingStrategy {
	highWaterMark?: unknown;
	size?: unknown;
}

type InspectOptions = { depth?: number | null };

function assert(value: unknown): void {
	if (!value) {
		const err = new Error(
			"This is caused by either a bug in Node.js or incorrect usage of Node.js internals.\nPlease open an issue with this stack trace at https://github.com/nodejs/node/issues\n",
		) as Error & { code: string };
		err.code = "ERR_INTERNAL_ASSERTION";
		throw err;
	}
}

const kAbort = Symbol("kAbort");
const kCloseSentinel = Symbol("kCloseSentinel");
const kError = Symbol("kError");
const kSkipThrow = Symbol("kSkipThrow");

// Shared sentinels for the "no pending request" state records. These
// records are only ever replaced wholesale and never mutated in place.
const kNilRequest: PromiseRequest = Object.setPrototypeOf(
	{
		promise: undefined,
		resolve: undefined,
		reject: undefined,
	},
	null,
);
const kNilPendingAbortRequest: PendingAbortRequest = Object.setPrototypeOf(
	{
		abort: kNilRequest,
		reason: undefined,
		wasAlreadyErroring: false,
	},
	null,
);

let releasedError: Error | undefined;

function lazyWritableReleasedError(): Error {
	if (releasedError) {
		return releasedError;
	}
	const userModuleRegExp = /^ {4}at (?:[^/\\(]+ \()(?!node:(.+):\d+:\d+\)$).*/gm;

	releasedError = new ERR_INVALID_STATE.TypeError("Writer has been released");
	// Remove userland stack frames.
	releasedError.stack = String(releasedError.stack).replace(userModuleRegExp, "");
	return releasedError;
}

export class WritableStream {
	[kType] = "WritableStream";
	[kState]: WritableStreamState = undefined as unknown as WritableStreamState;

	constructor(
		sink: UnderlyingSink | typeof kSkipThrow = kEmptyObject,
		strategy: QueuingStrategy | null = kEmptyObject,
	) {
		// Internal construction (transform streams, adapters): the caller
		// sets up the controller.
		if (sink === kSkipThrow) {
			this[kState] = createWritableStreamState();
			return;
		}
		validateObject(sink, "sink", kValidateObjectAllowObjects);
		validateObject(strategy, "strategy", kValidateObjectAllowObjectsAndNull);
		const type = sink?.type;
		if (type !== undefined) throw new ERR_INVALID_ARG_VALUE.RangeError("type", type);

		this[kState] = createWritableStreamState();

		const size = extractSizeAlgorithm(strategy?.size);
		const highWaterMark = extractHighWaterMark(strategy?.highWaterMark, 1);

		setupWritableStreamDefaultControllerFromSink(this, sink, highWaterMark, size);
	}

	get [kIsErrored](): boolean {
		return this[kState].state === "errored";
	}

	get [kIsWritable](): boolean {
		return this[kState].state === "writable";
	}

	[kControllerErrorFunction](error: unknown): void {
		// Used by the internal stream interop (addAbortSignal).
		this[kState].controller!.error(error);
	}

	// Used by the internal stream interop (end-of-stream). Materialized
	// lazily since its settlement is derivable from the stream state.
	get [kIsClosedPromise](): PromiseRequest {
		let cache = this[kState].closedPromise;
		if (cache === undefined) {
			switch (this[kState].state) {
				case "writable":
				case "erroring":
					cache = Promise.withResolvers() as PromiseRequest;
					break;
				case "closed":
					cache = resolvedRecord() as PromiseRequest;
					break;
				default:
					cache = rejectedHandledRecord(this[kState].storedError) as PromiseRequest;
			}
			this[kState].closedPromise = cache;
		}
		return cache;
	}

	get locked(): boolean {
		if (!isWritableStream(this)) throw new ERR_INVALID_THIS("WritableStream");
		return isWritableStreamLocked(this);
	}

	abort(reason: unknown = undefined): Promise<unknown> {
		if (!isWritableStream(this)) return Promise.reject(new ERR_INVALID_THIS("WritableStream"));
		if (isWritableStreamLocked(this)) {
			return Promise.reject(new ERR_INVALID_STATE.TypeError("WritableStream is locked"));
		}
		return writableStreamAbort(this, reason);
	}

	close(): Promise<unknown> {
		if (!isWritableStream(this)) return Promise.reject(new ERR_INVALID_THIS("WritableStream"));
		if (isWritableStreamLocked(this)) {
			return Promise.reject(new ERR_INVALID_STATE.TypeError("WritableStream is locked"));
		}
		if (writableStreamCloseQueuedOrInFlight(this)) {
			return Promise.reject(new ERR_INVALID_STATE.TypeError("Failure closing WritableStream"));
		}
		return writableStreamClose(this);
	}

	getWriter(): WritableStreamDefaultWriter {
		if (!isWritableStream(this)) throw new ERR_INVALID_THIS("WritableStream");
		return new WritableStreamDefaultWriter(this);
	}

	[kInspect](depth: number, options: InspectOptions): unknown {
		return customInspect.call(this, depth, options, this[kType], {
			locked: this.locked,
			state: this[kState].state,
		});
	}
}

Object.defineProperties(WritableStream.prototype, {
	locked: kEnumerableProperty,
	abort: kEnumerableProperty,
	close: kEnumerableProperty,
	getWriter: kEnumerableProperty,
	[Symbol.toStringTag]: getNonWritablePropertyDescriptor(WritableStream.name),
});

export class WritableStreamDefaultWriter {
	[kType] = "WritableStreamDefaultWriter";
	[kState]: WriterState = undefined as unknown as WriterState;

	constructor(stream: WritableStream) {
		if (!isWritableStream(stream)) throw new ERR_INVALID_ARG_TYPE("stream", "WritableStream", stream);
		this[kState] = {
			stream: undefined,
			// Lazily materialized by writerClosedPromise()/writerReadyPromise().
			close: undefined,
			ready: undefined,
		};
		setupWritableStreamDefaultWriter(this, stream);
	}

	get closed(): Promise<unknown> {
		if (!isWritableStreamDefaultWriter(this))
			return Promise.reject(new ERR_INVALID_THIS("WritableStreamDefaultWriter"));
		return writerClosedPromise(this).promise!;
	}

	get desiredSize(): number | null {
		if (!isWritableStreamDefaultWriter(this)) throw new ERR_INVALID_THIS("WritableStreamDefaultWriter");
		if (this[kState].stream === undefined) {
			throw new ERR_INVALID_STATE.TypeError("Writer is not bound to a WritableStream");
		}
		return writableStreamDefaultWriterGetDesiredSize(this);
	}

	get ready(): Promise<unknown> {
		if (!isWritableStreamDefaultWriter(this))
			return Promise.reject(new ERR_INVALID_THIS("WritableStreamDefaultWriter"));
		return writerReadyPromise(this).promise!;
	}

	abort(reason: unknown = undefined): Promise<unknown> {
		if (!isWritableStreamDefaultWriter(this))
			return Promise.reject(new ERR_INVALID_THIS("WritableStreamDefaultWriter"));
		if (this[kState].stream === undefined) {
			return Promise.reject(new ERR_INVALID_STATE.TypeError("Writer is not bound to a WritableStream"));
		}
		return writableStreamDefaultWriterAbort(this, reason);
	}

	close(): Promise<unknown> {
		if (!isWritableStreamDefaultWriter(this))
			return Promise.reject(new ERR_INVALID_THIS("WritableStreamDefaultWriter"));
		const { stream } = this[kState];
		if (stream === undefined) {
			return Promise.reject(new ERR_INVALID_STATE.TypeError("Writer is not bound to a WritableStream"));
		}
		if (writableStreamCloseQueuedOrInFlight(stream)) {
			return Promise.reject(new ERR_INVALID_STATE.TypeError("Failure to close WritableStream"));
		}
		return writableStreamDefaultWriterClose(this);
	}

	releaseLock(): void {
		if (!isWritableStreamDefaultWriter(this)) throw new ERR_INVALID_THIS("WritableStreamDefaultWriter");
		const { stream } = this[kState];
		if (stream === undefined) return;
		assert(stream[kState].writer !== undefined);
		writableStreamDefaultWriterRelease(this);
	}

	write(chunk: unknown = undefined): Promise<unknown> {
		if (!isWritableStreamDefaultWriter(this))
			return Promise.reject(new ERR_INVALID_THIS("WritableStreamDefaultWriter"));
		if (this[kState].stream === undefined) {
			return Promise.reject(new ERR_INVALID_STATE.TypeError("Writer is not bound to a WritableStream"));
		}
		return writableStreamDefaultWriterWrite(this, chunk);
	}

	[kInspect](depth: number, options: InspectOptions): unknown {
		return customInspect.call(this, depth, options, this[kType], {
			stream: this[kState].stream,
			close: writerClosedPromise(this).promise,
			ready: writerReadyPromise(this).promise,
			desiredSize: this.desiredSize,
		});
	}
}

Object.defineProperties(WritableStreamDefaultWriter.prototype, {
	closed: kEnumerableProperty,
	ready: kEnumerableProperty,
	desiredSize: kEnumerableProperty,
	abort: kEnumerableProperty,
	close: kEnumerableProperty,
	releaseLock: kEnumerableProperty,
	write: kEnumerableProperty,
	[Symbol.toStringTag]: getNonWritablePropertyDescriptor(WritableStreamDefaultWriter.name),
});

export class WritableStreamDefaultController {
	[kType] = "WritableStreamDefaultController";
	[kState]: ControllerState = undefined as unknown as ControllerState;

	constructor(skipThrowSymbol: unknown = undefined) {
		if (skipThrowSymbol !== kSkipThrow) {
			throw new ERR_ILLEGAL_CONSTRUCTOR();
		}
	}

	[kAbort](reason: unknown): Promise<unknown> {
		const result = this[kState].abortAlgorithm!(reason, this);
		writableStreamDefaultControllerClearAlgorithms(this);
		return result;
	}

	[kError](): void {
		resetQueue(this);
	}

	get signal(): AbortSignal {
		if (!isWritableStreamDefaultController(this)) throw new ERR_INVALID_THIS("WritableStreamDefaultController");
		return this[kState].abortController.signal;
	}

	error(error: unknown = undefined): void {
		if (!isWritableStreamDefaultController(this)) throw new ERR_INVALID_THIS("WritableStreamDefaultController");
		if (this[kState].stream[kState].state !== "writable") return;
		writableStreamDefaultControllerError(this, error);
	}

	[kInspect](depth: number, options: InspectOptions): unknown {
		return customInspect.call(this, depth, options, this[kType], {
			stream: this[kState].stream,
		});
	}
}

Object.defineProperties(WritableStreamDefaultController.prototype, {
	signal: kEnumerableProperty,
	error: kEnumerableProperty,
	[Symbol.toStringTag]: getNonWritablePropertyDescriptor(WritableStreamDefaultController.name),
});

export function createWritableStream(
	start: StartAlgorithm,
	write: WriteAlgorithm,
	close: CloseAlgorithm,
	abort: AbortAlgorithm,
	highWaterMark = 1,
	size: SizeAlgorithm = defaultSizeAlgorithm,
): WritableStream {
	const stream = new WritableStream(kSkipThrow);
	setupWritableStreamDefaultController(
		stream,
		new WritableStreamDefaultController(kSkipThrow),
		start,
		write,
		close,
		abort,
		highWaterMark,
		size,
	);
	return stream;
}

export const isWritableStream = isBrandCheck("WritableStream") as (value: unknown) => value is WritableStream;
export const isWritableStreamDefaultWriter = isBrandCheck("WritableStreamDefaultWriter") as (
	value: unknown,
) => value is WritableStreamDefaultWriter;
export const isWritableStreamDefaultController = isBrandCheck("WritableStreamDefaultController") as (
	value: unknown,
) => value is WritableStreamDefaultController;

// The state record class nulls out its prototype chain to keep
// Object.prototype out of property lookups; every field ever assigned
// is declared so the shape never transitions.
class WritableStreamState {
	closedPromise: PromiseRequest | undefined = undefined;
	closeRequest: PromiseRequest = kNilRequest;
	// Mirrors "closeRequest or inFlightCloseRequest is pending".
	closeQueuedOrInFlight = false;
	inFlightWriteRequest: PromiseRequest = kNilRequest;
	inFlightCloseRequest: PromiseRequest = kNilRequest;
	pendingAbortRequest: PendingAbortRequest = kNilPendingAbortRequest;
	backpressure = false;
	controller: WritableStreamDefaultController | undefined = undefined;
	state: "writable" | "erroring" | "closed" | "errored" = "writable";
	storedError: unknown = undefined;
	// Ring-buffer request queue, materialized lazily on the first pending write.
	writeRequests: Queue = kEmptyQueue;
	writer: WritableStreamDefaultWriter | undefined = undefined;
}
Object.setPrototypeOf(WritableStreamState.prototype, null);

function createWritableStreamState(): WritableStreamState {
	return new WritableStreamState();
}

export function isWritableStreamLocked(stream: WritableStream): boolean {
	return stream[kState].writer !== undefined;
}

export function setupWritableStreamDefaultWriter(writer: WritableStreamDefaultWriter, stream: WritableStream): void {
	if (isWritableStreamLocked(stream)) throw new ERR_INVALID_STATE.TypeError("WritableStream is locked");
	writer[kState].stream = stream;
	stream[kState].writer = writer;
	// The writer's [[readyPromise]] and [[closedPromise]] are materialized
	// lazily by writerReadyPromise()/writerClosedPromise().
}

// Materializes the writer's [[closedPromise]] record on first observation.
export function writerClosedPromise(writer: WritableStreamDefaultWriter): PromiseRequest {
	let close = writer[kState].close;
	if (close === undefined) {
		const stream = writer[kState].stream;
		if (stream === undefined) {
			close = rejectedHandledRecord(lazyWritableReleasedError()) as PromiseRequest;
		} else {
			switch (stream[kState].state) {
				case "writable":
				case "erroring":
					close = Promise.withResolvers() as PromiseRequest;
					break;
				case "closed":
					close = resolvedRecord() as PromiseRequest;
					break;
				default:
					close = rejectedHandledRecord(stream[kState].storedError) as PromiseRequest;
			}
		}
		writer[kState].close = close;
	}
	return close;
}

// Same as writerClosedPromise(), for the writer's [[readyPromise]].
export function writerReadyPromise(writer: WritableStreamDefaultWriter): PromiseRequest {
	let ready = writer[kState].ready;
	if (ready === undefined) {
		const stream = writer[kState].stream;
		if (stream === undefined) {
			ready = rejectedHandledRecord(lazyWritableReleasedError()) as PromiseRequest;
		} else {
			switch (stream[kState].state) {
				case "writable":
					ready =
						!writableStreamCloseQueuedOrInFlight(stream) && stream[kState].backpressure
							? (Promise.withResolvers() as PromiseRequest)
							: (resolvedRecord() as PromiseRequest);
					break;
				case "closed":
					ready = resolvedRecord() as PromiseRequest;
					break;
				default:
					ready = rejectedHandledRecord(stream[kState].storedError) as PromiseRequest;
			}
		}
		writer[kState].ready = ready;
	}
	return ready;
}

export function writableStreamAbort(stream: WritableStream, reason: unknown): Promise<unknown> {
	const controller = stream[kState].controller!;

	let state = stream[kState].state;
	if (state === "closed" || state === "errored") return Promise.resolve();

	controller[kState].abortController.abort(reason);

	state = stream[kState].state;
	if (state === "closed" || state === "errored") return Promise.resolve();

	if (stream[kState].pendingAbortRequest.abort.promise !== undefined)
		return stream[kState].pendingAbortRequest.abort.promise;

	assert(state === "writable" || state === "erroring");

	let wasAlreadyErroring = false;
	if (state === "erroring") {
		wasAlreadyErroring = true;
		reason = undefined;
	}

	const abort = Promise.withResolvers() as SettledRequest;

	stream[kState].pendingAbortRequest = {
		abort,
		reason,
		wasAlreadyErroring,
	};

	if (!wasAlreadyErroring) writableStreamStartErroring(stream, reason);

	return abort.promise;
}

export function writableStreamClose(stream: WritableStream): Promise<unknown> {
	const { state, writer, backpressure, controller } = stream[kState];
	if (state === "closed" || state === "errored") {
		return Promise.reject(new ERR_INVALID_STATE.TypeError("WritableStream is closed"));
	}
	assert(state === "writable" || state === "erroring");
	assert(!writableStreamCloseQueuedOrInFlight(stream));
	const closeRequest = Promise.withResolvers() as SettledRequest;
	stream[kState].closeRequest = closeRequest;
	stream[kState].closeQueuedOrInFlight = true;
	const { promise } = closeRequest;
	if (writer !== undefined && backpressure && state === "writable") writer[kState].ready?.resolve?.();
	writableStreamDefaultControllerClose(controller!);
	return promise;
}

// Callers invoke this only in the 'writable' state with no close queued or
// in flight, so the backpressure value is derived here from the desired size.
export function writableStreamUpdateBackpressure(
	controller: WritableStreamDefaultController,
	streamState: WritableStreamState,
): void {
	const controllerState = controller[kState];
	const backpressure = controllerState.highWaterMark - controllerState.queueTotalSize <= 0;
	const writer = streamState.writer;
	const changed = streamState.backpressure !== backpressure;
	// The state field is published before the ready record is resolved so
	// that a ready resolve hook (pipeTo's pump continuation) observes the
	// new value.
	streamState.backpressure = backpressure;
	if (writer !== undefined && changed) {
		if (backpressure) {
			// The spec replaces [[readyPromise]] with a fresh pending promise;
			// dropping the cache lets the next observation derive it.
			writer[kState].ready = undefined;
		} else {
			writer[kState].ready?.resolve?.();
		}
	}
}

export function writableStreamStartErroring(stream: WritableStream, reason: unknown): void {
	assert(stream[kState].storedError === undefined);
	assert(stream[kState].state === "writable");
	const { controller, writer } = stream[kState];
	assert(controller !== undefined);
	stream[kState].state = "erroring";
	stream[kState].storedError = reason;
	if (writer !== undefined) {
		writableStreamDefaultWriterEnsureReadyPromiseRejected(writer, reason);
	}
	if (!writableStreamHasOperationMarkedInFlight(stream) && controller![kState].started) {
		writableStreamFinishErroring(stream);
	}
}

export function writableStreamRejectCloseAndClosedPromiseIfNeeded(stream: WritableStream): void {
	assert(stream[kState].state === "errored");
	if (stream[kState].closeRequest.promise !== undefined) {
		assert(stream[kState].inFlightCloseRequest.promise === undefined);
		stream[kState].closeRequest.reject!(stream[kState].storedError);
		stream[kState].closeRequest = kNilRequest;
		stream[kState].closeQueuedOrInFlight = false;
	}

	const closedPromiseCache = stream[kState].closedPromise;
	if (closedPromiseCache !== undefined && isPromisePending(closedPromiseCache.promise)) {
		setPromiseHandled(closedPromiseCache.promise!);
		closedPromiseCache.reject!(stream[kState].storedError);
	}

	const { writer } = stream[kState];
	if (writer !== undefined) {
		const closeCache = writer[kState].close;
		if (closeCache !== undefined && isPromisePending(closeCache.promise)) {
			setPromiseHandled(closeCache.promise!);
			closeCache.reject!(stream[kState].storedError);
		}
	}
}

export function writableStreamMarkFirstWriteRequestInFlight(stream: WritableStream): void {
	assert(stream[kState].inFlightWriteRequest.promise === undefined);
	assert(stream[kState].writeRequests.length);
	const writeRequest = stream[kState].writeRequests.shift() as PromiseRequest;
	stream[kState].inFlightWriteRequest = writeRequest;
}

export function writableStreamMarkCloseRequestInFlight(stream: WritableStream): void {
	assert(stream[kState].inFlightWriteRequest.promise === undefined);
	assert(stream[kState].closeRequest.promise !== undefined);
	stream[kState].inFlightCloseRequest = stream[kState].closeRequest;
	stream[kState].closeRequest = kNilRequest;
}

export function writableStreamHasOperationMarkedInFlight(stream: WritableStream): boolean {
	const { inFlightWriteRequest, inFlightCloseRequest } = stream[kState];
	if (inFlightWriteRequest.promise === undefined && inFlightCloseRequest.promise === undefined) {
		return false;
	}
	return true;
}

export function writableStreamFinishInFlightWriteWithError(stream: WritableStream, error: unknown): void {
	assert(stream[kState].inFlightWriteRequest.promise !== undefined);
	stream[kState].inFlightWriteRequest.reject!(error);
	stream[kState].inFlightWriteRequest = kNilRequest;
	assert(stream[kState].state === "writable" || stream[kState].state === "erroring");
	writableStreamDealWithRejection(stream, error);
}

export function writableStreamFinishInFlightWrite(stream: WritableStream): void {
	assert(stream[kState].inFlightWriteRequest.promise !== undefined);
	stream[kState].inFlightWriteRequest.resolve!();
	stream[kState].inFlightWriteRequest = kNilRequest;
}

export function writableStreamFinishInFlightCloseWithError(stream: WritableStream, error: unknown): void {
	assert(stream[kState].inFlightCloseRequest.promise !== undefined);
	stream[kState].inFlightCloseRequest.reject!(error);
	stream[kState].inFlightCloseRequest = kNilRequest;
	stream[kState].closeQueuedOrInFlight = false;
	assert(stream[kState].state === "writable" || stream[kState].state === "erroring");
	if (stream[kState].pendingAbortRequest.abort.promise !== undefined) {
		stream[kState].pendingAbortRequest.abort.reject!(error);
		stream[kState].pendingAbortRequest = kNilPendingAbortRequest;
	}
	writableStreamDealWithRejection(stream, error);
}

export function writableStreamFinishInFlightClose(stream: WritableStream): void {
	assert(stream[kState].inFlightCloseRequest.promise !== undefined);
	stream[kState].inFlightCloseRequest.resolve!();
	stream[kState].inFlightCloseRequest = kNilRequest;
	stream[kState].closeQueuedOrInFlight = false;
	if (stream[kState].state === "erroring") {
		stream[kState].storedError = undefined;
		if (stream[kState].pendingAbortRequest.abort.promise !== undefined) {
			stream[kState].pendingAbortRequest.abort.resolve!();
			stream[kState].pendingAbortRequest = kNilPendingAbortRequest;
		}
	}
	stream[kState].state = "closed";
	if (stream[kState].writer !== undefined) stream[kState].writer![kState].close?.resolve?.();
	stream[kState].closedPromise?.resolve?.();
	assert(stream[kState].pendingAbortRequest.abort.promise === undefined);
	assert(stream[kState].storedError === undefined);
}

export function writableStreamFinishErroring(stream: WritableStream): void {
	assert(stream[kState].state === "erroring");
	assert(!writableStreamHasOperationMarkedInFlight(stream));
	stream[kState].state = "errored";
	stream[kState].controller![kError]();
	const storedError = stream[kState].storedError;
	const writeRequests = stream[kState].writeRequests;
	while (writeRequests.length) (writeRequests.shift() as PromiseRequest).reject!(storedError);

	if (stream[kState].pendingAbortRequest.abort.promise === undefined) {
		writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
		return;
	}

	const abortRequest = stream[kState].pendingAbortRequest;
	stream[kState].pendingAbortRequest = kNilPendingAbortRequest;
	if (abortRequest.wasAlreadyErroring) {
		abortRequest.abort.reject!(storedError);
		writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
		return;
	}
	// The closed promises are only rejected once the sink's abort algorithm
	// settles, so materialize them here.
	stream[kState].closedPromise ??= Promise.withResolvers() as PromiseRequest;
	const writer = stream[kState].writer;
	if (writer !== undefined) writer[kState].close ??= Promise.withResolvers() as PromiseRequest;
	stream[kState].controller![kAbort](abortRequest.reason).then(
		() => {
			abortRequest.abort.resolve!();
			writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
		},
		(error: unknown) => {
			abortRequest.abort.reject!(error);
			writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
		},
	);
}

export function writableStreamDealWithRejection(stream: WritableStream, error: unknown): void {
	const { state } = stream[kState];
	if (state === "writable") {
		writableStreamStartErroring(stream, error);
		return;
	}

	assert(state === "erroring");
	writableStreamFinishErroring(stream);
}

export function writableStreamCloseQueuedOrInFlight(stream: WritableStream): boolean {
	return stream[kState].closeQueuedOrInFlight;
}

export function writableStreamAddWriteRequest(stream: WritableStream): Promise<unknown> {
	assert(isWritableStreamLocked(stream));
	assert(stream[kState].state === "writable");
	const writeRequest = Promise.withResolvers();
	const streamState = stream[kState];
	let writeRequests = streamState.writeRequests;
	if (writeRequests === kEmptyQueue) writeRequests = streamState.writeRequests = new Queue();
	writeRequests.push(writeRequest);
	return writeRequest.promise;
}

export function writableStreamDefaultWriterWrite(
	writer: WritableStreamDefaultWriter,
	chunk: unknown,
): Promise<unknown> {
	const writerState = writer[kState];
	const stream = writerState.stream;
	assert(stream !== undefined);
	const streamState = stream![kState];
	const controller = streamState.controller!;
	const chunkSize = writableStreamDefaultControllerGetChunkSize(controller, chunk);
	if (stream !== writerState.stream) {
		return Promise.reject(new ERR_INVALID_STATE.TypeError("Mismatched WritableStreams"));
	}
	const { state } = streamState;

	if (state === "errored") return Promise.reject(streamState.storedError);

	if (streamState.closeQueuedOrInFlight || state === "closed") {
		return Promise.reject(new ERR_INVALID_STATE.TypeError("WritableStream is closed"));
	}

	if (state === "erroring") return Promise.reject(streamState.storedError);

	assert(state === "writable");

	const promise = writableStreamAddWriteRequest(stream!);
	writableStreamDefaultControllerWrite(controller, chunk, chunkSize);
	return promise;
}

export function writableStreamDefaultWriterIsStarted(writer: WritableStreamDefaultWriter): boolean {
	const stream = writer[kState].stream;
	return stream === undefined || stream[kState].controller![kState].started;
}

// Returns the error a write through the writer would fail with, or
// undefined if the stream still accepts writes.
export function writableStreamDefaultWriterWriteFailure(writer: WritableStreamDefaultWriter): unknown {
	const streamState = writer[kState].stream![kState];
	const { state } = streamState;
	if (state === "errored" || state === "erroring") return streamState.storedError;
	if (streamState.closeQueuedOrInFlight || state === "closed")
		return new ERR_INVALID_STATE.TypeError("WritableStream is closed");
	return undefined;
}

// Variant of writableStreamDefaultWriterWrite for pipeTo: the caller
// provides a shared request object instead of a per-write promise record.
// `pending` is incremented before the controller write, which can settle
// requests synchronously when the stream starts erroring; precondition
// failures are latched on `failed`/`failure`.
export function writableStreamDefaultWriterWriteWithRequest(
	writer: WritableStreamDefaultWriter,
	chunk: unknown,
	request: PipeWriteRequest,
): void {
	const writerState = writer[kState];
	const stream = writerState.stream;
	assert(stream !== undefined);
	const streamState = stream![kState];
	const controller = streamState.controller!;
	const chunkSize = writableStreamDefaultControllerGetChunkSize(controller, chunk);
	if (stream !== writerState.stream) {
		request.failed = true;
		request.failure = new ERR_INVALID_STATE.TypeError("Mismatched WritableStreams");
		return;
	}
	const { state } = streamState;

	if (state === "errored") {
		request.failed = true;
		request.failure = streamState.storedError;
		return;
	}

	if (streamState.closeQueuedOrInFlight || state === "closed") {
		request.failed = true;
		request.failure = new ERR_INVALID_STATE.TypeError("WritableStream is closed");
		return;
	}

	if (state === "erroring") {
		request.failed = true;
		request.failure = streamState.storedError;
		return;
	}

	assert(state === "writable");

	let writeRequests = streamState.writeRequests;
	if (writeRequests === kEmptyQueue) writeRequests = streamState.writeRequests = new Queue();
	writeRequests.push(request);
	request.pending++;
	writableStreamDefaultControllerWrite(controller, chunk, chunkSize);
}

export function writableStreamDefaultWriterRelease(writer: WritableStreamDefaultWriter): void {
	const { stream } = writer[kState];
	assert(stream !== undefined);
	assert(stream![kState].writer === writer);
	const releasedStateError = lazyWritableReleasedError();
	writableStreamDefaultWriterEnsureReadyPromiseRejected(writer, releasedStateError);
	writableStreamDefaultWriterEnsureClosedPromiseRejected(writer, releasedStateError);
	stream![kState].writer = undefined;
	writer[kState].stream = undefined;
}

export function writableStreamDefaultWriterGetDesiredSize(writer: WritableStreamDefaultWriter): number | null {
	const stream = writer[kState].stream!;
	switch (stream[kState].state) {
		case "errored":
		// Fall through
		case "erroring":
			return null;
		case "closed":
			return 0;
	}
	return writableStreamDefaultControllerGetDesiredSize(stream[kState].controller!);
}

export function writableStreamDefaultWriterEnsureReadyPromiseRejected(
	writer: WritableStreamDefaultWriter,
	error: unknown,
): void {
	const ready = writer[kState].ready;
	if (ready !== undefined && isPromisePending(ready.promise)) {
		ready.reject!(error);
		setPromiseHandled(ready.promise!);
	} else {
		// The spec replaces [[readyPromise]] with a promise rejected with the
		// same error the post-release/post-erroring state derives; dropping
		// the cache lets the next observation materialize it.
		writer[kState].ready = undefined;
	}
}

export function writableStreamDefaultWriterEnsureClosedPromiseRejected(
	writer: WritableStreamDefaultWriter,
	error: unknown,
): void {
	const close = writer[kState].close;
	if (close !== undefined && isPromisePending(close.promise)) {
		close.reject!(error);
		setPromiseHandled(close.promise!);
	} else {
		// See writableStreamDefaultWriterEnsureReadyPromiseRejected.
		writer[kState].close = undefined;
	}
}

export function writableStreamDefaultWriterCloseWithErrorPropagation(
	writer: WritableStreamDefaultWriter,
): Promise<unknown> {
	const { stream } = writer[kState];
	assert(stream !== undefined);
	const { state } = stream![kState];
	if (writableStreamCloseQueuedOrInFlight(stream!) || state === "closed") return Promise.resolve();

	if (state === "errored") return Promise.reject(stream![kState].storedError);

	assert(state === "writable" || state === "erroring");

	return writableStreamDefaultWriterClose(writer);
}

export function writableStreamDefaultWriterClose(writer: WritableStreamDefaultWriter): Promise<unknown> {
	const { stream } = writer[kState];
	assert(stream !== undefined);
	return writableStreamClose(stream!);
}

export function writableStreamDefaultWriterAbort(
	writer: WritableStreamDefaultWriter,
	reason: unknown,
): Promise<unknown> {
	const { stream } = writer[kState];
	assert(stream !== undefined);
	return writableStreamAbort(stream!, reason);
}

export function writableStreamDefaultControllerWrite(
	controller: WritableStreamDefaultController,
	chunk: unknown,
	chunkSize: unknown,
): void {
	try {
		enqueueValueWithSize(controller, chunk, chunkSize);
	} catch (error) {
		writableStreamDefaultControllerErrorIfNeeded(controller, error);
		return;
	}
	const { stream } = controller[kState];
	const streamState = stream[kState];
	if (!streamState.closeQueuedOrInFlight && streamState.state === "writable") {
		writableStreamUpdateBackpressure(controller, streamState);
	}
	writableStreamDefaultControllerAdvanceQueueIfNeeded(controller);
}

export function writableStreamDefaultControllerProcessWrite(
	controller: WritableStreamDefaultController,
	chunk: unknown,
): void {
	const { stream, writeAlgorithm } = controller[kState];
	writableStreamMarkFirstWriteRequestInFlight(stream);

	if (controller[kState].writeFulfilled === undefined) {
		// The write reaction closures only capture the stream and controller,
		// so they are created once on the first write and reused.
		controller[kState].writeFulfilled = () => {
			writableStreamFinishInFlightWrite(stream);
			const streamState = stream[kState];
			const { state } = streamState;
			assert(state === "writable" || state === "erroring");
			dequeueValue(controller);
			if (!streamState.closeQueuedOrInFlight && state === "writable") {
				writableStreamUpdateBackpressure(controller, streamState);
			}
			writableStreamDefaultControllerAdvanceQueueIfNeeded(controller);
		};
		controller[kState].writeRejected = (error: unknown) => {
			if (stream[kState].state === "writable") writableStreamDefaultControllerClearAlgorithms(controller);
			writableStreamFinishInFlightWriteWithError(stream, error);
		};
	}

	// The write algorithm may be a raw callback or an internal algorithm
	// that always returns a promise; thenAlgorithmResult handles both.
	let result: unknown;
	try {
		result = writeAlgorithm!(chunk, controller);
	} catch (error) {
		result = Promise.reject(error);
	}
	thenAlgorithmResult(
		result,
		controller[kState].writeFulfilled as () => void,
		controller[kState].writeRejected as (error: unknown) => void,
	);
}

export function writableStreamDefaultControllerProcessClose(controller: WritableStreamDefaultController): void {
	const { closeAlgorithm, queue, stream } = controller[kState];
	writableStreamMarkCloseRequestInFlight(stream);
	dequeueValue(controller);
	assert(!queue.length);
	const sinkClosePromise = closeAlgorithm!(controller);
	writableStreamDefaultControllerClearAlgorithms(controller);
	sinkClosePromise.then(
		() => writableStreamFinishInFlightClose(stream),
		(error: unknown) => writableStreamFinishInFlightCloseWithError(stream, error),
	);
}

export function writableStreamDefaultControllerGetDesiredSize(controller: WritableStreamDefaultController): number {
	const { highWaterMark, queueTotalSize } = controller[kState];
	return highWaterMark - queueTotalSize;
}

export function writableStreamDefaultControllerGetChunkSize(
	controller: WritableStreamDefaultController,
	chunk: unknown,
): unknown {
	const { stream, sizeAlgorithm } = controller[kState];
	if (sizeAlgorithm === undefined) {
		// The algorithms are also cleared while the close request is in
		// flight, before the stream leaves the 'writable' state; the caller
		// then rejects the write as closing.
		assert(stream[kState].state !== "writable" || stream[kState].closeQueuedOrInFlight);
		return 1;
	}

	// The internal default size algorithm always returns 1 and cannot throw.
	if (sizeAlgorithm === defaultSizeAlgorithm) return 1;

	try {
		return sizeAlgorithm.call(undefined, chunk);
	} catch (error) {
		writableStreamDefaultControllerErrorIfNeeded(controller, error);
		return 1;
	}
}

export function writableStreamDefaultControllerErrorIfNeeded(
	controller: WritableStreamDefaultController,
	error: unknown,
): void {
	const { stream } = controller[kState];
	if (stream[kState].state === "writable") writableStreamDefaultControllerError(controller, error);
}

export function writableStreamDefaultControllerError(
	controller: WritableStreamDefaultController,
	error: unknown,
): void {
	const { stream } = controller[kState];
	assert(stream[kState].state === "writable");
	writableStreamDefaultControllerClearAlgorithms(controller);
	writableStreamStartErroring(stream, error);
}

export function writableStreamDefaultControllerClose(controller: WritableStreamDefaultController): void {
	enqueueValueWithSize(controller, kCloseSentinel, 0);
	writableStreamDefaultControllerAdvanceQueueIfNeeded(controller);
}

export function writableStreamDefaultControllerClearAlgorithms(controller: WritableStreamDefaultController): void {
	controller[kState].writeAlgorithm = undefined;
	controller[kState].closeAlgorithm = undefined;
	controller[kState].abortAlgorithm = undefined;
	controller[kState].sizeAlgorithm = undefined;
}

export function writableStreamDefaultControllerAdvanceQueueIfNeeded(controller: WritableStreamDefaultController): void {
	const { queue, started, stream } = controller[kState];
	if (!started || stream[kState].inFlightWriteRequest.promise !== undefined) return;

	if (stream[kState].state === "erroring") {
		writableStreamFinishErroring(stream);
		return;
	}

	if (!queue.length) return;

	const value = peekQueueValue(controller);
	if (value === kCloseSentinel) writableStreamDefaultControllerProcessClose(controller);
	else writableStreamDefaultControllerProcessWrite(controller, value);
}

export function setupWritableStreamDefaultControllerFromSink(
	stream: WritableStream,
	sink: UnderlyingSink,
	highWaterMark: number,
	sizeAlgorithm: SizeAlgorithm,
): void {
	const controller = new WritableStreamDefaultController(kSkipThrow);
	const start = sink?.start;
	const write = sink?.write;
	const close = sink?.close;
	const abort = sink?.abort;
	const startAlgorithm: StartAlgorithm = start ? start.bind(sink, controller) : nonOpCallback;
	const writeAlgorithm = (
		write ? createRawCallback2Params("sink.write", write, sink) : nonOpCallback
	) as WriteAlgorithm;
	const closeAlgorithm = (
		close ? createPromiseCallbackNoParams("sink.close", close, sink) : nonOpCancel
	) as CloseAlgorithm;
	const abortAlgorithm = (
		abort ? createPromiseCallback1Param("sink.abort", abort, sink) : nonOpCancel
	) as AbortAlgorithm;
	setupWritableStreamDefaultController(
		stream,
		controller,
		startAlgorithm,
		writeAlgorithm,
		closeAlgorithm,
		abortAlgorithm,
		highWaterMark,
		sizeAlgorithm,
	);
}

export function setupWritableStreamDefaultController(
	stream: WritableStream,
	controller: WritableStreamDefaultController,
	startAlgorithm: StartAlgorithm,
	writeAlgorithm: WriteAlgorithm,
	closeAlgorithm: CloseAlgorithm,
	abortAlgorithm: AbortAlgorithm,
	highWaterMark: number,
	sizeAlgorithm: SizeAlgorithm,
): void {
	assert(isWritableStream(stream));
	assert(stream[kState].controller === undefined);
	controller[kState] = {
		abortAlgorithm,
		closeAlgorithm,
		highWaterMark,
		queue: kEmptyQueue,
		queueTotalSize: 0,
		abortController: new AbortController(),
		sizeAlgorithm,
		started: false,
		stream,
		// The TransformStream this controller is the sink of, if any.
		transformStream: undefined,
		writeAlgorithm,
		writeFulfilled: undefined,
		writeRejected: undefined,
	};
	stream[kState].controller = controller;

	writableStreamUpdateBackpressure(controller, stream[kState]);

	const startResult = startAlgorithm();
	if (startResult === kDeferredStart) return;

	const started = () => writableStreamDefaultControllerStarted(controller);

	if (startResult === null || (typeof startResult !== "object" && typeof startResult !== "function")) {
		// Non-thenable start result: run the post-start step at the exact
		// microtask position the promise reaction would have had.
		queueMicrotask(started);
		return;
	}

	// The wrapper promise matches the reference implementation's
	// promiseResolvedWith(), whose extra microtask hops WPT relies on.
	const wrapper = Promise.withResolvers<unknown>();
	wrapper.resolve(startResult);
	wrapper.promise.then(started, (error: unknown) => writableStreamDefaultControllerStartFailed(controller, error));
}

// The post-start steps; transform streams deliver them directly.
export function writableStreamDefaultControllerStarted(controller: WritableStreamDefaultController): void {
	const { stream } = controller[kState];
	assert(stream[kState].state === "writable" || stream[kState].state === "erroring");
	controller[kState].started = true;
	writableStreamDefaultControllerAdvanceQueueIfNeeded(controller);
}

export function writableStreamDefaultControllerStartFailed(
	controller: WritableStreamDefaultController,
	error: unknown,
): void {
	const { stream } = controller[kState];
	assert(stream[kState].state === "writable" || stream[kState].state === "erroring");
	controller[kState].started = true;
	writableStreamDealWithRejection(stream, error);
}
