// internal:fs/glob — the glob engine shared by Bun.Glob and node:fs glob.
//
// Syntax (Bun): `*` (any run of non-`/` characters), `**` (as a whole path
// segment: zero or more segments), `?`, `[ab]`, `[a-z]`, `[!a]`/`[^a]`,
// `{a,b}` (nestable, may span segments), `{1..3}`/`{01..10}`/`{1..9..2}`/
// `{a..e}`, extglobs `?(a|b)` `*(a|b)` `+(a|b)` `@(a|b)` `!(a|b)`, a leading
// `!` negating the whole pattern, and `\` escaping the next character.
//
// Patterns are brace-expanded, split into `/` segments and every segment is
// compiled to a literal, a `**` marker or an anchored RegExp. Matching runs
// segment by segment; the walker drives the same segments as an NFA over the
// directory tree, so it only reads directories where a match is still possible.

import * as fs from "node:fs";
import { posix as pathPosix } from "node:path";

export interface GlobMatcher {
	match(path: string): boolean;
}

export interface CompileOptions {
	/** Let wildcards and `**` match segments starting with `.` (default false). */
	dot?: boolean;
	nocase?: boolean;
}

export interface WalkOptions {
	cwd: string;
	dot?: boolean;
	absolute?: boolean;
	followSymlinks?: boolean;
	onlyFiles?: boolean;
	throwErrorOnBrokenSymlink?: boolean;
	/** Return true to drop an entry (and, for a directory, its whole subtree). */
	exclude?: (relPath: string, isDir: boolean) => boolean;
}

// ---------------------------------------------------------------------------
// Pattern parsing

interface Segment {
	/** `**` */
	star: boolean;
	/** Non-null when the segment has no magic (escapes already resolved). */
	literal: string | null;
	re: RegExp | null;
}

type Token = { t: "lit"; s: string } | { t: "src"; s: string } | { t: "ext"; kind: string; alts: Token[][] };

const RE_SPECIAL = /[.*+?^${}()|[\]\\/]/g;
const CLASS_SPECIAL = /[\\\]\[^-]/g;

function escapeClass(s: string): string {
	return s.replace(CLASS_SPECIAL, "\\$&");
}

/** Index of the `)` closing the `(` at `open`, or -1. */
function findParen(src: string, open: number, end: number): number {
	let depth = 0;
	for (let i = open; i < end; i++) {
		const c = src[i];
		if (c === "\\") i++;
		else if (c === "(") depth++;
		else if (c === ")" && --depth === 0) return i;
	}
	return -1;
}

/** Split src[a, b) on top-level `|` (outside parens, respecting escapes). */
function splitAlternatives(src: string, a: number, b: number): [number, number][] {
	const out: [number, number][] = [];
	let depth = 0;
	let start = a;
	for (let i = a; i < b; i++) {
		const c = src[i];
		if (c === "\\") i++;
		else if (c === "(") depth++;
		else if (c === ")") depth--;
		else if (c === "|" && depth === 0) {
			out.push([start, i]);
			start = i + 1;
		}
	}
	out.push([start, b]);
	return out;
}

/** Parse the bracket expression starting at src[i] === "[". */
function parseClass(src: string, i: number, end: number): { src: string; next: number } | null {
	let j = i + 1;
	let negated = false;
	if (src[j] === "!" || src[j] === "^") {
		negated = true;
		j++;
	}
	let body = "";
	let first = true;
	while (j < end) {
		const c = src[j];
		if (c === "]" && !first) {
			return { src: negated ? `[^/${body}]` : body === "" ? "(?!)" : `[${body}]`, next: j + 1 };
		}
		first = false;
		let lo: string;
		if (c === "\\" && j + 1 < end) {
			lo = String.fromCodePoint(src.codePointAt(j + 1) as number);
			j += 1 + lo.length;
		} else {
			lo = String.fromCodePoint(src.codePointAt(j) as number);
			j += lo.length;
		}
		if (src[j] === "-" && j + 1 < end && src[j + 1] !== "]") {
			let hi: string;
			if (src[j + 1] === "\\" && j + 2 < end) {
				hi = String.fromCodePoint(src.codePointAt(j + 2) as number);
				j += 2 + hi.length;
			} else {
				hi = String.fromCodePoint(src.codePointAt(j + 1) as number);
				j += 1 + hi.length;
			}
			// An inverted range matches nothing.
			if ((lo.codePointAt(0) as number) <= (hi.codePointAt(0) as number))
				body += `${escapeClass(lo)}-${escapeClass(hi)}`;
		} else {
			body += escapeClass(lo);
		}
	}
	return null;
}

function tokenize(src: string, start: number, end: number): Token[] {
	const toks: Token[] = [];
	let i = start;
	while (i < end) {
		const c = src[i];
		if (c === "\\") {
			if (i + 1 < end) {
				const ch = String.fromCodePoint(src.codePointAt(i + 1) as number);
				toks.push({ t: "lit", s: ch });
				i += 1 + ch.length;
			} else {
				toks.push({ t: "lit", s: "\\" });
				i++;
			}
			continue;
		}
		if ((c === "?" || c === "*" || c === "+" || c === "@" || c === "!") && src[i + 1] === "(") {
			const close = findParen(src, i + 1, end);
			if (close !== -1) {
				const alts: Token[][] = [];
				for (const [a, b] of splitAlternatives(src, i + 2, close)) alts.push(tokenize(src, a, b));
				toks.push({ t: "ext", kind: c, alts });
				i = close + 1;
				continue;
			}
		}
		if (c === "*") {
			i++;
			while (src[i] === "*" && src[i + 1] !== "(") i++;
			toks.push({ t: "src", s: "[^/]*" });
			continue;
		}
		if (c === "?") {
			toks.push({ t: "src", s: "[^/]" });
			i++;
			continue;
		}
		if (c === "[") {
			const cls = parseClass(src, i, end);
			if (cls) {
				toks.push({ t: "src", s: cls.src });
				i = cls.next;
				continue;
			}
		}
		const ch = String.fromCodePoint(src.codePointAt(i) as number);
		toks.push({ t: "lit", s: ch });
		i += ch.length;
	}
	return toks;
}

/**
 * Regex source for a token sequence. `rest` is the source of everything that
 * follows the sequence inside the segment: `!(…)` needs it for its lookahead.
 */
function emit(toks: Token[], rest: string): string {
	let tail = "";
	for (let k = toks.length - 1; k >= 0; k--) {
		const tok = toks[k];
		let s: string;
		if (tok.t === "lit") s = tok.s.replace(RE_SPECIAL, "\\$&");
		else if (tok.t === "src") s = tok.s;
		else {
			const after = tail + rest;
			if (tok.kind === "!") {
				const alts = tok.alts.map((a) => emit(a, "")).join("|");
				s = `(?:(?!(?:${alts})${after}$)[^/]*?)`;
			} else {
				const alts = tok.alts.map((a) => emit(a, after)).join("|");
				s = tok.kind === "@" ? `(?:${alts})` : `(?:${alts})${tok.kind}`;
			}
		}
		tail = s + tail;
	}
	return tail;
}

function compileSegment(src: string, dot: boolean, nocase: boolean): Segment {
	if (src === "**") return { star: true, literal: null, re: null };
	const toks = tokenize(src, 0, src.length);
	let literal = "";
	for (const tok of toks) {
		if (tok.t !== "lit") {
			const explicitDot = toks[0].t === "lit" && toks[0].s === ".";
			const guard = dot || explicitDot ? "" : "(?!\\.)";
			return { star: false, literal: null, re: new RegExp(`^${guard}${emit(toks, "")}$`, nocase ? "iu" : "u") };
		}
		literal += tok.s;
	}
	return { star: false, literal: nocase ? literal.toLowerCase() : literal, re: null };
}

/** Strip leading `!`s; an odd count negates. `!(` is an extglob, not negation. */
function splitNegation(pattern: string): { negate: boolean; body: string } {
	let i = 0;
	let negate = false;
	while (pattern[i] === "!" && pattern[i + 1] !== "(") {
		negate = !negate;
		i++;
	}
	return { negate, body: pattern.slice(i) };
}

// ---------------------------------------------------------------------------
// Brace expansion

/** Index of the `}` closing the `{` at `open`, or -1. Skips escapes and bracket expressions. */
function findBrace(src: string, open: number): number {
	let depth = 0;
	for (let i = open; i < src.length; i++) {
		const c = src[i];
		if (c === "\\") i++;
		else if (c === "[") {
			const cls = parseClass(src, i, src.length);
			if (cls) i = cls.next - 1;
		} else if (c === "{") depth++;
		else if (c === "}" && --depth === 0) return i;
	}
	return -1;
}

function splitCommas(src: string, a: number, b: number): string[] {
	const out: string[] = [];
	let depth = 0;
	let start = a;
	for (let i = a; i < b; i++) {
		const c = src[i];
		if (c === "\\") i++;
		else if (c === "[") {
			const cls = parseClass(src, i, b);
			if (cls) i = cls.next - 1;
		} else if (c === "{") depth++;
		else if (c === "}") depth--;
		else if (c === "," && depth === 0) {
			out.push(src.slice(start, i));
			start = i + 1;
		}
	}
	out.push(src.slice(start, b));
	return out;
}

const NUM_RANGE = /^(-?\d+)\.\.(-?\d+)(?:\.\.(-?\d+))?$/;
const CHAR_RANGE = /^([^\\])\.\.([^\\])(?:\.\.(-?\d+))?$/u;
const MAX_RANGE = 10000;

function expandRange(body: string): string[] | null {
	const num = NUM_RANGE.exec(body);
	if (num) {
		const a = Number(num[1]);
		const b = Number(num[2]);
		const step = Math.abs(Number(num[3] ?? "1")) || 1;
		const pad = /^-?0\d/.test(num[1]) || /^-?0\d/.test(num[2]) ? Math.max(num[1].length, num[2].length) : 0;
		const out: string[] = [];
		const dir = a <= b ? 1 : -1;
		for (let v = a; dir > 0 ? v <= b : v >= b; v += dir * step) {
			if (out.length >= MAX_RANGE) break;
			let s = String(Math.abs(v));
			const width = pad - (v < 0 ? 1 : 0);
			while (s.length < width) s = `0${s}`;
			out.push(v < 0 ? `-${s}` : s);
		}
		return out;
	}
	const chr = CHAR_RANGE.exec(body);
	if (chr) {
		const a = chr[1].codePointAt(0) as number;
		const b = chr[2].codePointAt(0) as number;
		const step = Math.abs(Number(chr[3] ?? "1")) || 1;
		const out: string[] = [];
		const dir = a <= b ? 1 : -1;
		for (let v = a; dir > 0 ? v <= b : v >= b; v += dir * step) {
			if (out.length >= MAX_RANGE) break;
			// Expanded characters are literal text in the resulting pattern.
			out.push(String.fromCodePoint(v).replace(/[\\*?[\]{}()!@+|,]/, "\\$&"));
		}
		return out;
	}
	return null;
}

function expandInto(pattern: string, out: string[], seen: Set<string>): void {
	for (let i = 0; i < pattern.length; i++) {
		const c = pattern[i];
		if (c === "\\") {
			i++;
			continue;
		}
		if (c === "[") {
			const cls = parseClass(pattern, i, pattern.length);
			if (cls) i = cls.next - 1;
			continue;
		}
		if (c !== "{") continue;
		const close = findBrace(pattern, i);
		if (close === -1) continue; // unbalanced: a literal `{`
		if (close === i + 1) {
			i = close; // `{}` is literal
			continue;
		}
		const prefix = pattern.slice(0, i);
		const suffix = pattern.slice(close + 1);
		const parts = splitCommas(pattern, i + 1, close);
		const alts = parts.length > 1 ? parts : (expandRange(parts[0]) ?? parts);
		for (const alt of alts) expandInto(prefix + alt + suffix, out, seen);
		return;
	}
	if (!seen.has(pattern)) {
		seen.add(pattern);
		out.push(pattern);
	}
}

function expandBraces(pattern: string): string[] {
	const out: string[] = [];
	expandInto(pattern, out, new Set());
	return out;
}

// ---------------------------------------------------------------------------
// Matching

function coalesceStars(segs: Segment[]): Segment[] {
	const out: Segment[] = [];
	for (const s of segs) {
		if (s.star && out.length > 0 && out[out.length - 1].star) continue;
		out.push(s);
	}
	return out;
}

function segmentMatches(seg: Segment, name: string, nocase: boolean): boolean {
	if (seg.literal !== null) return (nocase ? name.toLowerCase() : name) === seg.literal;
	return (seg.re as RegExp).test(name);
}

function matchSegments(segs: Segment[], parts: string[], dot: boolean, nocase: boolean): boolean {
	let hasStar = false;
	for (const s of segs) if (s.star) hasStar = true;
	if (!hasStar) {
		if (segs.length !== parts.length) return false;
		for (let i = 0; i < segs.length; i++) if (!segmentMatches(segs[i], parts[i], nocase)) return false;
		return true;
	}
	const width = parts.length + 1;
	// 0 unknown, 1 no, 2 yes
	const memo = new Uint8Array((segs.length + 1) * width);
	const rec = (pi: number, si: number): boolean => {
		const key = pi * width + si;
		const m = memo[key];
		if (m !== 0) return m === 2;
		let ok: boolean;
		if (pi === segs.length) ok = si === parts.length;
		else if (segs[pi].star) {
			ok = rec(pi + 1, si) || (si < parts.length && (dot || parts[si][0] !== ".") && rec(pi, si + 1));
		} else {
			ok = si < parts.length && segmentMatches(segs[pi], parts[si], nocase) && rec(pi + 1, si + 1);
		}
		memo[key] = ok ? 2 : 1;
		return ok;
	};
	return rec(0, 0);
}

/** Compile a Bun-syntax glob; `match()` tests a `/`-separated path as-is. */
export function compileGlob(pattern: string, opts?: CompileOptions): GlobMatcher {
	const dot = !!opts?.dot;
	const nocase = !!opts?.nocase;
	const { negate, body } = splitNegation(pattern);
	const alternatives: Segment[][] = [];
	for (const p of expandBraces(body)) {
		alternatives.push(coalesceStars(p.split("/").map((s) => compileSegment(s, dot, nocase))));
	}
	return {
		match(path: string): boolean {
			const parts = path.split("/");
			let hit = false;
			for (const segs of alternatives) {
				if (matchSegments(segs, parts, dot, nocase)) {
					hit = true;
					break;
				}
			}
			return hit !== negate;
		},
	};
}

// ---------------------------------------------------------------------------
// Walking

interface DirEntryLike {
	name: string;
	isFile(): boolean;
	isDirectory(): boolean;
	isSymbolicLink(): boolean;
}

interface StatsLike {
	dev: number;
	ino: number;
	isFile(): boolean;
	isDirectory(): boolean;
	isSymbolicLink(): boolean;
}

interface FsLike {
	readdirSync(path: string, opts: { withFileTypes: true }): DirEntryLike[];
	statSync(path: string): StatsLike;
	lstatSync(path: string): StatsLike;
	promises: {
		readdir(path: string, opts: { withFileTypes: true }): Promise<DirEntryLike[]>;
		stat(path: string): Promise<StatsLike>;
		lstat(path: string): Promise<StatsLike>;
	};
}

type IoRequest = { op: "readdir" | "stat" | "lstat"; path: string };
interface IoResponse {
	entries?: DirEntryLike[];
	stats?: StatsLike;
	error?: Error;
}
/** The walker yields I/O requests (answered by a sync or async driver) and result paths. */
type WalkStep = Generator<IoRequest | string, void, IoResponse | undefined>;

interface WalkPattern {
	segs: Segment[];
	/** Pattern ended with `/`: only directories match. */
	dirOnly: boolean;
	/** Negated pattern: walk everything, keep entries the matcher rejects. */
	reject: GlobMatcher | null;
}

interface State {
	p: WalkPattern;
	i: number;
}

interface Frame {
	abs: string;
	rel: string;
	states: State[];
	/** dev:ino of this directory and its ancestors (only with followSymlinks). */
	ancestors: Set<string> | null;
}

/** Add state (p, i) and, while it is a `**`, the states after it. Drops end states. */
function addState(out: State[], keys: Map<WalkPattern, Set<number>>, p: WalkPattern, i: number): void {
	while (i < p.segs.length) {
		let set = keys.get(p);
		if (!set) {
			set = new Set();
			keys.set(p, set);
		}
		if (set.has(i)) return;
		set.add(i);
		out.push({ p, i });
		if (!p.segs[i].star) return;
		i++;
	}
}

function compileWalkPatterns(patterns: string[], dot: boolean): Map<string, WalkPattern[]> {
	const groups = new Map<string, WalkPattern[]>();
	const add = (prefix: string, wp: WalkPattern): void => {
		const list = groups.get(prefix);
		if (list) list.push(wp);
		else groups.set(prefix, [wp]);
	};
	for (const pattern of patterns) {
		const { negate, body } = splitNegation(pattern);
		if (negate) {
			add("", { segs: [compileSegment("**", dot, false)], dirOnly: false, reject: compileGlob(body, { dot }) });
			continue;
		}
		for (const expanded of expandBraces(body)) {
			const raw = expanded.split("/");
			const absolute = raw.length > 1 && raw[0] === "";
			const dirOnly = raw.length > 1 && raw[raw.length - 1] === "";
			const segs: Segment[] = [];
			for (const s of raw) if (s !== "" && s !== ".") segs.push(compileSegment(s, dot, false));
			if (segs.length === 0) continue;
			const merged = coalesceStars(segs);
			// The literal prefix (never the last segment) is the walk root.
			let n = 0;
			while (n < merged.length - 1 && merged[n].literal !== null) n++;
			const lit: string[] = [];
			for (let k = 0; k < n; k++) lit.push(merged[k].literal as string);
			let prefix = lit.join("/");
			if (absolute) prefix = `/${prefix}`;
			if (prefix !== "") prefix = pathPosix.normalize(prefix);
			if (prefix === ".") prefix = "";
			add(prefix, { segs: merged.slice(n), dirOnly, reject: null });
		}
	}
	return groups;
}

const KIND_FILE = 0;
const KIND_DIR = 1;
const KIND_LINK = 2;

function* walkCore(patterns: string[], opts: WalkOptions): WalkStep {
	const dot = !!opts.dot;
	const absolute = !!opts.absolute;
	const follow = !!opts.followSymlinks;
	const onlyFiles = opts.onlyFiles !== false;
	const throwBroken = !!opts.throwErrorOnBrokenSymlink;
	const exclude = opts.exclude;
	const cwd = pathPosix.resolve(opts.cwd);
	const groups = compileWalkPatterns(patterns, dot);
	const seen: Set<string> | null = groups.size > 1 ? new Set() : null;

	for (const [prefix, pats] of groups) {
		const rootAbs = pathPosix.resolve(cwd, prefix);
		const rootStates: State[] = [];
		const rootKeys = new Map<WalkPattern, Set<number>>();
		for (const p of pats) addState(rootStates, rootKeys, p, 0);
		let rootAncestors: Set<string> | null = null;
		if (follow) {
			const res = (yield { op: "stat", path: rootAbs }) as IoResponse;
			if (!res.stats) continue;
			rootAncestors = new Set([`${res.stats.dev}:${res.stats.ino}`]);
		}
		const stack: Frame[] = [{ abs: rootAbs, rel: prefix, states: rootStates, ancestors: rootAncestors }];

		while (stack.length > 0) {
			const frame = stack.pop() as Frame;
			const { abs, rel, states } = frame;

			// Collect candidate entries: read the directory unless every state
			// is a literal name, in which case just lstat those names.
			let allLiteral = true;
			for (const s of states) if (s.p.segs[s.i].literal === null) allLiteral = false;
			const entries: { name: string; kind: number }[] = [];
			if (allLiteral) {
				const names = new Set<string>();
				for (const s of states) names.add(s.p.segs[s.i].literal as string);
				for (const name of names) {
					const res = (yield { op: "lstat", path: pathPosix.join(abs, name) }) as IoResponse;
					if (!res.stats) continue;
					const st = res.stats;
					entries.push({ name, kind: st.isSymbolicLink() ? KIND_LINK : st.isDirectory() ? KIND_DIR : KIND_FILE });
				}
			} else {
				const res = (yield { op: "readdir", path: abs }) as IoResponse;
				if (!res.entries) continue;
				for (const d of res.entries) {
					let kind = d.isSymbolicLink() ? KIND_LINK : d.isDirectory() ? KIND_DIR : d.isFile() ? KIND_FILE : -1;
					if (kind === -1) {
						const st = ((yield { op: "lstat", path: pathPosix.join(abs, d.name) }) as IoResponse).stats;
						kind = !st ? KIND_FILE : st.isSymbolicLink() ? KIND_LINK : st.isDirectory() ? KIND_DIR : KIND_FILE;
					}
					entries.push({ name: d.name, kind });
				}
			}

			for (const entry of entries) {
				const name = entry.name;
				const dotOk = dot || name[0] !== ".";
				const childStates: State[] = [];
				const childKeys = new Map<WalkPattern, Set<number>>();
				// Patterns whose last segment matched this entry.
				let matchAny: WalkPattern[] | null = null;
				for (const s of states) {
					const seg = s.p.segs[s.i];
					const last = s.i === s.p.segs.length - 1;
					if (seg.star) {
						if (!dotOk) continue;
						addState(childStates, childKeys, s.p, s.i);
						if (last) (matchAny ??= []).push(s.p);
					} else if (segmentMatches(seg, name, false)) {
						if (last) (matchAny ??= []).push(s.p);
						else addState(childStates, childKeys, s.p, s.i + 1);
					}
				}
				if (matchAny === null && childStates.length === 0) continue;

				const childRel = rel === "" ? name : rel.endsWith("/") ? rel + name : `${rel}/${name}`;
				const childAbs = pathPosix.join(abs, name);
				// Resolve what the entry is: traversable directory, file, unfollowed link, broken link.
				let isDir = entry.kind === KIND_DIR;
				// Unfollowed links and directories are reported only when onlyFiles is off.
				let emittable = entry.kind === KIND_FILE || !onlyFiles;
				let id: string | null = null;
				if (entry.kind === KIND_LINK) {
					if (follow) {
						const res = (yield { op: "stat", path: childAbs }) as IoResponse;
						if (!res.stats) {
							if (throwBroken) throw res.error as Error;
							emittable = !onlyFiles;
						} else {
							isDir = res.stats.isDirectory();
							emittable = !isDir || !onlyFiles;
							id = `${res.stats.dev}:${res.stats.ino}`;
						}
					}
				}

				if (exclude && exclude(childRel, isDir)) continue;

				if (matchAny !== null && emittable) {
					let hit = false;
					for (const p of matchAny) {
						if (p.dirOnly && !isDir) continue;
						if (p.reject && p.reject.match(childRel)) continue;
						hit = true;
						break;
					}
					if (hit && (seen === null || !seen.has(childRel))) {
						seen?.add(childRel);
						yield absolute ? pathPosix.resolve(cwd, childRel) : childRel;
					}
				}

				// isDir is set only for real directories and followed links to directories.
				if (!isDir || childStates.length === 0) continue;
				let ancestors: Set<string> | null = null;
				if (follow) {
					if (id === null) {
						const res = (yield { op: "stat", path: childAbs }) as IoResponse;
						if (!res.stats) continue;
						id = `${res.stats.dev}:${res.stats.ino}`;
					}
					if ((frame.ancestors as Set<string>).has(id)) continue; // symlink loop
					ancestors = new Set(frame.ancestors);
					ancestors.add(id);
				}
				stack.push({ abs: childAbs, rel: childRel, states: childStates, ancestors });
			}
		}
	}
}

function answerSync(fs: FsLike, req: IoRequest): IoResponse {
	try {
		if (req.op === "readdir") return { entries: fs.readdirSync(req.path, { withFileTypes: true }) };
		if (req.op === "stat") return { stats: fs.statSync(req.path) };
		return { stats: fs.lstatSync(req.path) };
	} catch (e) {
		return { error: e as Error };
	}
}

async function answerAsync(fs: FsLike, req: IoRequest): Promise<IoResponse> {
	try {
		if (req.op === "readdir") return { entries: await fs.promises.readdir(req.path, { withFileTypes: true }) };
		if (req.op === "stat") return { stats: await fs.promises.stat(req.path) };
		return { stats: await fs.promises.lstat(req.path) };
	} catch (e) {
		return { error: e as Error };
	}
}

/** Walk `opts.cwd` for entries matching any pattern; paths use `/` separators. */
export function globWalkSync(patterns: string[], opts: WalkOptions): string[] {
	const out: string[] = [];
	const gen = walkCore(patterns, opts);
	let res: IoResponse | undefined;
	for (;;) {
		const step = gen.next(res);
		if (step.done) break;
		const v = step.value;
		if (typeof v === "string") {
			out.push(v);
			res = undefined;
		} else {
			res = answerSync(fs, v);
		}
	}
	return out;
}

export async function* globWalk(patterns: string[], opts: WalkOptions): AsyncGenerator<string> {
	const gen = walkCore(patterns, opts);
	let res: IoResponse | undefined;
	for (;;) {
		const step = gen.next(res);
		if (step.done) break;
		const v = step.value;
		if (typeof v === "string") {
			res = undefined;
			yield v;
		} else {
			res = await answerAsync(fs, v);
		}
	}
}
