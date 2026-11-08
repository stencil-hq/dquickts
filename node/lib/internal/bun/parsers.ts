// Bun's synchronous configuration/data parsers.
import * as yaml from "internal:parsers/yaml-bun";
import * as toml from "internal:parsers/toml-bun";
import json5 from "internal:parsers/json5";
import { parseJSONC, parseJSONL, parseJSONLChunk } from "internal:parsers/json";
import {
	bytes,
	decode,
	incompleteUTF8,
	offset,
	text,
	utf8Offset,
	type Input,
	type DocumentInput,
} from "internal:parsers/input";

type Replacer = null | undefined;
interface ChunkResult {
	values: unknown[];
	read: number;
	done: boolean;
	error: SyntaxError | null;
}

export const YAML = {
	parse(input: DocumentInput): unknown {
		return yaml.parse(text(input));
	},
	stringify(input: unknown, replacer?: Replacer, space?: string | number): string {
		return yaml.stringify(input, replacer, space);
	},
};
export const TOML = {
	parse(input: DocumentInput): object {
		return toml.parse(text(input));
	},
	stringify(input: unknown, replacer?: Replacer, space?: string | number): string | undefined {
		return toml.stringify(input, replacer, space);
	},
};
export const JSONC = {
	parse(input: string): unknown {
		return parseJSONC(input);
	},
};
export const JSON5 = {
	parse(input: string): unknown {
		return json5.parse(String(input));
	},
	stringify(input: unknown, replacer?: Replacer, space?: string | number): string | undefined {
		if (replacer !== undefined && replacer !== null) throw new TypeError("JSON5.stringify does not support a replacer");
		return json5.stringify(input, { quote: '"', space });
	},
};
export const JSONL = {
	parse(input: Input): unknown[] {
		return parseJSONL(text(input));
	},
	parseChunk(input: Input, start?: number, end?: number): ChunkResult {
		const source = typeof input === "string" ? input : bytes(input);
		const from = offset(start, 0, source.length, "start");
		const to = offset(end, source.length, source.length, "end");
		if (to < from) throw new RangeError("end must not be less than start");
		if (typeof source === "string") {
			const result = parseJSONLChunk(source.slice(from, to));
			result.read += from;
			return result;
		}
		let bom = 0;
		if (from === 0 && to >= 3 && source[0] === 0xef && source[1] === 0xbb && source[2] === 0xbf) bom = 3;
		const chunk = source.subarray(from + bom, to);
		const pending = incompleteUTF8(chunk);
		const decoded = decode(pending ? chunk.subarray(0, chunk.length - pending) : chunk, false);
		const result = parseJSONLChunk(decoded);
		result.read = from + bom + utf8Offset(chunk, result.read);
		if (pending) result.done = false;
		return result;
	},
};
