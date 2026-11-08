import { CORE_SCHEMA, loadAll, types } from "internal:parsers/yaml";

// Core schema deliberately excludes YAML 1.1 timestamp coercion. Merge and binary
// are useful optional extensions, without changing plain scalar resolution.
const schema = CORE_SCHEMA.extend({ implicit: [types.merge], explicit: [types.binary] });

type MarkedError = Error & { reason?: string; mark?: { line: number; column: number } };
export function parse(text: string): unknown {
	try {
		const documents = loadAll(text, undefined, { schema, maxDepth: 1000, maxTotalMergeKeys: Infinity }) as unknown[];
		return documents.length > 1 ? documents : documents.length ? documents[0] : null;
	} catch (cause) {
		const error = cause as MarkedError;
		const line = (error.mark?.line ?? 0) + 1;
		const column = (error.mark?.column ?? 0) + 1;
		throw Object.assign(new SyntaxError(`${error.reason ?? error.message} at line ${line}, column ${column}`), {
			line,
			column,
		});
	}
}

function ownKeys(value: object): string[] {
	const result: string[] = [];
	for (const key in value) if (Object.prototype.hasOwnProperty.call(value, key)) result.push(key);
	return result;
}

function quote(value: string): string {
	// YAML's indicators, core-schema scalars, and flow delimiters must not be
	// emitted as ambiguous plain strings (including strings used as map keys).
	const resolved =
		/^(?:null|true|false|~|[-+]?(?:\.(?:inf|nan)|0o[0-7]+|0x[\da-f]+|(?:\d+(?:\.\d*)?|\.\d+)(?:e[-+]?\d+)?))$/i.test(
			value,
		);
	if (
		!value ||
		/^[-?:,\[\]{}#&*!|>'"%@`\s]/u.test(value) ||
		/[\s]$/u.test(value) ||
		/[:\[\]{},#\x00-\x1f\x7f-\x9f\u2028\u2029]/u.test(value) ||
		resolved ||
		value === "---" ||
		value === "..."
	) {
		return JSON.stringify(value).replace(
			/[\x7f-\x9f\u2028\u2029]/gu,
			(c) => `\\u${c.charCodeAt(0).toString(16).padStart(4, "0")}`,
		);
	}
	return value;
}

export function stringify(input: unknown, replacer?: null, space?: number | string): string {
	if (replacer !== undefined && replacer !== null) throw new TypeError("YAML.stringify does not support a replacer");
	const indent =
		typeof space === "number"
			? " ".repeat(Math.max(0, Math.min(10, Math.trunc(space) || 0)))
			: typeof space === "string"
				? space.slice(0, 10)
				: "";
	const counts = new Map<object, number>();
	const anchors = new Map<object, number>();
	const emitted = new Set<object>();
	function children(value: object): unknown[] {
		if (Array.isArray(value)) return value;
		// The keys come from this object's own enumerable properties.
		const record = value as Record<string, unknown>;
		return ownKeys(value).map((key) => record[key]);
	}
	function count(value: unknown): void {
		if (value === null || typeof value !== "object") return;
		const previous = counts.get(value) ?? 0;
		counts.set(value, previous + 1);
		if (previous) return;
		for (const child of children(value)) count(child);
	}
	count(input);
	let nextAnchor = 1;
	for (const [value, count] of counts) if (count > 1) anchors.set(value, nextAnchor++);
	function scalar(value: unknown): string {
		if (value === null || value === undefined) return "null";
		if (typeof value === "string") return quote(value);
		if (typeof value === "number") {
			if (Number.isNaN(value)) return ".nan";
			if (value === Infinity) return ".inf";
			if (value === -Infinity) return "-.inf";
			if (Object.is(value, -0)) return "-0.0";
			return String(value);
		}
		if (typeof value === "boolean" || typeof value === "bigint") return String(value);
		throw new TypeError(`Cannot serialize ${typeof value} to YAML`);
	}
	function render(value: unknown, depth: number): string {
		if (value === null || typeof value !== "object") return scalar(value);
		const anchor = anchors.get(value);
		if (anchor && emitted.has(value)) return `*${anchor}`;
		emitted.add(value);
		const prefix = anchor ? `&${anchor} ` : "";
		const array = Array.isArray(value);
		const keys = array ? [] : ownKeys(value);
		if (array ? !value.length : !keys.length) return prefix + (array ? "[]" : "{}");
		// All indexed properties below are enumerated from this collection.
		const record = value as Record<string, unknown>;
		if (!indent) {
			if (array) {
				const items: string[] = [];
				for (let i = 0; i < value.length; i++) items.push(render(value[i], depth + 1));
				return prefix + `[${items.join(",")}]`;
			}
			return prefix + `{${keys.map((key) => `${quote(key)}: ${render(record[key], depth + 1)}`).join(",")}}`;
		}
		const padding = indent.repeat(depth);
		const lines: string[] = [];
		const entries: [string, unknown][] = [];
		if (array) {
			for (let i = 0; i < value.length; i++) entries.push(["-", value[i]]);
		} else {
			for (const key of keys) entries.push([`${quote(key)}:`, record[key]]);
		}
		for (const [key, child] of entries) {
			const childIsCollection = child !== null && typeof child === "object";
			const isAlias = childIsCollection && anchors.has(child) && emitted.has(child);
			const nonempty = childIsCollection && (Array.isArray(child) ? child.length > 0 : ownKeys(child).length > 0);
			const text = render(child, depth + 1);
			if (nonempty && !isAlias) {
				// Anchors belong on the mapping/sequence header, before its body.
				const match = /^(&\d+)\n/.exec(text);
				if (array && !match) {
					// Use compact sequence children: "- key: value".
					lines.push(`${padding}${key} ${text.slice(indent.repeat(depth + 1).length)}`);
				} else {
					lines.push(`${padding}${key} ${match ? match[1] : ""}\n${match ? text.slice(match[0].length) : text}`);
				}
			} else lines.push(`${padding}${key} ${text}`);
		}
		return (anchor ? `&${anchor}\n` : "") + lines.join("\n");
	}
	return render(input, 0);
}
