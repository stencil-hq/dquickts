// Port of Node's lib/internal/webstreams/transformstream.js (transfer support and
// async-context wiring dropped).

import {
	ERR_ILLEGAL_CONSTRUCTOR,
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
	createPromiseCallback1Param,
	createRawCallback2Params,
	customInspect,
	deferredStartAlgorithm,
	extractHighWaterMark,
	extractSizeAlgorithm,
	getNonWritablePropertyDescriptor,
	isBrandCheck,
	kParkedAlgorithmResult,
	kResolvedPromise,
	kState,
	kType,
	nonOpCancel,
	nonOpFlush,
} from "internal:webstreams/util";
import {
	ReadableStream,
	createReadableStream,
	readableStreamDefaultControllerCanCloseOrEnqueue,
	readableStreamDefaultControllerClose,
	readableStreamDefaultControllerEnqueue,
	readableStreamDefaultControllerError,
	readableStreamDefaultControllerGetDesiredSize,
	readableStreamDefaultControllerHasBackpressure,
	readableStreamDefaultControllerStarted,
} from "internal:webstreams/readablestream";
import {
	WritableStream,
	WritableStreamDefaultController,
	createWritableStream,
	writableStreamDefaultControllerErrorIfNeeded,
	writableStreamDefaultControllerStartFailed,
	writableStreamDefaultControllerStarted,
} from "internal:webstreams/writablestream";

/** The parts of the readable side's internal state this module touches. */
interface ReadableControllerInternals {
	[kState]: {
		transformStream: TransformStream | undefined;
		pullFulfilled: (() => void) | undefined;
	};
}
interface ReadableInternals {
	[kState]: {
		controller: ReadableControllerInternals;
		state: string;
		storedError: unknown;
	};
}
interface WritableControllerInternals {
	[kState]: {
		transformStream: TransformStream | undefined;
		writeFulfilled: (() => void) | undefined;
		writeRejected: ((error: unknown) => void) | undefined;
	};
}
interface WritableInternals {
	[kState]: {
		controller: WritableControllerInternals;
		state: string;
		storedError: unknown;
	};
}

type TransformAlgorithm = (chunk: unknown, controller: TransformStreamDefaultController) => unknown;
type PromiseAlgorithm = (arg: unknown) => Promise<unknown>;

interface Transformer {
	start?: (controller: TransformStreamDefaultController) => unknown;
	transform?: (chunk: unknown, controller: TransformStreamDefaultController) => unknown;
	flush?: (controller: TransformStreamDefaultController) => unknown;
	cancel?: (reason: unknown) => unknown;
	readableType?: unknown;
	writableType?: unknown;
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

const kSkipThrow = Symbol("kSkipThrow");

// The readable/writable internals are typed by their own modules; view them
// through the narrow shapes above.
function readableInternals(readable: ReadableStream | undefined): ReadableInternals[typeof kState] {
	return (readable as unknown as ReadableInternals)[kState];
}
function writableInternals(writable: WritableStream | undefined): WritableInternals[typeof kState] {
	return (writable as unknown as WritableInternals)[kState];
}

export class TransformStream {
	[kType] = "TransformStream";
	[kState]: TransformStreamState = undefined as unknown as TransformStreamState;

	constructor(
		transformer: Transformer = kEmptyObject,
		writableStrategy: QueuingStrategy | null = kEmptyObject,
		readableStrategy: QueuingStrategy | null = kEmptyObject,
	) {
		validateObject(transformer, "transformer", kValidateObjectAllowObjects);
		validateObject(writableStrategy, "writableStrategy", kValidateObjectAllowObjectsAndNull);
		validateObject(readableStrategy, "readableStrategy", kValidateObjectAllowObjectsAndNull);
		const readableType = transformer?.readableType;
		const writableType = transformer?.writableType;
		const start = transformer?.start;

		if (readableType !== undefined) {
			throw new ERR_INVALID_ARG_VALUE.RangeError("transformer.readableType", readableType);
		}
		if (writableType !== undefined) {
			throw new ERR_INVALID_ARG_VALUE.RangeError("transformer.writableType", writableType);
		}

		const readableHighWaterMark = readableStrategy?.highWaterMark;
		const readableSize = readableStrategy?.size;

		const writableHighWaterMark = writableStrategy?.highWaterMark;
		const writableSize = writableStrategy?.size;

		const actualReadableHighWaterMark = extractHighWaterMark(readableHighWaterMark, 0);
		const actualReadableSize = extractSizeAlgorithm(readableSize);

		const actualWritableHighWaterMark = extractHighWaterMark(writableHighWaterMark, 1);
		const actualWritableSize = extractSizeAlgorithm(writableSize);

		initializeTransformStream(
			this,
			actualWritableHighWaterMark,
			actualWritableSize,
			actualReadableHighWaterMark,
			actualReadableSize,
		);

		setupTransformStreamDefaultControllerFromTransformer(this, transformer);

		transformStreamStart(this, start === undefined ? undefined : start.call(transformer, this[kState].controller!));
	}

	get readable(): ReadableStream {
		if (!isTransformStream(this)) throw new ERR_INVALID_THIS("TransformStream");
		return this[kState].readable!;
	}

	get writable(): WritableStream {
		if (!isTransformStream(this)) throw new ERR_INVALID_THIS("TransformStream");
		return this[kState].writable!;
	}

	[kInspect](depth: number, options: InspectOptions): unknown {
		return customInspect.call(this, depth, options, this[kType], {
			readable: this.readable,
			writable: this.writable,
			backpressure: this[kState].backpressure,
		});
	}
}

Object.defineProperties(TransformStream.prototype, {
	readable: kEnumerableProperty,
	writable: kEnumerableProperty,
	[Symbol.toStringTag]: getNonWritablePropertyDescriptor(TransformStream.name),
});

// The state record classes null out their prototype chain to keep
// Object.prototype out of property lookups.
class TransformStreamState {
	readable: ReadableStream | undefined = undefined;
	writable: WritableStream | undefined = undefined;
	controller: TransformStreamDefaultController | undefined = undefined;
	backpressure: boolean | undefined = undefined;
	// Continuation slots replacing the spec's [[backpressureChangePromise]];
	// see transformStreamSetBackpressure.
	pullPending = false;
	pendingWriteParked = false;
	pendingWriteChunk: unknown = undefined;
	writeContinuation: (() => void) | undefined = undefined;
}
Object.setPrototypeOf(TransformStreamState.prototype, null);

class TransformStreamControllerState {
	stream: TransformStream | undefined = undefined;
	transformAlgorithm: TransformAlgorithm | undefined = undefined;
	flushAlgorithm: PromiseAlgorithm | undefined = undefined;
	cancelAlgorithm: PromiseAlgorithm | undefined = undefined;
	performTransformRejected: ((error: unknown) => never) | undefined = undefined;
	finishPromise: Promise<unknown> | undefined = undefined;
}
Object.setPrototypeOf(TransformStreamControllerState.prototype, null);

export class TransformStreamDefaultController {
	[kType] = "TransformStreamDefaultController";
	[kState]: TransformStreamControllerState = undefined as unknown as TransformStreamControllerState;

	constructor(skipThrowSymbol: unknown = undefined) {
		if (skipThrowSymbol !== kSkipThrow) {
			throw new ERR_ILLEGAL_CONSTRUCTOR();
		}
	}

	get desiredSize(): number | null {
		if (!isTransformStreamDefaultController(this)) throw new ERR_INVALID_THIS("TransformStreamDefaultController");
		const stream = this[kState].stream!;
		const readableController = readableInternals(stream[kState].readable).controller;
		return readableStreamDefaultControllerGetDesiredSize(readableController as never);
	}

	enqueue(chunk: unknown = undefined): void {
		if (!isTransformStreamDefaultController(this)) throw new ERR_INVALID_THIS("TransformStreamDefaultController");
		transformStreamDefaultControllerEnqueue(this, chunk);
	}

	error(reason: unknown = undefined): void {
		if (!isTransformStreamDefaultController(this)) throw new ERR_INVALID_THIS("TransformStreamDefaultController");
		transformStreamDefaultControllerError(this, reason);
	}

	terminate(): void {
		if (!isTransformStreamDefaultController(this)) throw new ERR_INVALID_THIS("TransformStreamDefaultController");
		transformStreamDefaultControllerTerminate(this);
	}

	[kInspect](depth: number, options: InspectOptions): unknown {
		return customInspect.call(this, depth, options, this[kType], {
			stream: this[kState].stream,
		});
	}
}

Object.defineProperties(TransformStreamDefaultController.prototype, {
	desiredSize: kEnumerableProperty,
	enqueue: kEnumerableProperty,
	error: kEnumerableProperty,
	terminate: kEnumerableProperty,
	[Symbol.toStringTag]: getNonWritablePropertyDescriptor(TransformStreamDefaultController.name),
});

export const isTransformStream = isBrandCheck("TransformStream") as (value: unknown) => value is TransformStream;
export const isTransformStreamDefaultController = isBrandCheck("TransformStreamDefaultController") as (
	value: unknown,
) => value is TransformStreamDefaultController;

// Raw callback: invoked inside the try/catch of
// transformStreamDefaultControllerPerformTransform.
function defaultTransformAlgorithm(chunk: unknown, controller: TransformStreamDefaultController): void {
	transformStreamDefaultControllerEnqueue(controller, chunk);
}

function initializeTransformStream(
	stream: TransformStream,
	writableHighWaterMark: number,
	writableSizeAlgorithm: (chunk: unknown) => unknown,
	readableHighWaterMark: number,
	readableSizeAlgorithm: (chunk: unknown) => unknown,
): void {
	// The sink and source algorithms are shared functions that reach the
	// transform stream through their controller, and the post-start step
	// is delivered by transformStreamStart.
	const writable = createWritableStream(
		deferredStartAlgorithm,
		transformStreamDefaultSinkWriteAlgorithm as never,
		transformStreamDefaultSinkCloseAlgorithm as never,
		transformStreamDefaultSinkAbortAlgorithm as never,
		writableHighWaterMark,
		writableSizeAlgorithm,
	);
	writableInternals(writable).controller[kState].transformStream = stream;

	const readable = createReadableStream(
		deferredStartAlgorithm,
		transformStreamDefaultSourcePullAlgorithm,
		transformStreamDefaultSourceCancelAlgorithm,
		readableHighWaterMark,
		readableSizeAlgorithm,
	) as ReadableStream;
	readableInternals(readable).controller[kState].transformStream = stream;

	const state = new TransformStreamState();
	state.readable = readable;
	state.writable = writable;
	stream[kState] = state;

	transformStreamSetBackpressure(stream, true);
}

// Delivers the post-start step of both sides. With a non-thenable start
// result the started steps run three microtasks after construction, and
// with a thenable one two microtasks after the adopting promise settles.
function transformStreamStart(stream: TransformStream, startResult: unknown): void {
	const state = stream[kState];
	const started = () => {
		writableStreamDefaultControllerStarted(
			writableInternals(state.writable).controller as unknown as WritableStreamDefaultController,
		);
		readableStreamDefaultControllerStarted(readableInternals(state.readable).controller as never);
	};
	if (startResult === null || (typeof startResult !== "object" && typeof startResult !== "function")) {
		kResolvedPromise.then(undefined, undefined).then(undefined, undefined).then(started);
		return;
	}
	const start = Promise.withResolvers<unknown>();
	start.resolve(startResult);
	start.promise.then(undefined, undefined).then(started, (error: unknown) => {
		writableStreamDefaultControllerStartFailed(
			writableInternals(state.writable).controller as unknown as WritableStreamDefaultController,
			error,
		);
		readableStreamDefaultControllerError(readableInternals(state.readable).controller as never, error);
	});
}

export function transformStreamError(stream: TransformStream, error: unknown): void {
	const { controller } = readableInternals(stream[kState].readable);
	readableStreamDefaultControllerError(controller as never, error);
	transformStreamErrorWritableAndUnblockWrite(stream, error);
}

export function transformStreamErrorWritableAndUnblockWrite(stream: TransformStream, error: unknown): void {
	const { controller, writable } = stream[kState];
	transformStreamDefaultControllerClearAlgorithms(controller!);
	writableStreamDefaultControllerErrorIfNeeded(
		writableInternals(writable).controller as unknown as WritableStreamDefaultController,
		error,
	);
	transformStreamUnblockWrite(stream);
}

export function transformStreamUnblockWrite(stream: TransformStream): void {
	if (stream[kState].backpressure) transformStreamSetBackpressure(stream, false);
}

// The spec's [[backpressureChangePromise]] is only observed internally, so
// it is replaced by continuation slots: a parked pull is completed by
// delivering the readable controller's pull-fulfilled step, and a parked
// write by the cached write continuation. Each is enqueued on the shared
// resolved promise at the exact microtask position the old record's
// reaction would have had.
export function transformStreamSetBackpressure(stream: TransformStream, backpressure: boolean): void {
	const state = stream[kState];
	assert(state.backpressure !== backpressure);
	state.backpressure = backpressure;
	if (backpressure) {
		if (state.pullPending) {
			state.pullPending = false;
			kResolvedPromise.then(readableInternals(state.readable).controller[kState].pullFulfilled);
		}
	} else if (state.pendingWriteParked) {
		kResolvedPromise.then(state.writeContinuation);
	}
}

export function setupTransformStreamDefaultController(
	stream: TransformStream,
	controller: TransformStreamDefaultController,
	transformAlgorithm: TransformAlgorithm,
	flushAlgorithm: PromiseAlgorithm,
	cancelAlgorithm: PromiseAlgorithm,
): void {
	assert(isTransformStream(stream));
	assert(stream[kState].controller === undefined);
	const controllerState = new TransformStreamControllerState();
	controllerState.stream = stream;
	controllerState.transformAlgorithm = transformAlgorithm;
	controllerState.flushAlgorithm = flushAlgorithm;
	controllerState.cancelAlgorithm = cancelAlgorithm;
	controller[kState] = controllerState;
	stream[kState].controller = controller;
}

export function setupTransformStreamDefaultControllerFromTransformer(
	stream: TransformStream,
	transformer: Transformer,
): void {
	const controller = new TransformStreamDefaultController(kSkipThrow);
	const transform = transformer?.transform;
	const flush = transformer?.flush;
	const cancel = transformer?.cancel;
	const transformAlgorithm = (
		transform ? createRawCallback2Params("transformer.transform", transform, transformer) : defaultTransformAlgorithm
	) as TransformAlgorithm;
	const flushAlgorithm = flush ? createPromiseCallback1Param("transformer.flush", flush, transformer) : nonOpFlush;
	const cancelAlgorithm = cancel ? createPromiseCallback1Param("transformer.cancel", cancel, transformer) : nonOpCancel;

	setupTransformStreamDefaultController(stream, controller, transformAlgorithm, flushAlgorithm, cancelAlgorithm);
}

export function transformStreamDefaultControllerClearAlgorithms(controller: TransformStreamDefaultController): void {
	controller[kState].transformAlgorithm = undefined;
	controller[kState].flushAlgorithm = undefined;
	controller[kState].cancelAlgorithm = undefined;
}

export function transformStreamDefaultControllerEnqueue(
	controller: TransformStreamDefaultController,
	chunk: unknown,
): void {
	const stream = controller[kState].stream!;
	const readable = readableInternals(stream[kState].readable);
	const readableController = readable.controller as never;
	if (!readableStreamDefaultControllerCanCloseOrEnqueue(readableController))
		throw new ERR_INVALID_STATE.TypeError("Unable to enqueue");
	try {
		readableStreamDefaultControllerEnqueue(readableController, chunk);
	} catch (error) {
		transformStreamErrorWritableAndUnblockWrite(stream, error);
		throw readable.storedError;
	}
	const backpressure = readableStreamDefaultControllerHasBackpressure(readableController);
	if (backpressure !== stream[kState].backpressure) {
		assert(backpressure);
		transformStreamSetBackpressure(stream, true);
	}
}

export function transformStreamDefaultControllerError(
	controller: TransformStreamDefaultController,
	error: unknown,
): void {
	transformStreamError(controller[kState].stream!, error);
}

// Mirrors `promiseCall(transformAlgorithm, ...).then(undefined, rejectionSteps)`:
// the returned promise settles one microtask after the (coerced) result
// does, and a rejection errors the transform stream before propagating.
export function transformStreamDefaultControllerPerformTransform(
	controller: TransformStreamDefaultController,
	chunk: unknown,
): Promise<unknown> {
	const controllerState = controller[kState];
	const transformAlgorithm = controllerState.transformAlgorithm;
	if (transformAlgorithm === undefined) {
		// Algorithms were cleared by a concurrent cancel/abort/close.
		return kResolvedPromise;
	}
	let result: unknown;
	try {
		result = transformAlgorithm(chunk, controller);
	} catch (error) {
		result = Promise.reject(error);
	}
	let promise: Promise<unknown>;
	if (result === null || (typeof result !== "object" && typeof result !== "function")) {
		promise = kResolvedPromise;
	} else {
		promise = Promise.resolve(result);
	}
	controllerState.performTransformRejected ??= (error: unknown): never => {
		transformStreamError(controller[kState].stream!, error);
		throw error;
	};
	return promise.then(undefined, controllerState.performTransformRejected);
}

export function transformStreamDefaultControllerTerminate(controller: TransformStreamDefaultController): void {
	const stream = controller[kState].stream!;
	const { readable } = stream[kState];
	assert(readable !== undefined);
	const readableController = readableInternals(readable).controller;
	readableStreamDefaultControllerClose(readableController as never);
	transformStreamErrorWritableAndUnblockWrite(
		stream,
		new ERR_INVALID_STATE.TypeError("TransformStream has been terminated"),
	);
}

export function transformStreamDefaultSinkWriteAlgorithm(
	chunk: unknown,
	writableController: WritableControllerInternals,
): unknown {
	const stream = writableController[kState].transformStream!;
	const state = stream[kState];
	const { writable, controller } = state;
	assert(writableInternals(writable).state === "writable");
	if (state.backpressure) {
		// Park the chunk; the backpressure -> false flip delivers the cached
		// continuation (see transformStreamSetBackpressure). The continuation
		// wires the perform-transform promise directly to the writable
		// controller's write reactions. The writable dispatches a single
		// write at a time, so one pending slot suffices.
		assert(!state.pendingWriteParked);
		state.pendingWriteParked = true;
		state.pendingWriteChunk = chunk;
		state.writeContinuation ??= () => {
			const pendingChunk = state.pendingWriteChunk;
			state.pendingWriteParked = false;
			state.pendingWriteChunk = undefined;
			const writableState = writableInternals(state.writable);
			const writableControllerState = writableState.controller[kState];
			if (writableState.state === "erroring") {
				const error = writableState.storedError;
				kResolvedPromise.then(() => writableControllerState.writeRejected!(error));
				return;
			}
			assert(writableState.state === "writable");
			transformStreamDefaultControllerPerformTransform(controller!, pendingChunk).then(
				writableControllerState.writeFulfilled,
				writableControllerState.writeRejected,
			);
		};
		return kParkedAlgorithmResult;
	}
	return transformStreamDefaultControllerPerformTransform(controller!, chunk);
}

export async function transformStreamDefaultSinkAbortAlgorithm(
	reason: unknown,
	writableController: WritableControllerInternals,
): Promise<unknown> {
	const stream = writableController[kState].transformStream!;
	const { controller, readable } = stream[kState];
	const controllerState = controller![kState];
	const readableState = readableInternals(readable);

	if (controllerState.finishPromise !== undefined) {
		return controllerState.finishPromise;
	}

	const { promise, resolve, reject } = Promise.withResolvers<void>();
	controllerState.finishPromise = promise;
	const cancelPromise = controllerState.cancelAlgorithm!(reason);
	transformStreamDefaultControllerClearAlgorithms(controller!);

	cancelPromise.then(
		() => {
			if (readableState.state === "errored") reject(readableState.storedError);
			else {
				readableStreamDefaultControllerError(readableState.controller as never, reason);
				resolve();
			}
		},
		(error: unknown) => {
			readableStreamDefaultControllerError(readableState.controller as never, error);
			reject(error);
		},
	);

	return controllerState.finishPromise;
}

export function transformStreamDefaultSinkCloseAlgorithm(
	writableController: WritableControllerInternals,
): Promise<unknown> {
	const stream = writableController[kState].transformStream!;
	const { readable, controller } = stream[kState];
	const controllerState = controller![kState];
	const readableState = readableInternals(readable);

	if (controllerState.finishPromise !== undefined) {
		return controllerState.finishPromise;
	}
	const { promise, resolve, reject } = Promise.withResolvers<void>();
	controllerState.finishPromise = promise;
	const flushPromise = controllerState.flushAlgorithm!(controller);
	transformStreamDefaultControllerClearAlgorithms(controller!);
	flushPromise.then(
		() => {
			if (readableState.state === "errored") reject(readableState.storedError);
			else {
				readableStreamDefaultControllerClose(readableState.controller as never);
				resolve();
			}
		},
		(error: unknown) => {
			readableStreamDefaultControllerError(readableState.controller as never, error);
			reject(error);
		},
	);
	return controllerState.finishPromise;
}

export function transformStreamDefaultSourcePullAlgorithm(readableController: ReadableControllerInternals): unknown {
	const stream = readableController[kState].transformStream!;
	const state = stream[kState];
	assert(state.backpressure);
	transformStreamSetBackpressure(stream, false);
	// Park the pull: the next backpressure -> true flip delivers the
	// pull-fulfilled step (see transformStreamSetBackpressure).
	state.pullPending = true;
	return kParkedAlgorithmResult;
}

export function transformStreamDefaultSourceCancelAlgorithm(
	reason: unknown,
	readableController: ReadableControllerInternals,
): Promise<unknown> {
	const stream = readableController[kState].transformStream!;
	const { controller, writable } = stream[kState];
	const controllerState = controller![kState];
	const writableState = writableInternals(writable);

	if (controllerState.finishPromise !== undefined) {
		return controllerState.finishPromise;
	}

	const { promise, resolve, reject } = Promise.withResolvers<void>();
	controllerState.finishPromise = promise;
	const cancelPromise = controllerState.cancelAlgorithm!(reason);
	transformStreamDefaultControllerClearAlgorithms(controller!);

	cancelPromise.then(
		() => {
			if (writableState.state === "errored") reject(writableState.storedError);
			else {
				writableStreamDefaultControllerErrorIfNeeded(
					writableState.controller as unknown as WritableStreamDefaultController,
					reason,
				);
				transformStreamUnblockWrite(stream);
				resolve();
			}
		},
		(error: unknown) => {
			writableStreamDefaultControllerErrorIfNeeded(
				writableState.controller as unknown as WritableStreamDefaultController,
				error,
			);
			transformStreamUnblockWrite(stream);
			reject(error);
		},
	);

	return controllerState.finishPromise;
}
