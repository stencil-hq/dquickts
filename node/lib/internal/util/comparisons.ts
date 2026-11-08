import types from "node:util/types";
export function compare(
	a: unknown,
	b: unknown,
	strict = true,
	partial = false,
	pairs = new Map<object, object>(),
	skipPrototype = false,
): boolean {
	if (Object.is(a, b) || (typeof a === "number" && typeof b === "number" && Number.isNaN(a) && Number.isNaN(b)))
		return true;
	if (strict && typeof a === "number" && typeof b === "number" && a === 0 && b === 0) return false;
	if (
		a === null ||
		b === null ||
		(typeof a !== "object" && typeof a !== "function") ||
		(typeof b !== "object" && typeof b !== "function")
	)
		return strict ? false : a == b;
	if (typeof a === "function" || typeof b === "function") return false;
	if (strict && !skipPrototype && Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
	if (Object.prototype.toString.call(a) !== Object.prototype.toString.call(b)) return false;
	if (pairs.has(a)) return pairs.get(a) === b;
	for (const [left, right] of pairs) if (right === b && left !== a) return false;
	pairs.set(a, b);
	const eq = (x: unknown, y: unknown): boolean => compare(x, y, strict, partial, new Map(pairs), skipPrototype);
	if (types.isDate(a) && !Object.is((a as Date).getTime(), (b as Date).getTime())) return false;
	if (types.isRegExp(a)) {
		const x = a as RegExp,
			y = b as RegExp;
		if (x.source !== y.source || x.flags !== y.flags || x.lastIndex !== y.lastIndex) return false;
	}
	if (types.isNativeError(a)) {
		const x = a as Error & { cause?: unknown; errors?: unknown },
			y = b as typeof x;
		if (
			x.name !== y.name ||
			x.message !== y.message ||
			((!partial || "cause" in y) && "cause" in x !== "cause" in y) ||
			("cause" in y && !eq(x.cause, y.cause)) ||
			("errors" in y && !eq(x.errors, y.errors))
		)
			return false;
	}
	if (types.isBoxedPrimitive(a) && !eq(a.valueOf(), b.valueOf())) return false;
	if (types.isPromise(a) || types.isWeakMap(a) || types.isWeakSet(a)) return false;
	if (types.isAnyArrayBuffer(a) || ArrayBuffer.isView(a)) {
		const bytes = (v: object): Uint8Array =>
			ArrayBuffer.isView(v) ? new Uint8Array(v.buffer, v.byteOffset, v.byteLength) : new Uint8Array(v as ArrayBuffer);
		const x = bytes(a),
			y = bytes(b);
		if (partial ? x.length < y.length : x.length !== y.length) return false;
		let at = 0;
		for (let i = 0; i < y.length; i++) {
			if (partial) {
				while (at < x.length && x[at] !== y[i]) at++;
				if (at === x.length) return false;
				at++;
			} else if (x[i] !== y[i]) return false;
		}
	}
	if (types.isMap(a) || types.isSet(a)) {
		const x = a as Map<unknown, unknown> | Set<unknown>,
			y = b as typeof x;
		if (partial ? x.size < y.size : x.size !== y.size) return false;
		const remaining = Array.from(x);
		for (const item of y) {
			let found = -1;
			for (let i = 0; i < remaining.length; i++) {
				if (
					types.isMap(a)
						? eq((remaining[i] as unknown[])[0], (item as unknown[])[0]) &&
							eq((remaining[i] as unknown[])[1], (item as unknown[])[1])
						: eq(remaining[i], item)
				) {
					found = i;
					break;
				}
			}
			if (found < 0) return false;
			remaining.splice(found, 1);
		}
	}
	if (
		Array.isArray(a) &&
		((!partial && a.length !== (b as unknown[]).length) || (partial && a.length < (b as unknown[]).length))
	)
		return false;
	if (partial && Array.isArray(a) && Array.isArray(b)) {
		let at = 0;
		for (let i = 0; i < b.length; i++) {
			let found = false;
			while (at < a.length) {
				const current = at++;
				if (Object.hasOwn(a, current) === Object.hasOwn(b, i) && eq(a[current], b[i])) {
					found = true;
					break;
				}
			}
			if (!found) return false;
		}
	}
	const keys = (v: object): PropertyKey[] =>
		Reflect.ownKeys(v).filter(
			(k) =>
				(strict || typeof k !== "symbol") &&
				!(
					partial &&
					(Array.isArray(v) || ArrayBuffer.isView(v)) &&
					typeof k === "string" &&
					/^(0|[1-9]\d*)$/.test(k)
				) &&
				Object.getOwnPropertyDescriptor(v, k)?.enumerable,
		);
	const ak = keys(a),
		bk = keys(b);
	if (!partial && ak.length !== bk.length) return false;
	for (const key of bk) {
		if (
			!Object.prototype.propertyIsEnumerable.call(a, key) ||
			!eq((a as Record<PropertyKey, unknown>)[key], (b as Record<PropertyKey, unknown>)[key])
		)
			return false;
	}
	return true;
}
