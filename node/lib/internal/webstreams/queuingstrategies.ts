// Port of Node's lib/internal/webstreams/queuingstrategies.js.

import {
	ERR_MISSING_OPTION,
	customInspectSymbol as kInspect,
	kEnumerableProperty,
	validateObject,
} from "internal:streams/shared";
import { customInspect, getNonWritablePropertyDescriptor } from "internal:webstreams/util";

type QueuingStrategySize = (chunk: unknown) => number;
type StrategyInit = { highWaterMark?: unknown };

const nameDescriptor = { __proto__: null, value: "size" } as PropertyDescriptor;
const byteSizeFunction: QueuingStrategySize = Object.defineProperty(
	(chunk: unknown) => (chunk as { byteLength: number }).byteLength,
	"name",
	nameDescriptor,
);
const countSizeFunction: QueuingStrategySize = Object.defineProperty(() => 1, "name", nameDescriptor);

export class ByteLengthQueuingStrategy {
	#state: { highWaterMark: number };
	#byteSizeFunction = byteSizeFunction;

	constructor(init: StrategyInit) {
		validateObject(init, "init");
		if (init.highWaterMark === undefined) throw new ERR_MISSING_OPTION("init.highWaterMark");

		// The highWaterMark value is not checked until the strategy
		// is actually used, per the spec.
		this.#state = {
			highWaterMark: +(init.highWaterMark as number),
		};
	}

	get highWaterMark(): number {
		return this.#state.highWaterMark;
	}

	get size(): QueuingStrategySize {
		return this.#byteSizeFunction;
	}

	[kInspect](depth: number, options: { depth?: number | null }): unknown {
		return customInspect.call(this, depth, options, "ByteLengthQueuingStrategy", {
			highWaterMark: this.highWaterMark,
		});
	}
}

Object.defineProperties(ByteLengthQueuingStrategy.prototype, {
	highWaterMark: kEnumerableProperty,
	size: kEnumerableProperty,
	[Symbol.toStringTag]: getNonWritablePropertyDescriptor(ByteLengthQueuingStrategy.name),
});

export class CountQueuingStrategy {
	#state: { highWaterMark: number };
	#countSizeFunction = countSizeFunction;

	constructor(init: StrategyInit) {
		validateObject(init, "init");
		if (init.highWaterMark === undefined) throw new ERR_MISSING_OPTION("init.highWaterMark");

		// The highWaterMark value is not checked until the strategy
		// is actually used, per the spec.
		this.#state = {
			highWaterMark: +(init.highWaterMark as number),
		};
	}

	get highWaterMark(): number {
		return this.#state.highWaterMark;
	}

	get size(): QueuingStrategySize {
		return this.#countSizeFunction;
	}

	[kInspect](depth: number, options: { depth?: number | null }): unknown {
		return customInspect.call(this, depth, options, "CountQueuingStrategy", {
			highWaterMark: this.highWaterMark,
		});
	}
}

Object.defineProperties(CountQueuingStrategy.prototype, {
	highWaterMark: kEnumerableProperty,
	size: kEnumerableProperty,
	[Symbol.toStringTag]: getNonWritablePropertyDescriptor(CountQueuingStrategy.name),
});
