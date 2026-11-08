// Port of Node's lib/internal/streams/duplexpair.js.
import { Duplex } from "internal:streams/duplex";

type Cb = (err?: unknown) => void;

const kCallback = Symbol("Callback");
const kInitOtherSide = Symbol("InitOtherSide");

function assert(value: unknown): void {
	if (!value) {
		const err = new Error("Assertion failed");
		Object.defineProperty(err, "code", {
			value: "ERR_ASSERTION",
			enumerable: true,
			configurable: true,
			writable: true,
		});
		throw err;
	}
}

class DuplexSide extends Duplex {
	#otherSide: DuplexSide | null = null;
	[kCallback]: (() => void) | null;

	constructor(options?: object) {
		super(options);
		this[kCallback] = null;
		this.#otherSide = null;
	}

	[kInitOtherSide](otherSide: DuplexSide): void {
		// Ensure this can only be set once, to enforce encapsulation.
		if (this.#otherSide === null) {
			this.#otherSide = otherSide;
		} else {
			assert(this.#otherSide === null);
		}
	}

	_read(): void {
		const callback = this[kCallback];
		if (callback) {
			this[kCallback] = null;
			callback();
		}
	}

	_write(chunk: { length: number }, _encoding: string, callback: () => void): void {
		assert(this.#otherSide !== null);
		const otherSide = this.#otherSide!;
		assert(otherSide[kCallback] === null);
		if (chunk.length === 0) {
			process.nextTick(callback);
		} else {
			otherSide.push(chunk);
			otherSide[kCallback] = callback;
		}
	}

	_final(callback: Cb): void {
		const otherSide = this.#otherSide!;
		otherSide.on("end", callback);
		otherSide.push(null);
	}

	_destroy(err: Error | null, callback: Cb): void {
		const otherSide = this.#otherSide;
		if (otherSide !== null && !otherSide.destroyed) {
			// nextTick avoids crashing the current execution stack (like the HTTP parser).
			process.nextTick(() => {
				if (otherSide.destroyed) return;
				if (err) {
					// Close the other side without the error: it must not hang, nor crash with "Unhandled error".
					otherSide.destroy();
				} else {
					otherSide.push(null);
				}
			});
		}
		callback(err);
	}
}

export function duplexPair(options?: object): [DuplexSide, DuplexSide] {
	const side0 = new DuplexSide(options);
	const side1 = new DuplexSide(options);
	side0[kInitOtherSide](side1);
	side1[kInitOtherSide](side0);
	return [side0, side1];
}
