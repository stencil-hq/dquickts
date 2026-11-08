// Match npm's complete comparator/range grammar.
import { satisfies, compare } from "internal:parsers/semver";

export const semver = {
	satisfies(version: unknown, range: unknown): boolean {
		return satisfies(String(version), String(range));
	},
	order(a: unknown, b: unknown): -1 | 0 | 1 {
		return compare(String(a), String(b));
	},
};
