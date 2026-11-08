// node:v8. The serialization API follows Node's (Serializer, Deserializer,
// serialize, deserialize) but the format is qbun's, not V8's: a header
// (0xFF, format version), then each writeValue() is a little endian uint32
// byte length followed by the engine's structured clone data (HTML
// structured clone; Buffers keep their prototype, transferArrayBuffer()
// ids refer to out of band ArrayBuffers). writeUint32/writeUint64 are
// LEB128 varints, writeDouble a little endian float64, like V8.
import * as core from "qbun:core";
import * as clone from "qbun:worker";
import { writeFileSync } from "node:fs";
import { invalidArgType } from "internal:errors";

interface CloneNative {
	serialize(value: unknown, transfer: undefined, options: { buffers: boolean; externals?: Map<ArrayBuffer, number> }): Uint8Array;
	deserialize(bytes: Uint8Array, options: { externals?: Map<number, ArrayBuffer | SharedArrayBuffer> }): unknown;
}
const native = clone as unknown as CloneNative;

const kFormatVersion = 1;
export function getHeapStatistics(): object {
	const m = core.memoryUsage();
	return {
		total_heap_size: m.heapTotal,
		total_heap_size_executable: 0,
		total_physical_size: m.heapTotal,
		total_available_size: 0,
		used_heap_size: m.heapUsed,
		heap_size_limit: 0,
		malloced_memory: m.external,
		peak_malloced_memory: m.external,
		does_zap_garbage: 0,
		number_of_native_contexts: 1,
		number_of_detached_contexts: 0,
		total_global_handles_size: 0,
		used_global_handles_size: 0,
		external_memory: m.external,
	};
}
export function getHeapSpaceStatistics(): object[] {
	return [];
}
export function getHeapCodeStatistics(): object {
	return {
		code_and_metadata_size: 0,
		bytecode_and_metadata_size: 0,
		external_script_source_size: 0,
		cpu_profiler_metadata_size: 0,
	};
}
export function setFlagsFromString(_flags: string): void {}
export function cachedDataVersionTag(): number {
	return 0;
}
export function writeHeapSnapshot(filename = `Heap-${Date.now()}.heapsnapshot`): string {
	writeFileSync(
		filename,
		JSON.stringify({
			snapshot: { meta: { node_fields: [], edge_fields: [] }, node_count: 0, edge_count: 0 },
			nodes: [],
			edges: [],
			strings: [],
		}),
	);
	return filename;
}
export function isStringOneByteRepresentation(value: string): boolean {
	for (let i = 0; i < value.length; i++) if (value.charCodeAt(i) > 255) return false;
	return true;
}
function toBytes(name: string, value: unknown): Uint8Array {
	if (!ArrayBuffer.isView(value))
		throw invalidArgType(name, "an instance of Buffer, TypedArray, or DataView", value);
	return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
}

function isDataCloneError(e: unknown): e is DOMException {
	return e instanceof DOMException && e.name === "DataCloneError";
}

export class Serializer {
	#chunks: Uint8Array[] = [];
	#size = 0;
	#externals: Map<ArrayBuffer, number> | undefined;
	#push(bytes: Uint8Array): void {
		this.#chunks.push(bytes);
		this.#size += bytes.length;
	}
	writeHeader(): void {
		this.#push(new Uint8Array([0xff, kFormatVersion]));
	}
	writeValue(value: unknown): boolean {
		let data: Uint8Array;
		try {
			data = native.serialize(value, undefined, { buffers: true, externals: this.#externals });
		} catch (e) {
			if (!isDataCloneError(e)) throw e;
			throw this._getDataCloneError(e.message);
		}
		const length = new Uint8Array(4);
		new DataView(length.buffer).setUint32(0, data.length, true);
		this.#push(length);
		this.#push(data);
		return true;
	}
	releaseBuffer(): Buffer {
		const out = Buffer.allocUnsafe(this.#size);
		let i = 0;
		for (const b of this.#chunks) {
			out.set(b, i);
			i += b.length;
		}
		this.#chunks = [];
		this.#size = 0;
		return out;
	}
	transferArrayBuffer(id: number, arrayBuffer: ArrayBuffer): void {
		if (!(arrayBuffer instanceof ArrayBuffer)) throw invalidArgType("arrayBuffer", "an instance of ArrayBuffer", arrayBuffer);
		(this.#externals ??= new Map()).set(arrayBuffer, id >>> 0);
	}
	writeUint32(value: number): void {
		this.#writeVarint(BigInt(value >>> 0));
	}
	writeUint64(hi: number, lo: number): void {
		this.#writeVarint((BigInt(hi >>> 0) << 32n) | BigInt(lo >>> 0));
	}
	#writeVarint(v: bigint): void {
		const out: number[] = [];
		do {
			let byte = Number(v & 0x7fn);
			v >>= 7n;
			if (v !== 0n) byte |= 0x80;
			out.push(byte);
		} while (v !== 0n);
		this.#push(new Uint8Array(out));
	}
	writeDouble(value: number): void {
		const b = new Uint8Array(8);
		new DataView(b.buffer).setFloat64(0, Number(value), true);
		this.#push(b);
	}
	writeRawBytes(source: unknown): void {
		this.#push(toBytes("source", source).slice());
	}
	_setTreatArrayBufferViewsAsHostObjects(_flag: boolean): void {}
	/** the error thrown for a value that cannot be cloned (overridable, Error by default) */
	_getDataCloneError(message: string): Error {
		return new Error(message);
	}
}

function invalidData(): Error {
	return new Error("Unable to deserialize cloned data.");
}

export class Deserializer {
	#bytes: Uint8Array;
	#offset = 0;
	#version = 0;
	#externals: Map<number, ArrayBuffer | SharedArrayBuffer> | undefined;
	constructor(buffer: unknown) {
		this.#bytes = toBytes("buffer", buffer);
	}
	readHeader(): boolean {
		const b = this.#bytes;
		if (b.length - this.#offset < 2 || b[this.#offset] !== 0xff || b[this.#offset + 1] !== kFormatVersion)
			throw new Error("Unable to deserialize cloned data due to invalid or unsupported version.");
		this.#version = b[this.#offset + 1];
		this.#offset += 2;
		return true;
	}
	getWireFormatVersion(): number {
		return this.#version;
	}
	readValue(): unknown {
		const b = this.#bytes;
		if (b.length - this.#offset < 4) throw invalidData();
		const length = new DataView(b.buffer, b.byteOffset + this.#offset, 4).getUint32(0, true);
		if (b.length - this.#offset - 4 < length) throw invalidData();
		const data = b.subarray(this.#offset + 4, this.#offset + 4 + length);
		this.#offset += 4 + length;
		return native.deserialize(data, { externals: this.#externals });
	}
	transferArrayBuffer(id: number, arrayBuffer: ArrayBuffer | SharedArrayBuffer): void {
		if (!(arrayBuffer instanceof ArrayBuffer) && !(arrayBuffer instanceof SharedArrayBuffer))
			throw invalidArgType("arrayBuffer", "an instance of ArrayBuffer or SharedArrayBuffer", arrayBuffer);
		(this.#externals ??= new Map()).set(id >>> 0, arrayBuffer);
	}
	readRawBytes(length: number): Buffer {
		if (!Number.isInteger(length) || length < 0 || this.#bytes.length - this.#offset < length) throw invalidData();
		const start = this.#bytes.byteOffset + this.#offset;
		this.#offset += length;
		return Buffer.from(this.#bytes.buffer, start, length);
	}
	#readVarint(): bigint {
		let v = 0n;
		let shift = 0n;
		for (;;) {
			if (this.#offset >= this.#bytes.length || shift > 63n) throw invalidData();
			const byte = this.#bytes[this.#offset++];
			v |= BigInt(byte & 0x7f) << shift;
			if (!(byte & 0x80)) return v;
			shift += 7n;
		}
	}
	readUint32(): number {
		return Number(this.#readVarint() & 0xffffffffn);
	}
	readUint64(): [number, number] {
		const v = this.#readVarint();
		return [Number((v >> 32n) & 0xffffffffn), Number(v & 0xffffffffn)];
	}
	readDouble(): number {
		if (this.#bytes.length - this.#offset < 8) throw invalidData();
		const value = new DataView(this.#bytes.buffer, this.#bytes.byteOffset + this.#offset, 8).getFloat64(0, true);
		this.#offset += 8;
		return value;
	}
}
export class DefaultSerializer extends Serializer {}
export class DefaultDeserializer extends Deserializer {}

/** Serialize `value` with a DefaultSerializer (header included). */
export function serialize(value: unknown): Buffer {
	const ser = new DefaultSerializer();
	ser.writeHeader();
	ser.writeValue(value);
	return ser.releaseBuffer();
}

/** Read a value written by serialize(). */
export function deserialize(buffer: unknown): unknown {
	const des = new DefaultDeserializer(buffer);
	des.readHeader();
	return des.readValue();
}
export default {
	serialize,
	deserialize,
	getHeapStatistics,
	getHeapSpaceStatistics,
	getHeapCodeStatistics,
	setFlagsFromString,
	cachedDataVersionTag,
	writeHeapSnapshot,
	isStringOneByteRepresentation,
	Serializer,
	Deserializer,
	DefaultSerializer,
	DefaultDeserializer,
};
