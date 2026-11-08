import * as core from "qbun:core";
const tag = (v: unknown): string => Object.prototype.toString.call(v);
const tagged =
	(name: string) =>
	(v: unknown): boolean =>
		v !== null && (typeof v === "object" || typeof v === "function") && tag(v) === `[object ${name}]`;
export const isArrayBuffer = tagged("ArrayBuffer");
export const isSharedArrayBuffer = tagged("SharedArrayBuffer");
export const isAnyArrayBuffer = (v: unknown): boolean => isArrayBuffer(v) || isSharedArrayBuffer(v);
export const isArrayBufferView = ArrayBuffer.isView;
export const isAsyncFunction = tagged("AsyncFunction");
export const isGeneratorFunction = (v: unknown): boolean => /\[object (Async)?GeneratorFunction\]/.test(tag(v));
export const isGeneratorObject = (v: unknown): boolean => /\[object (Async)?Generator\]/.test(tag(v));
export const isPromise = (v: unknown): boolean => core.promiseState(v) !== undefined;
export const isProxy = (v: unknown): boolean => core.proxyTarget(v) !== undefined;
export const isRegExp = tagged("RegExp");
export const isDate = tagged("Date");
export const isNativeError = (v: unknown): boolean => v instanceof Error || tag(v) === "[object Error]";
export const isBooleanObject = tagged("Boolean");
export const isNumberObject = tagged("Number");
export const isStringObject = tagged("String");
export const isSymbolObject = tagged("Symbol");
export const isBigIntObject = tagged("BigInt");
export const isBoxedPrimitive = (v: unknown): boolean =>
	typeof v === "object" && v !== null && /\[object (Boolean|Number|String|Symbol|BigInt)\]/.test(tag(v));
export const isMap = tagged("Map");
export const isSet = tagged("Set");
export const isWeakMap = tagged("WeakMap");
export const isWeakSet = tagged("WeakSet");
export const isMapIterator = tagged("Map Iterator");
export const isSetIterator = tagged("Set Iterator");
export const isTypedArray = (v: unknown): boolean => ArrayBuffer.isView(v) && !isDataView(v);
export const isUint8Array = tagged("Uint8Array");
export const isUint8ClampedArray = tagged("Uint8ClampedArray");
export const isUint16Array = tagged("Uint16Array");
export const isUint32Array = tagged("Uint32Array");
export const isInt8Array = tagged("Int8Array");
export const isInt16Array = tagged("Int16Array");
export const isInt32Array = tagged("Int32Array");
export const isFloat16Array = tagged("Float16Array");
export const isFloat32Array = tagged("Float32Array");
export const isFloat64Array = tagged("Float64Array");
export const isBigInt64Array = tagged("BigInt64Array");
export const isBigUint64Array = tagged("BigUint64Array");
export const isDataView = tagged("DataView");
export const isExternal = (_v: unknown): boolean => false;
export const isModuleNamespaceObject = (v: unknown): boolean =>
	v !== null && typeof v === "object" && Object.getPrototypeOf(v) === null && tag(v) === "[object Module]";
export const isKeyObject = (v: unknown): boolean =>
	v !== null && typeof v === "object" && /^(Secret|Public|Private)?KeyObject$/.test(v.constructor?.name ?? "");
export const isCryptoKey = (v: unknown): boolean =>
	v !== null && typeof v === "object" && v.constructor?.name === "CryptoKey";
export default {
	isArrayBuffer,
	isSharedArrayBuffer,
	isAnyArrayBuffer,
	isArrayBufferView,
	isAsyncFunction,
	isGeneratorFunction,
	isGeneratorObject,
	isPromise,
	isProxy,
	isRegExp,
	isDate,
	isNativeError,
	isBooleanObject,
	isNumberObject,
	isStringObject,
	isSymbolObject,
	isBigIntObject,
	isBoxedPrimitive,
	isMap,
	isSet,
	isWeakMap,
	isWeakSet,
	isMapIterator,
	isSetIterator,
	isTypedArray,
	isUint8Array,
	isUint8ClampedArray,
	isUint16Array,
	isUint32Array,
	isInt8Array,
	isInt16Array,
	isInt32Array,
	isFloat16Array,
	isFloat32Array,
	isFloat64Array,
	isBigInt64Array,
	isBigUint64Array,
	isDataView,
	isExternal,
	isModuleNamespaceObject,
	isKeyObject,
	isCryptoKey,
};
