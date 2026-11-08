import { parse as parseTOML, stringify as stringifyTOML } from "internal:parsers/toml";

export function parse(text: string): object {
	const value = parseTOML(text, { useLegacyDate: false }) as Record<string, unknown>;
	// Bun returns ordinary own-property objects, including literal __proto__ keys.
	function plain(value: unknown): unknown {
		if (value === null || typeof value !== "object") return value;
		if (Array.isArray(value)) return value.map(plain);
		if (Object.getPrototypeOf(value) !== null) return value;
		const result: Record<string, unknown> = {};
		// Null-prototype parser tables consist exclusively of own string keys.
		const table = value as Record<string, unknown>;
		for (const key in table)
			Object.defineProperty(result, key, {
				value: plain(table[key]),
				writable: true,
				configurable: true,
				enumerable: true,
			});
		return result;
	}
	return plain(value) as object;
}

function temporalTag(value: object): string | undefined {
	if (Symbol.toStringTag in value && typeof value[Symbol.toStringTag] === "string") return value[Symbol.toStringTag];
	return undefined;
}

function unsupported(value: unknown): boolean {
	return value === undefined || typeof value === "function" || typeof value === "symbol";
}

export function stringify(input: unknown, replacer?: null, _space?: string | number): string | undefined {
	if (replacer !== null && replacer !== undefined) throw new TypeError("TOML.stringify does not support a replacer");
	if (unsupported(input)) return undefined;
	if (
		input === null ||
		typeof input !== "object" ||
		Array.isArray(input) ||
		input instanceof Date ||
		temporalTag(input)?.startsWith("Temporal.")
	)
		throw new TypeError("TOML document must be an object");
	const active = new Set<object>();
	function prepare(value: unknown): unknown {
		if (value === null) throw new TypeError("TOML cannot represent null");
		if (typeof value === "bigint") throw new TypeError("TOML.stringify does not support BigInt");
		if (unsupported(value)) throw new TypeError("TOML arrays cannot contain undefined, functions, symbols, or holes");
		if (typeof value !== "object") return value;
		if (value instanceof Date) {
			if (!Number.isFinite(value.getTime()) || value.getUTCFullYear() < 0 || value.getUTCFullYear() > 9999)
				throw new RangeError("TOML date must have a year between 0000 and 9999");
			return value;
		}
		const tag = temporalTag(value);
		if (tag?.startsWith("Temporal.")) {
			if (!/^(?:Temporal\.(?:Instant|PlainDateTime|PlainDate|PlainTime|ZonedDateTime))$/.test(tag))
				throw new TypeError(`TOML cannot represent ${tag}`);
			// The recognized Temporal tag identifies the intrinsic's options-aware method.
			const temporal = value as { toString(options?: object): string };
			const iso = temporal.toString({ calendarName: "never", timeZoneName: "never" });
			if (tag !== "Temporal.PlainTime" && !/^\d{4}-/.test(iso))
				throw new RangeError("TOML date must have a year between 0000 and 9999");
			return value;
		}
		if (active.has(value)) throw new TypeError("Converting circular structure to TOML");
		active.add(value);
		let result: unknown;
		if (Array.isArray(value)) {
			const array: unknown[] = [];
			for (let i = 0; i < value.length; i++) array.push(prepare(value[i]));
			result = array;
		} else {
			const object: Record<string, unknown> = Object.create(null);
			// Only this object's own enumerable string properties form a TOML table.
			const table = value as Record<string, unknown>;
			for (const key in table)
				if (Object.prototype.hasOwnProperty.call(table, key)) {
					const child = table[key];
					if (!unsupported(child)) object[key] = prepare(child);
				}
			result = object;
		}
		active.delete(value);
		return result;
	}
	return stringifyTOML(prepare(input));
}
