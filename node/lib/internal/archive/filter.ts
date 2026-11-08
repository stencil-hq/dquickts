import { compileGlob } from "internal:fs/glob";
import { invalidArgType } from "internal:errors";

interface Matcher {
	match(path: string): boolean;
}

export function archiveFilter(patterns: string | readonly string[] | undefined): (path: string) => boolean {
	if (patterns === undefined) return () => true;
	const list = typeof patterns === "string" ? [patterns] : patterns;
	if (!Array.isArray(list) || !list.every((pattern) => typeof pattern === "string")) {
		throw invalidArgType("glob", "a string or an array of strings", patterns);
	}
	if (list.length === 0) return () => false;
	const positive: Matcher[] = [];
	const negative: Matcher[] = [];
	for (const pattern of list) {
		if (pattern.startsWith("!") && pattern[1] !== "(") negative.push(compileGlob(pattern.slice(1), { dot: true }));
		else positive.push(compileGlob(pattern, { dot: true }));
	}
	return (path) =>
		(positive.length === 0 || positive.some((matcher) => matcher.match(path))) &&
		!negative.some((matcher) => matcher.match(path));
}
