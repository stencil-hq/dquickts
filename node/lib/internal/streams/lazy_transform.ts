// Port of Node's lib/internal/streams/lazy_transform.js: a Transform whose state is created on first access
// of `_readableState` / `_writableState`.
import { Transform } from "internal:streams/transform";

interface LazyTransformThis {
	_options: object | undefined;
	_writableState: { decodeStrings: boolean };
	[key: string]: unknown;
}

export function LazyTransform(this: LazyTransformThis, options?: object): void {
	this._options = options;
}
Object.setPrototypeOf(LazyTransform.prototype, Transform.prototype);
Object.setPrototypeOf(LazyTransform, Transform);

function makeGetter(name: string) {
	return function (this: LazyTransformThis): unknown {
		Transform.call(this, this._options);
		this._writableState.decodeStrings = false;
		return this[name];
	};
}

function makeSetter(name: string) {
	return function (this: LazyTransformThis, val: unknown): void {
		Object.defineProperty(this, name, {
			value: val,
			enumerable: true,
			configurable: true,
			writable: true,
		});
	};
}

Object.defineProperties(LazyTransform.prototype, {
	_readableState: {
		get: makeGetter("_readableState"),
		set: makeSetter("_readableState"),
		configurable: true,
		enumerable: true,
	},
	_writableState: {
		get: makeGetter("_writableState"),
		set: makeSetter("_writableState"),
		configurable: true,
		enumerable: true,
	},
});
