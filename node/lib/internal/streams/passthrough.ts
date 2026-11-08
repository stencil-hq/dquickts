// PassThrough: a Transform that passes input bytes across to the output
// (Node's lib/internal/streams/passthrough.js). MIT license.

import { Transform, type TransformImpl, type TransformOptions } from "internal:streams/transform";

export interface PassThroughConstructor {
	new (options?: TransformOptions): TransformImpl;
	(this: unknown, options?: TransformOptions): void;
	prototype: TransformImpl;
	[key: string]: unknown;
}

export const PassThrough = function PassThrough(this: TransformImpl, options?: TransformOptions): TransformImpl | void {
	if (!(this instanceof PassThrough)) return new PassThrough(options);
	Transform.call(this, options);
} as unknown as PassThroughConstructor;

Object.setPrototypeOf(PassThrough.prototype, Transform.prototype);
Object.setPrototypeOf(PassThrough, Transform);

(PassThrough.prototype as unknown as Record<string, unknown>)._transform = function (
	chunk: unknown,
	_encoding: string,
	cb: (err: null, data: unknown) => void,
): void {
	cb(null, chunk);
};
