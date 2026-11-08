// Port of Node's lib/internal/webstreams/util.js (transfer helpers dropped)
// plus the WebIDL helpers the web stream modules need (lib/internal/webidl.js).

import * as core from "qbun:core";
import { inspect } from "node:util";
import { ERR_INVALID_ARG_VALUE, isDataView, kEmptyObject, validateFunction } from "internal:streams/shared";

export const kState: unique symbol = Symbol("kState") as never;
export const kType: unique symbol = Symbol("kType") as never;

type Fn = (...args: unknown[]) => unknown;

function assert(value: unknown, message?: string): void {
	if (!value) {
		const err = new Error(
			message ??
				"This is caused by either a bug in Node.js or incorrect usage of Node.js internals.\nPlease open an issue with this stack trace at https://github.com/nodejs/node/issues\n",
		) as Error & { code: string };
		err.code = "ERR_INTERNAL_ASSERTION";
		throw err;
	}
}

export const getNonWritablePropertyDescriptor = (value: unknown): PropertyDescriptor => {
	return {
		__proto__: null,
		configurable: true,
		value,
	} as PropertyDescriptor;
};

export function extractHighWaterMark(value: unknown, defaultHWM: number): number {
	if (value === undefined) return defaultHWM;
	const coercedValue = +(value as number);
	if (Number.isNaN(coercedValue) || coercedValue < 0)
		throw new ERR_INVALID_ARG_VALUE.RangeError("strategy.highWaterMark", value);
	return coercedValue;
}

// The default size algorithm is never exposed to user code, so a single
// shared function avoids one closure allocation per stream.
export const defaultSizeAlgorithm = (): number => 1;

export function extractSizeAlgorithm(size: unknown): Fn {
	if (size === undefined) return defaultSizeAlgorithm;
	validateFunction(size, "strategy.size");
	return size;
}

type InspectOptions = { depth?: number | null; [key: string]: unknown };

export function customInspect(
	this: unknown,
	depth: number,
	options: InspectOptions,
	name: string,
	data: unknown,
): unknown {
	if (depth < 0) return this;

	const opts = {
		...options,
		depth: options.depth == null ? null : options.depth - 1,
	};

	return `${name} ${inspect(data, opts)}`;
}

// Use the intrinsic getters so user tampering with own/prototype accessors is not observed.
const typedArrayProto = Object.getPrototypeOf(Uint8Array.prototype) as object;
const getTypedArrayBuffer = Object.getOwnPropertyDescriptor(typedArrayProto, "buffer")!.get!;
const getTypedArrayByteLength = Object.getOwnPropertyDescriptor(typedArrayProto, "byteLength")!.get!;
const getTypedArrayByteOffset = Object.getOwnPropertyDescriptor(typedArrayProto, "byteOffset")!.get!;
const getDataViewBuffer = Object.getOwnPropertyDescriptor(DataView.prototype, "buffer")!.get!;
const getDataViewByteLength = Object.getOwnPropertyDescriptor(DataView.prototype, "byteLength")!.get!;
const getDataViewByteOffset = Object.getOwnPropertyDescriptor(DataView.prototype, "byteOffset")!.get!;
const getArrayBufferByteLength = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "byteLength")!.get!;
const getArrayBufferDetachedDesc = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "detached");
const getArrayBufferDetached = getArrayBufferDetachedDesc?.get;

export function ArrayBufferViewGetBuffer(view: ArrayBufferView): ArrayBuffer {
	return (isDataView(view) ? getDataViewBuffer.call(view) : getTypedArrayBuffer.call(view)) as ArrayBuffer;
}
export function ArrayBufferViewGetByteLength(view: ArrayBufferView): number {
	return (isDataView(view) ? getDataViewByteLength.call(view) : getTypedArrayByteLength.call(view)) as number;
}
export function ArrayBufferViewGetByteOffset(view: ArrayBufferView): number {
	return (isDataView(view) ? getDataViewByteOffset.call(view) : getTypedArrayByteOffset.call(view)) as number;
}

export function isArrayBufferDetached(buffer: ArrayBuffer): boolean {
	if (getArrayBufferDetached !== undefined) return getArrayBufferDetached.call(buffer) as boolean;
	return (buffer as ArrayBuffer & { detached?: boolean }).detached === true;
}

export function cloneAsUint8Array(view: ArrayBufferView): Uint8Array {
	const buffer = ArrayBufferViewGetBuffer(view);
	const byteOffset = ArrayBufferViewGetByteOffset(view);
	const byteLength = ArrayBufferViewGetByteLength(view);
	return new Uint8Array(buffer.slice(byteOffset, byteOffset + byteLength));
}

export function canCopyArrayBuffer(
	toBuffer: ArrayBuffer,
	toIndex: number,
	fromBuffer: ArrayBuffer,
	fromIndex: number,
	count: number,
): boolean {
	return (
		toBuffer !== fromBuffer &&
		!isArrayBufferDetached(toBuffer) &&
		!isArrayBufferDetached(fromBuffer) &&
		toIndex + count <= (getArrayBufferByteLength.call(toBuffer) as number) &&
		fromIndex + count <= (getArrayBufferByteLength.call(fromBuffer) as number)
	);
}

export function copyArrayBuffer(
	destination: ArrayBuffer,
	destinationOffset: number,
	source: ArrayBuffer,
	sourceOffset: number,
	bytesToCopy: number,
): void {
	new Uint8Array(destination, destinationOffset, bytesToCopy).set(new Uint8Array(source, sourceOffset, bytesToCopy));
}

/** Detach `buffer`, returning a new ArrayBuffer owning its contents. */
export function transferArrayBuffer(buffer: ArrayBuffer): ArrayBuffer {
	return (buffer as ArrayBuffer & { transfer(): ArrayBuffer }).transfer();
}

export function isBrandCheck(brand: string): (value: unknown) => boolean {
	return (value: unknown): boolean => {
		return (
			value != null &&
			(value as Record<symbol, unknown>)[kState] !== undefined &&
			(value as Record<symbol, unknown>)[kType] === brand
		);
	};
}

// Backing store for the spec's [[queue]]: a power-of-two ring buffer.
// Default controller queues store (value, size) pairs in two consecutive
// slots via the *Pair methods; the byte controller queue stores single
// records via push/shift/peek.
export class Queue {
	head: number;
	tail: number;
	length: number;
	capacityMask: number;
	list: unknown[];
	dequeuedSize: number;

	constructor(listLength = 8) {
		this.head = 0;
		this.tail = 0;
		this.length = 0;
		this.capacityMask = listLength - 1;
		this.list = new Array(listLength);
		this.dequeuedSize = 0;
	}

	push(entry: unknown): void {
		if (this.length === this.list.length) this.grow();
		const tail = this.tail;
		this.list[tail] = entry;
		this.tail = (tail + 1) & this.capacityMask;
		this.length++;
	}

	shift(): unknown {
		const head = this.head;
		const list = this.list;
		const entry = list[head];
		list[head] = undefined;
		this.head = (head + 1) & this.capacityMask;
		if (--this.length === 0) this.rewind();
		return entry;
	}

	peek(): unknown {
		return this.list[this.head];
	}

	pushPair(value: unknown, size: number): void {
		if (this.length * 2 === this.list.length) this.grow();
		const tail = this.tail;
		const list = this.list;
		list[tail] = value;
		list[tail + 1] = size;
		this.tail = (tail + 2) & this.capacityMask;
		this.length++;
	}

	shiftPair(): unknown {
		const head = this.head;
		const list = this.list;
		const value = list[head];
		this.dequeuedSize = list[head + 1] as number;
		list[head] = undefined;
		list[head + 1] = undefined;
		this.head = (head + 2) & this.capacityMask;
		if (--this.length === 0) this.rewind();
		return value;
	}

	peekPairValue(): unknown {
		return this.list[this.head];
	}

	grow(): void {
		const list = this.list;
		const capacity = list.length;
		const head = this.head;
		if (head !== 0) {
			const relinearized = new Array(capacity * 2);
			let n = 0;
			for (let i = head; i < capacity; i++) relinearized[n++] = list[i];
			for (let i = 0; i < head; i++) relinearized[n++] = list[i];
			this.list = relinearized;
			this.head = 0;
		} else {
			list.length = capacity * 2;
		}
		this.tail = capacity;
		this.capacityMask = capacity * 2 - 1;
	}

	rewind(): void {
		this.head = 0;
		this.tail = 0;
		if (this.list.length > 1024) {
			this.list.length = 8;
			this.capacityMask = 0b111;
		}
	}
}

// Shared immutable empty queue; a real Queue is materialized on first enqueue.
export const kEmptyQueue: Queue = Object.freeze(new Queue(0)) as Queue;

type QueueState = { queue: Queue; queueTotalSize: number };
type QueueHolder = { [kState]: QueueState };

export function materializeQueue(state: QueueState): Queue {
	const queue = state.queue;
	if (queue === kEmptyQueue) return (state.queue = new Queue());
	return queue;
}

export function dequeueValue(controller: unknown): unknown {
	const state = (controller as QueueHolder)[kState];
	const queue = state.queue;
	assert(queue.length);
	const value = queue.shiftPair();
	state.queueTotalSize = Math.max(0, state.queueTotalSize - queue.dequeuedSize);
	return value;
}

export function resetQueue(controller: unknown): void {
	const state = (controller as QueueHolder)[kState];
	state.queue = kEmptyQueue;
	state.queueTotalSize = 0;
}

export function peekQueueValue(controller: unknown): unknown {
	const state = (controller as QueueHolder)[kState];
	assert(state.queue.length);
	return state.queue.peekPairValue();
}

export function enqueueValueWithSize(controller: unknown, value: unknown, size: unknown): void {
	const state = (controller as QueueHolder)[kState];
	const coercedSize = +(size as number);
	if (Number.isNaN(coercedSize) || coercedSize < 0 || coercedSize === Infinity) {
		throw new ERR_INVALID_ARG_VALUE.RangeError("size", size);
	}
	materializeQueue(state).pushPair(value, coercedSize);
	state.queueTotalSize += coercedSize;
}

export function createPromiseCallbackNoParams(name: string, fn: unknown, thisArg: unknown): () => Promise<unknown> {
	validateFunction(fn, name);
	return async () => fn.call(thisArg);
}

// Raw variants that skip the async wrapper's implicit result promise.
export function createRawCallback1Param(name: string, fn: unknown, thisArg: unknown): (arg: unknown) => unknown {
	validateFunction(fn, name);
	return (arg: unknown) => fn.call(thisArg, arg);
}

export function createRawCallback2Params(
	name: string,
	fn: unknown,
	thisArg: unknown,
): (arg1: unknown, arg2: unknown) => unknown {
	validateFunction(fn, name);
	return (arg1: unknown, arg2: unknown) => fn.call(thisArg, arg1, arg2);
}

// A single shared, forever-resolved promise.
export const kResolvedPromise: Promise<void> = Promise.resolve();

// Returned by an internal algorithm to signal that it parked the operation.
export const kParkedAlgorithmResult: unique symbol = Symbol("kParkedAlgorithmResult") as never;

// Start algorithm of internal streams whose creator delivers the post-start step itself.
export const kDeferredStart: unique symbol = Symbol("kDeferredStart") as never;

export function deferredStartAlgorithm(): typeof kDeferredStart {
	return kDeferredStart;
}

export function thenAlgorithmResult(
	result: unknown,
	onFulfilled: (value: unknown) => unknown,
	onRejected?: (reason: unknown) => unknown,
): void {
	if (result === kParkedAlgorithmResult) return;
	if (result === null || (typeof result !== "object" && typeof result !== "function")) {
		kResolvedPromise.then(onFulfilled);
	} else {
		Promise.resolve(result).then(onFulfilled, onRejected);
	}
}

export function createPromiseCallback1Param(
	name: string,
	fn: unknown,
	thisArg: unknown,
): (arg: unknown) => Promise<unknown> {
	validateFunction(fn, name);
	return async (arg: unknown) => fn.call(thisArg, arg);
}

export function createPromiseCallback2Params(
	name: string,
	fn: unknown,
	thisArg: unknown,
): (arg1: unknown, arg2: unknown) => Promise<unknown> {
	validateFunction(fn, name);
	return async (arg1: unknown, arg2: unknown) => fn.call(thisArg, arg1, arg2);
}

export function isPromisePending(promise: Promise<unknown> | undefined): boolean {
	if (promise === undefined) return false;
	const details = core.promiseState(promise) as [number, unknown] | undefined;
	return details?.[0] === 0;
}

export type PromiseRecord<T = unknown> = {
	promise: Promise<T>;
	resolve: ((value: T) => void) | undefined;
	reject: ((reason: unknown) => void) | undefined;
};

export function resolvedRecord(): PromiseRecord {
	return {
		promise: Promise.resolve(),
		resolve: undefined,
		reject: undefined,
	};
}

export function rejectedHandledRecord(error: unknown): PromiseRecord {
	const record: PromiseRecord = {
		promise: Promise.reject(error),
		resolve: undefined,
		reject: undefined,
	};
	setPromiseHandled(record.promise);
	return record;
}

// A single shared, forever-pending promise.
export const kPendingPromise: Promise<never> = Promise.withResolvers<never>().promise;

export function setPromiseHandled(promise: Promise<unknown>): void {
	if (promise === kPendingPromise) return;
	promise.then(undefined, () => {});
}

export async function nonOpFlush(): Promise<void> {}

// Shared non-op for the start/pull/write raw algorithm callbacks.
export function nonOpCallback(): void {}

export async function nonOpCancel(): Promise<void> {}

// ---------------------------------------------------------------- webidl

type WebIDLOptions = { prefix?: string; context?: string; code?: string };

export function makeException(message: string, options: WebIDLOptions = kEmptyObject): TypeError {
	const prefix = options.prefix ? options.prefix + ": " : "";
	const context = options.context?.length === 0 ? "" : (options.context ?? "Value") + " ";
	const err = new TypeError(`${prefix}${context}${message}`) as TypeError & { code: string };
	err.code = options.code || "ERR_INVALID_ARG_TYPE";
	return err;
}

function webidlToString(V: unknown, options: WebIDLOptions): string {
	if (typeof V === "symbol") throw makeException("is a Symbol and cannot be converted to a string.", options);
	return String(V);
}

export function createEnumConverter(
	name: string,
	values: readonly string[],
): (V: unknown, options?: WebIDLOptions) => string {
	const E = new Set(values);
	return function (V: unknown, options: WebIDLOptions = kEmptyObject): string {
		const S = webidlToString(V, options);
		if (!E.has(S)) {
			throw makeException(`'${S}' is not a valid enum value of type ${name}.`, {
				prefix: options.prefix,
				context: options.context,
				code: "ERR_INVALID_ARG_VALUE",
			});
		}
		return S;
	};
}
