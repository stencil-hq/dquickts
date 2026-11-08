/* Vendored node-semver 7.8.5 (comparison and range dependency closure).
The ISC License

Copyright (c) Isaac Z. Schlueter and Contributors

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

*/
type CommonJSModule = { exports: unknown };
type Factory = (module: CommonJSModule, exports: Record<string, unknown>, require: (id: string) => unknown) => void;
const factories: Record<string, Factory> = {
	"functions/satisfies": function (module, exports, require) {
		"use strict";

		const Range = require("classes/range");
		const satisfies = (version, range, options) => {
			try {
				range = new Range(range, options);
			} catch (er) {
				return false;
			}
			return range.test(version);
		};
		module.exports = satisfies;
	},
	"classes/range": function (module, exports, require) {
		"use strict";

		const SPACE_CHARACTERS = /\s+/g;

		// hoisted class for cyclic dependency
		class Range {
			constructor(range, options) {
				options = parseOptions(options);

				if (range instanceof Range) {
					if (range.loose === !!options.loose && range.includePrerelease === !!options.includePrerelease) {
						return range;
					} else {
						return new Range(range.raw, options);
					}
				}

				if (range instanceof Comparator) {
					// just put it in the set and return
					this.raw = range.value;
					this.set = [[range]];
					this.formatted = undefined;
					return this;
				}

				this.options = options;
				this.loose = !!options.loose;
				this.includePrerelease = !!options.includePrerelease;

				// First reduce all whitespace as much as possible so we do not have to rely
				// on potentially slow regexes like \s*. This is then stored and used for
				// future error messages as well.
				this.raw = range.trim().replace(SPACE_CHARACTERS, " ");

				// First, split on ||
				this.set = this.raw
					.split("||")
					// map the range to a 2d array of comparators
					.map((r) => this.parseRange(r.trim()))
					// throw out any comparator lists that are empty
					// this generally means that it was not a valid range, which is allowed
					// in loose mode, but will still throw if the WHOLE range is invalid.
					.filter((c) => c.length);

				if (!this.set.length) {
					throw new TypeError(`Invalid SemVer Range: ${this.raw}`);
				}

				// if we have any that are not the null set, throw out null sets.
				if (this.set.length > 1) {
					// keep the first one, in case they're all null sets
					const first = this.set[0];
					this.set = this.set.filter((c) => !isNullSet(c[0]));
					if (this.set.length === 0) {
						this.set = [first];
					} else if (this.set.length > 1) {
						// if we have any that are *, then the range is just *
						for (const c of this.set) {
							if (c.length === 1 && isAny(c[0])) {
								this.set = [c];
								break;
							}
						}
					}
				}

				this.formatted = undefined;
			}

			get range() {
				if (this.formatted === undefined) {
					this.formatted = "";
					for (let i = 0; i < this.set.length; i++) {
						if (i > 0) {
							this.formatted += "||";
						}
						const comps = this.set[i];
						for (let k = 0; k < comps.length; k++) {
							if (k > 0) {
								this.formatted += " ";
							}
							this.formatted += comps[k].toString().trim();
						}
					}
				}
				return this.formatted;
			}

			format() {
				return this.range;
			}

			toString() {
				return this.range;
			}

			parseRange(range) {
				// strip build metadata so it can't bleed into the version
				range = range.replace(BUILDSTRIPRE, "");

				// memoize range parsing for performance.
				// this is a very hot path, and fully deterministic.
				const memoOpts =
					(this.options.includePrerelease && FLAG_INCLUDE_PRERELEASE) | (this.options.loose && FLAG_LOOSE);
				const memoKey = memoOpts + ":" + range;
				const cached = cache.get(memoKey);
				if (cached) {
					return cached;
				}

				const loose = this.options.loose;
				// `1.2.3 - 1.2.4` => `>=1.2.3 <=1.2.4`
				const hr = loose ? re[t.HYPHENRANGELOOSE] : re[t.HYPHENRANGE];
				range = range.replace(hr, hyphenReplace(this.options.includePrerelease));
				debug("hyphen replace", range);

				// `> 1.2.3 < 1.2.5` => `>1.2.3 <1.2.5`
				range = range.replace(re[t.COMPARATORTRIM], comparatorTrimReplace);
				debug("comparator trim", range);

				// `~ 1.2.3` => `~1.2.3`
				range = range.replace(re[t.TILDETRIM], tildeTrimReplace);
				debug("tilde trim", range);

				// `^ 1.2.3` => `^1.2.3`
				range = range.replace(re[t.CARETTRIM], caretTrimReplace);
				debug("caret trim", range);

				// At this point, the range is completely trimmed and
				// ready to be split into comparators.

				let rangeList = range
					.split(" ")
					.map((comp) => parseComparator(comp, this.options))
					.join(" ")
					.split(/\s+/)
					// >=0.0.0 is equivalent to *
					.map((comp) => replaceGTE0(comp, this.options));

				if (loose) {
					// in loose mode, throw out any that are not valid comparators
					rangeList = rangeList.filter((comp) => {
						debug("loose invalid filter", comp, this.options);
						return !!comp.match(re[t.COMPARATORLOOSE]);
					});
				}
				debug("range list", rangeList);

				// if any comparators are the null set, then replace with JUST null set
				// if more than one comparator, remove any * comparators
				// also, don't include the same comparator more than once
				const rangeMap = new Map();
				const comparators = rangeList.map((comp) => new Comparator(comp, this.options));
				for (const comp of comparators) {
					if (isNullSet(comp)) {
						return [comp];
					}
					rangeMap.set(comp.value, comp);
				}
				if (rangeMap.size > 1 && rangeMap.has("")) {
					rangeMap.delete("");
				}

				const result = [...rangeMap.values()];
				cache.set(memoKey, result);
				return result;
			}

			intersects(range, options) {
				if (!(range instanceof Range)) {
					throw new TypeError("a Range is required");
				}

				return this.set.some((thisComparators) => {
					return (
						isSatisfiable(thisComparators, options) &&
						range.set.some((rangeComparators) => {
							return (
								isSatisfiable(rangeComparators, options) &&
								thisComparators.every((thisComparator) => {
									return rangeComparators.every((rangeComparator) => {
										return thisComparator.intersects(rangeComparator, options);
									});
								})
							);
						})
					);
				});
			}

			// if ANY of the sets match ALL of its comparators, then pass
			test(version) {
				if (!version) {
					return false;
				}

				if (typeof version === "string") {
					try {
						version = new SemVer(version, this.options);
					} catch (er) {
						return false;
					}
				}

				for (let i = 0; i < this.set.length; i++) {
					if (testSet(this.set[i], version, this.options)) {
						return true;
					}
				}
				return false;
			}
		}

		module.exports = Range;

		const LRU = require("internal/lrucache");
		const cache = new LRU();

		const parseOptions = require("internal/parse-options");
		const Comparator = require("classes/comparator");
		const debug = require("internal/debug");
		const SemVer = require("classes/semver");
		const { safeRe: re, src, t, comparatorTrimReplace, tildeTrimReplace, caretTrimReplace } = require("internal/re");
		const { FLAG_INCLUDE_PRERELEASE, FLAG_LOOSE } = require("internal/constants");

		// unbounded global build-metadata stripper used by parseRange
		const BUILDSTRIPRE = new RegExp(src[t.BUILD], "g");

		const isNullSet = (c) => c.value === "<0.0.0-0";
		const isAny = (c) => c.value === "";

		// take a set of comparators and determine whether there
		// exists a version which can satisfy it
		const isSatisfiable = (comparators, options) => {
			let result = true;
			const remainingComparators = comparators.slice();
			let testComparator = remainingComparators.pop();

			while (result && remainingComparators.length) {
				result = remainingComparators.every((otherComparator) => {
					return testComparator.intersects(otherComparator, options);
				});

				testComparator = remainingComparators.pop();
			}

			return result;
		};

		// comprised of xranges, tildes, stars, and gtlt's at this point.
		// already replaced the hyphen ranges
		// turn into a set of JUST comparators.
		const parseComparator = (comp, options) => {
			comp = comp.replace(re[t.BUILD], "");
			debug("comp", comp, options);
			comp = replaceCarets(comp, options);
			debug("caret", comp);
			comp = replaceTildes(comp, options);
			debug("tildes", comp);
			comp = replaceXRanges(comp, options);
			debug("xrange", comp);
			comp = replaceStars(comp, options);
			debug("stars", comp);
			return comp;
		};

		const isX = (id) => !id || id.toLowerCase() === "x" || id === "*";

		const invalidXRangeOrder = (M, m, p) => (isX(M) && !isX(m)) || (isX(m) && p && !isX(p));

		// ~, ~> --> * (any, kinda silly)
		// ~2, ~2.x, ~2.x.x, ~>2, ~>2.x ~>2.x.x --> >=2.0.0 <3.0.0-0
		// ~2.0, ~2.0.x, ~>2.0, ~>2.0.x --> >=2.0.0 <2.1.0-0
		// ~1.2, ~1.2.x, ~>1.2, ~>1.2.x --> >=1.2.0 <1.3.0-0
		// ~1.2.3, ~>1.2.3 --> >=1.2.3 <1.3.0-0
		// ~1.2.0, ~>1.2.0 --> >=1.2.0 <1.3.0-0
		// ~0.0.1 --> >=0.0.1 <0.1.0-0
		const replaceTildes = (comp, options) => {
			return comp
				.trim()
				.split(/\s+/)
				.map((c) => replaceTilde(c, options))
				.join(" ");
		};

		const replaceTilde = (comp, options) => {
			const r = options.loose ? re[t.TILDELOOSE] : re[t.TILDE];
			// if we're including prereleases in the match, then the lower bound is
			// -0, the lowest possible prerelease value, just like x-ranges and carets.
			// this keeps `~1.2` equivalent to the `1.2.x` x-range it's documented as.
			const z = options.includePrerelease ? "-0" : "";
			return comp.replace(r, (_, M, m, p, pr) => {
				debug("tilde", comp, _, M, m, p, pr);
				let ret;

				if (isX(M)) {
					ret = "";
				} else if (isX(m)) {
					ret = `>=${M}.0.0${z} <${+M + 1}.0.0-0`;
				} else if (isX(p)) {
					// ~1.2 == >=1.2.0 <1.3.0-0
					ret = `>=${M}.${m}.0${z} <${M}.${+m + 1}.0-0`;
				} else if (pr) {
					debug("replaceTilde pr", pr);
					ret = `>=${M}.${m}.${p}-${pr} <${M}.${+m + 1}.0-0`;
				} else {
					// ~1.2.3 == >=1.2.3 <1.3.0-0
					ret = `>=${M}.${m}.${p} <${M}.${+m + 1}.0-0`;
				}

				debug("tilde return", ret);
				return ret;
			});
		};

		// ^ --> * (any, kinda silly)
		// ^2, ^2.x, ^2.x.x --> >=2.0.0 <3.0.0-0
		// ^2.0, ^2.0.x --> >=2.0.0 <3.0.0-0
		// ^1.2, ^1.2.x --> >=1.2.0 <2.0.0-0
		// ^1.2.3 --> >=1.2.3 <2.0.0-0
		// ^1.2.0 --> >=1.2.0 <2.0.0-0
		// ^0.0.1 --> >=0.0.1 <0.0.2-0
		// ^0.1.0 --> >=0.1.0 <0.2.0-0
		const replaceCarets = (comp, options) => {
			return comp
				.trim()
				.split(/\s+/)
				.map((c) => replaceCaret(c, options))
				.join(" ");
		};

		const replaceCaret = (comp, options) => {
			debug("caret", comp, options);
			const r = options.loose ? re[t.CARETLOOSE] : re[t.CARET];
			const z = options.includePrerelease ? "-0" : "";
			return comp.replace(r, (_, M, m, p, pr) => {
				debug("caret", comp, _, M, m, p, pr);
				let ret;

				if (isX(M)) {
					ret = "";
				} else if (isX(m)) {
					ret = `>=${M}.0.0${z} <${+M + 1}.0.0-0`;
				} else if (isX(p)) {
					if (M === "0") {
						ret = `>=${M}.${m}.0${z} <${M}.${+m + 1}.0-0`;
					} else {
						ret = `>=${M}.${m}.0${z} <${+M + 1}.0.0-0`;
					}
				} else if (pr) {
					debug("replaceCaret pr", pr);
					if (M === "0") {
						if (m === "0") {
							ret = `>=${M}.${m}.${p}-${pr} <${M}.${m}.${+p + 1}-0`;
						} else {
							ret = `>=${M}.${m}.${p}-${pr} <${M}.${+m + 1}.0-0`;
						}
					} else {
						ret = `>=${M}.${m}.${p}-${pr} <${+M + 1}.0.0-0`;
					}
				} else {
					debug("no pr");
					if (M === "0") {
						if (m === "0") {
							ret = `>=${M}.${m}.${p} <${M}.${m}.${+p + 1}-0`;
						} else {
							ret = `>=${M}.${m}.${p} <${M}.${+m + 1}.0-0`;
						}
					} else {
						ret = `>=${M}.${m}.${p} <${+M + 1}.0.0-0`;
					}
				}

				debug("caret return", ret);
				return ret;
			});
		};

		const replaceXRanges = (comp, options) => {
			debug("replaceXRanges", comp, options);
			return comp
				.split(/\s+/)
				.map((c) => replaceXRange(c, options))
				.join(" ");
		};

		const replaceXRange = (comp, options) => {
			comp = comp.trim();
			const r = options.loose ? re[t.XRANGELOOSE] : re[t.XRANGE];
			return comp.replace(r, (ret, gtlt, M, m, p, pr) => {
				debug("xRange", comp, ret, gtlt, M, m, p, pr);
				if (invalidXRangeOrder(M, m, p)) {
					return comp;
				}

				const xM = isX(M);
				const xm = xM || isX(m);
				const xp = xm || isX(p);
				const anyX = xp;

				if (gtlt === "=" && anyX) {
					gtlt = "";
				}

				// if we're including prereleases in the match, then we need
				// to fix this to -0, the lowest possible prerelease value
				pr = options.includePrerelease ? "-0" : "";

				if (xM) {
					if (gtlt === ">" || gtlt === "<") {
						// nothing is allowed
						ret = "<0.0.0-0";
					} else {
						// nothing is forbidden
						ret = "*";
					}
				} else if (gtlt && anyX) {
					// we know patch is an x, because we have any x at all.
					// replace X with 0
					if (xm) {
						m = 0;
					}
					p = 0;

					if (gtlt === ">") {
						// >1 => >=2.0.0
						// >1.2 => >=1.3.0
						gtlt = ">=";
						if (xm) {
							M = +M + 1;
							m = 0;
							p = 0;
						} else {
							m = +m + 1;
							p = 0;
						}
					} else if (gtlt === "<=") {
						// <=0.7.x is actually <0.8.0, since any 0.7.x should
						// pass.  Similarly, <=7.x is actually <8.0.0, etc.
						gtlt = "<";
						if (xm) {
							M = +M + 1;
						} else {
							m = +m + 1;
						}
					}

					if (gtlt === "<") {
						pr = "-0";
					}

					ret = `${gtlt + M}.${m}.${p}${pr}`;
				} else if (xm) {
					ret = `>=${M}.0.0${pr} <${+M + 1}.0.0-0`;
				} else if (xp) {
					ret = `>=${M}.${m}.0${pr} <${M}.${+m + 1}.0-0`;
				}

				debug("xRange return", ret);

				return ret;
			});
		};

		// Because * is AND-ed with everything else in the comparator,
		// and '' means "any version", just remove the *s entirely.
		const replaceStars = (comp, options) => {
			debug("replaceStars", comp, options);
			// Looseness is ignored here.  star is always as loose as it gets!
			return comp.trim().replace(re[t.STAR], "");
		};

		const replaceGTE0 = (comp, options) => {
			debug("replaceGTE0", comp, options);
			return comp.trim().replace(re[options.includePrerelease ? t.GTE0PRE : t.GTE0], "");
		};

		// This function is passed to string.replace(re[t.HYPHENRANGE])
		// M, m, patch, prerelease, build
		// 1.2 - 3.4.5 => >=1.2.0 <=3.4.5
		// 1.2.3 - 3.4 => >=1.2.0 <3.5.0-0 Any 3.4.x will do
		// 1.2 - 3.4 => >=1.2.0 <3.5.0-0
		// TODO build?
		const hyphenReplace = (incPr) => ($0, from, fM, fm, fp, fpr, fb, to, tM, tm, tp, tpr) => {
			if (isX(fM)) {
				from = "";
			} else if (isX(fm)) {
				from = `>=${fM}.0.0${incPr ? "-0" : ""}`;
			} else if (isX(fp)) {
				from = `>=${fM}.${fm}.0${incPr ? "-0" : ""}`;
			} else if (fpr) {
				from = `>=${from}`;
			} else {
				from = `>=${from}${incPr ? "-0" : ""}`;
			}

			if (isX(tM)) {
				to = "";
			} else if (isX(tm)) {
				to = `<${+tM + 1}.0.0-0`;
			} else if (isX(tp)) {
				to = `<${tM}.${+tm + 1}.0-0`;
			} else if (tpr) {
				to = `<=${tM}.${tm}.${tp}-${tpr}`;
			} else if (incPr) {
				to = `<${tM}.${tm}.${+tp + 1}-0`;
			} else {
				to = `<=${to}`;
			}

			return `${from} ${to}`.trim();
		};

		const testSet = (set, version, options) => {
			for (let i = 0; i < set.length; i++) {
				if (!set[i].test(version)) {
					return false;
				}
			}

			if (version.prerelease.length && !options.includePrerelease) {
				// Find the set of versions that are allowed to have prereleases
				// For example, ^1.2.3-pr.1 desugars to >=1.2.3-pr.1 <2.0.0
				// That should allow `1.2.3-pr.2` to pass.
				// However, `1.2.4-alpha.notready` should NOT be allowed,
				// even though it's within the range set by the comparators.
				for (let i = 0; i < set.length; i++) {
					debug(set[i].semver);
					if (set[i].semver === Comparator.ANY) {
						continue;
					}

					if (set[i].semver.prerelease.length > 0) {
						const allowed = set[i].semver;
						if (allowed.major === version.major && allowed.minor === version.minor && allowed.patch === version.patch) {
							return true;
						}
					}
				}

				// Version has a -pre, but it's not one of the ones we like.
				return false;
			}

			return true;
		};
	},
	"internal/lrucache": function (module, exports, require) {
		"use strict";

		class LRUCache {
			constructor() {
				this.max = 1000;
				this.map = new Map();
			}

			get(key) {
				const value = this.map.get(key);
				if (value === undefined) {
					return undefined;
				} else {
					// Remove the key from the map and add it to the end
					this.map.delete(key);
					this.map.set(key, value);
					return value;
				}
			}

			delete(key) {
				return this.map.delete(key);
			}

			set(key, value) {
				const deleted = this.delete(key);

				if (!deleted && value !== undefined) {
					// If cache is full, delete the least recently used item
					if (this.map.size >= this.max) {
						const firstKey = this.map.keys().next().value;
						this.delete(firstKey);
					}

					this.map.set(key, value);
				}

				return this;
			}
		}

		module.exports = LRUCache;
	},
	"internal/parse-options": function (module, exports, require) {
		"use strict";

		// parse out just the options we care about
		const looseOption = Object.freeze({ loose: true });
		const emptyOpts = Object.freeze({});
		const parseOptions = (options) => {
			if (!options) {
				return emptyOpts;
			}

			if (typeof options !== "object") {
				return looseOption;
			}

			return options;
		};
		module.exports = parseOptions;
	},
	"classes/comparator": function (module, exports, require) {
		"use strict";

		const ANY = Symbol("SemVer ANY");
		// hoisted class for cyclic dependency
		class Comparator {
			static get ANY() {
				return ANY;
			}

			constructor(comp, options) {
				options = parseOptions(options);

				if (comp instanceof Comparator) {
					if (comp.loose === !!options.loose) {
						return comp;
					} else {
						comp = comp.value;
					}
				}

				comp = comp.trim().split(/\s+/).join(" ");
				debug("comparator", comp, options);
				this.options = options;
				this.loose = !!options.loose;
				this.parse(comp);

				if (this.semver === ANY) {
					this.value = "";
				} else {
					this.value = this.operator + this.semver.version;
				}

				debug("comp", this);
			}

			parse(comp) {
				const r = this.options.loose ? re[t.COMPARATORLOOSE] : re[t.COMPARATOR];
				const m = comp.match(r);

				if (!m) {
					throw new TypeError(`Invalid comparator: ${comp}`);
				}

				this.operator = m[1] !== undefined ? m[1] : "";
				if (this.operator === "=") {
					this.operator = "";
				}

				// if it literally is just '>' or '' then allow anything.
				if (!m[2]) {
					this.semver = ANY;
				} else {
					this.semver = new SemVer(m[2], this.options.loose);
				}
			}

			toString() {
				return this.value;
			}

			test(version) {
				debug("Comparator.test", version, this.options.loose);

				if (this.semver === ANY || version === ANY) {
					return true;
				}

				if (typeof version === "string") {
					try {
						version = new SemVer(version, this.options);
					} catch (er) {
						return false;
					}
				}

				return cmp(version, this.operator, this.semver, this.options);
			}

			intersects(comp, options) {
				if (!(comp instanceof Comparator)) {
					throw new TypeError("a Comparator is required");
				}

				if (this.operator === "") {
					if (this.value === "") {
						return true;
					}
					return new Range(comp.value, options).test(this.value);
				} else if (comp.operator === "") {
					if (comp.value === "") {
						return true;
					}
					return new Range(this.value, options).test(comp.semver);
				}

				options = parseOptions(options);

				// Special cases where nothing can possibly be lower
				if (options.includePrerelease && (this.value === "<0.0.0-0" || comp.value === "<0.0.0-0")) {
					return false;
				}
				if (!options.includePrerelease && (this.value.startsWith("<0.0.0") || comp.value.startsWith("<0.0.0"))) {
					return false;
				}

				// Same direction increasing (> or >=)
				if (this.operator.startsWith(">") && comp.operator.startsWith(">")) {
					return true;
				}
				// Same direction decreasing (< or <=)
				if (this.operator.startsWith("<") && comp.operator.startsWith("<")) {
					return true;
				}
				// same SemVer and both sides are inclusive (<= or >=)
				if (this.semver.version === comp.semver.version && this.operator.includes("=") && comp.operator.includes("=")) {
					return true;
				}
				// opposite directions less than
				if (
					cmp(this.semver, "<", comp.semver, options) &&
					this.operator.startsWith(">") &&
					comp.operator.startsWith("<")
				) {
					return true;
				}
				// opposite directions greater than
				if (
					cmp(this.semver, ">", comp.semver, options) &&
					this.operator.startsWith("<") &&
					comp.operator.startsWith(">")
				) {
					return true;
				}
				return false;
			}
		}

		module.exports = Comparator;

		const parseOptions = require("internal/parse-options");
		const { safeRe: re, t } = require("internal/re");
		const cmp = require("functions/cmp");
		const debug = require("internal/debug");
		const SemVer = require("classes/semver");
		const Range = require("classes/range");
	},
	"internal/re": function (module, exports, require) {
		"use strict";

		const { MAX_SAFE_COMPONENT_LENGTH, MAX_SAFE_BUILD_LENGTH, MAX_LENGTH } = require("internal/constants");
		const debug = require("internal/debug");
		exports = module.exports = {};

		// The actual regexps go on exports.re
		const re = (exports.re = []);
		const safeRe = (exports.safeRe = []);
		const src = (exports.src = []);
		const safeSrc = (exports.safeSrc = []);
		const t = (exports.t = {});
		let R = 0;

		const LETTERDASHNUMBER = "[a-zA-Z0-9-]";

		// Replace some greedy regex tokens to prevent regex dos issues. These regex are
		// used internally via the safeRe object since all inputs in this library get
		// normalized first to trim and collapse all extra whitespace. The original
		// regexes are exported for userland consumption and lower level usage. A
		// future breaking change could export the safer regex only with a note that
		// all input should have extra whitespace removed.
		const safeRegexReplacements = [
			["\\s", 1],
			["\\d", MAX_LENGTH],
			[LETTERDASHNUMBER, MAX_SAFE_BUILD_LENGTH],
		];

		const makeSafeRegex = (value) => {
			for (const [token, max] of safeRegexReplacements) {
				value = value.split(`${token}*`).join(`${token}{0,${max}}`).split(`${token}+`).join(`${token}{1,${max}}`);
			}
			return value;
		};

		const createToken = (name, value, isGlobal) => {
			const safe = makeSafeRegex(value);
			const index = R++;
			debug(name, index, value);
			t[name] = index;
			src[index] = value;
			safeSrc[index] = safe;
			re[index] = new RegExp(value, isGlobal ? "g" : undefined);
			safeRe[index] = new RegExp(safe, isGlobal ? "g" : undefined);
		};

		// The following Regular Expressions can be used for tokenizing,
		// validating, and parsing SemVer version strings.

		// ## Numeric Identifier
		// A single `0`, or a non-zero digit followed by zero or more digits.

		createToken("NUMERICIDENTIFIER", "0|[1-9]\\d*");
		createToken("NUMERICIDENTIFIERLOOSE", "\\d+");

		// ## Non-numeric Identifier
		// Zero or more digits, followed by a letter or hyphen, and then zero or
		// more letters, digits, or hyphens.

		createToken("NONNUMERICIDENTIFIER", `\\d*[a-zA-Z-]${LETTERDASHNUMBER}*`);

		// ## Main Version
		// Three dot-separated numeric identifiers.

		createToken(
			"MAINVERSION",
			`(${src[t.NUMERICIDENTIFIER]})\\.` + `(${src[t.NUMERICIDENTIFIER]})\\.` + `(${src[t.NUMERICIDENTIFIER]})`,
		);

		createToken(
			"MAINVERSIONLOOSE",
			`(${src[t.NUMERICIDENTIFIERLOOSE]})\\.` +
				`(${src[t.NUMERICIDENTIFIERLOOSE]})\\.` +
				`(${src[t.NUMERICIDENTIFIERLOOSE]})`,
		);

		// ## Pre-release Version Identifier
		// A numeric identifier, or a non-numeric identifier.
		// Non-numeric identifiers include numeric identifiers but can be longer.
		// Therefore non-numeric identifiers must go first.

		createToken("PRERELEASEIDENTIFIER", `(?:${src[t.NONNUMERICIDENTIFIER]}|${src[t.NUMERICIDENTIFIER]})`);

		createToken("PRERELEASEIDENTIFIERLOOSE", `(?:${src[t.NONNUMERICIDENTIFIER]}|${src[t.NUMERICIDENTIFIERLOOSE]})`);

		// ## Pre-release Version
		// Hyphen, followed by one or more dot-separated pre-release version
		// identifiers.

		createToken("PRERELEASE", `(?:-(${src[t.PRERELEASEIDENTIFIER]}(?:\\.${src[t.PRERELEASEIDENTIFIER]})*))`);

		createToken(
			"PRERELEASELOOSE",
			`(?:-?(${src[t.PRERELEASEIDENTIFIERLOOSE]}(?:\\.${src[t.PRERELEASEIDENTIFIERLOOSE]})*))`,
		);

		// ## Build Metadata Identifier
		// Any combination of digits, letters, or hyphens.

		createToken("BUILDIDENTIFIER", `${LETTERDASHNUMBER}+`);

		// ## Build Metadata
		// Plus sign, followed by one or more period-separated build metadata
		// identifiers.

		createToken("BUILD", `(?:\\+(${src[t.BUILDIDENTIFIER]}(?:\\.${src[t.BUILDIDENTIFIER]})*))`);

		// ## Full Version String
		// A main version, followed optionally by a pre-release version and
		// build metadata.

		// Note that the only major, minor, patch, and pre-release sections of
		// the version string are capturing groups.  The build metadata is not a
		// capturing group, because it should not ever be used in version
		// comparison.

		createToken("FULLPLAIN", `v?${src[t.MAINVERSION]}${src[t.PRERELEASE]}?${src[t.BUILD]}?`);

		createToken("FULL", `^${src[t.FULLPLAIN]}$`);

		// like full, but allows v1.2.3 and =1.2.3, which people do sometimes.
		// also, 1.0.0alpha1 (prerelease without the hyphen) which is pretty
		// common in the npm registry.
		createToken("LOOSEPLAIN", `[v=\\s]*${src[t.MAINVERSIONLOOSE]}${src[t.PRERELEASELOOSE]}?${src[t.BUILD]}?`);

		createToken("LOOSE", `^${src[t.LOOSEPLAIN]}$`);

		createToken("GTLT", "((?:<|>)?=?)");

		// Something like "2.*" or "1.2.x".
		// Note that "x.x" is a valid xRange identifier, meaning "any version"
		// Only the first item is strictly required.
		createToken("XRANGEIDENTIFIERLOOSE", `${src[t.NUMERICIDENTIFIERLOOSE]}|x|X|\\*`);
		createToken("XRANGEIDENTIFIER", `${src[t.NUMERICIDENTIFIER]}|x|X|\\*`);

		createToken(
			"XRANGEPLAIN",
			`[v=\\s]*(${src[t.XRANGEIDENTIFIER]})` +
				`(?:\\.(${src[t.XRANGEIDENTIFIER]})` +
				`(?:\\.(${src[t.XRANGEIDENTIFIER]})` +
				`(?:${src[t.PRERELEASE]})?${src[t.BUILD]}?` +
				`)?)?`,
		);

		createToken(
			"XRANGEPLAINLOOSE",
			`[v=\\s]*(${src[t.XRANGEIDENTIFIERLOOSE]})` +
				`(?:\\.(${src[t.XRANGEIDENTIFIERLOOSE]})` +
				`(?:\\.(${src[t.XRANGEIDENTIFIERLOOSE]})` +
				`(?:${src[t.PRERELEASELOOSE]})?${src[t.BUILD]}?` +
				`)?)?`,
		);

		createToken("XRANGE", `^${src[t.GTLT]}\\s*${src[t.XRANGEPLAIN]}$`);
		createToken("XRANGELOOSE", `^${src[t.GTLT]}\\s*${src[t.XRANGEPLAINLOOSE]}$`);

		// Coercion.
		// Extract anything that could conceivably be a part of a valid semver
		createToken(
			"COERCEPLAIN",
			`${"(^|[^\\d])" + "(\\d{1,"}${MAX_SAFE_COMPONENT_LENGTH}})` +
				`(?:\\.(\\d{1,${MAX_SAFE_COMPONENT_LENGTH}}))?` +
				`(?:\\.(\\d{1,${MAX_SAFE_COMPONENT_LENGTH}}))?`,
		);
		createToken("COERCE", `${src[t.COERCEPLAIN]}(?:$|[^\\d])`);
		createToken(
			"COERCEFULL",
			src[t.COERCEPLAIN] + `(?:${src[t.PRERELEASE]})?` + `(?:${src[t.BUILD]})?` + `(?:$|[^\\d])`,
		);
		createToken("COERCERTL", src[t.COERCE], true);
		createToken("COERCERTLFULL", src[t.COERCEFULL], true);

		// Tilde ranges.
		// Meaning is "reasonably at or greater than"
		createToken("LONETILDE", "(?:~>?)");

		createToken("TILDETRIM", `(\\s*)${src[t.LONETILDE]}\\s+`, true);
		exports.tildeTrimReplace = "$1~";

		createToken("TILDE", `^${src[t.LONETILDE]}${src[t.XRANGEPLAIN]}$`);
		createToken("TILDELOOSE", `^${src[t.LONETILDE]}${src[t.XRANGEPLAINLOOSE]}$`);

		// Caret ranges.
		// Meaning is "at least and backwards compatible with"
		createToken("LONECARET", "(?:\\^)");

		createToken("CARETTRIM", `(\\s*)${src[t.LONECARET]}\\s+`, true);
		exports.caretTrimReplace = "$1^";

		createToken("CARET", `^${src[t.LONECARET]}${src[t.XRANGEPLAIN]}$`);
		createToken("CARETLOOSE", `^${src[t.LONECARET]}${src[t.XRANGEPLAINLOOSE]}$`);

		// A simple gt/lt/eq thing, or just "" to indicate "any version"
		createToken("COMPARATORLOOSE", `^${src[t.GTLT]}\\s*(${src[t.LOOSEPLAIN]})$|^$`);
		createToken("COMPARATOR", `^${src[t.GTLT]}\\s*(${src[t.FULLPLAIN]})$|^$`);

		// An expression to strip any whitespace between the gtlt and the thing
		// it modifies, so that `> 1.2.3` ==> `>1.2.3`
		createToken("COMPARATORTRIM", `(\\s*)${src[t.GTLT]}\\s*(${src[t.LOOSEPLAIN]}|${src[t.XRANGEPLAIN]})`, true);
		exports.comparatorTrimReplace = "$1$2$3";

		// Something like `1.2.3 - 1.2.4`
		// Note that these all use the loose form, because they'll be
		// checked against either the strict or loose comparator form
		// later.
		createToken("HYPHENRANGE", `^\\s*(${src[t.XRANGEPLAIN]})` + `\\s+-\\s+` + `(${src[t.XRANGEPLAIN]})` + `\\s*$`);

		createToken(
			"HYPHENRANGELOOSE",
			`^\\s*(${src[t.XRANGEPLAINLOOSE]})` + `\\s+-\\s+` + `(${src[t.XRANGEPLAINLOOSE]})` + `\\s*$`,
		);

		// Star ranges basically just allow anything at all.
		createToken("STAR", "(<|>)?=?\\s*\\*");
		// >=0.0.0 is like a star
		createToken("GTE0", "^\\s*>=\\s*0\\.0\\.0\\s*$");
		createToken("GTE0PRE", "^\\s*>=\\s*0\\.0\\.0-0\\s*$");
	},
	"internal/constants": function (module, exports, require) {
		"use strict";

		// Note: this is the semver.org version of the spec that it implements
		// Not necessarily the package version of this code.
		const SEMVER_SPEC_VERSION = "2.0.0";

		const MAX_LENGTH = 256;
		const MAX_SAFE_INTEGER = Number.MAX_SAFE_INTEGER || /* istanbul ignore next */ 9007199254740991;

		// Max safe segment length for coercion.
		const MAX_SAFE_COMPONENT_LENGTH = 16;

		// Max safe length for a build identifier. The max length minus 6 characters for
		// the shortest version with a build 0.0.0+BUILD.
		const MAX_SAFE_BUILD_LENGTH = MAX_LENGTH - 6;

		const RELEASE_TYPES = ["major", "premajor", "minor", "preminor", "patch", "prepatch", "prerelease"];

		module.exports = {
			MAX_LENGTH,
			MAX_SAFE_COMPONENT_LENGTH,
			MAX_SAFE_BUILD_LENGTH,
			MAX_SAFE_INTEGER,
			RELEASE_TYPES,
			SEMVER_SPEC_VERSION,
			FLAG_INCLUDE_PRERELEASE: 0b001,
			FLAG_LOOSE: 0b010,
		};
	},
	"internal/debug": function (module, exports, require) {
		"use strict";

		const debug =
			typeof process === "object" && process.env && process.env.NODE_DEBUG && /\bsemver\b/i.test(process.env.NODE_DEBUG)
				? (...args) => console.error("SEMVER", ...args)
				: () => {};

		module.exports = debug;
	},
	"functions/cmp": function (module, exports, require) {
		"use strict";

		const eq = require("functions/eq");
		const neq = require("functions/neq");
		const gt = require("functions/gt");
		const gte = require("functions/gte");
		const lt = require("functions/lt");
		const lte = require("functions/lte");

		const cmp = (a, op, b, loose) => {
			switch (op) {
				case "===":
					if (typeof a === "object") {
						a = a.version;
					}
					if (typeof b === "object") {
						b = b.version;
					}
					return a === b;

				case "!==":
					if (typeof a === "object") {
						a = a.version;
					}
					if (typeof b === "object") {
						b = b.version;
					}
					return a !== b;

				case "":
				case "=":
				case "==":
					return eq(a, b, loose);

				case "!=":
					return neq(a, b, loose);

				case ">":
					return gt(a, b, loose);

				case ">=":
					return gte(a, b, loose);

				case "<":
					return lt(a, b, loose);

				case "<=":
					return lte(a, b, loose);

				default:
					throw new TypeError(`Invalid operator: ${op}`);
			}
		};
		module.exports = cmp;
	},
	"functions/eq": function (module, exports, require) {
		"use strict";

		const compare = require("functions/compare");
		const eq = (a, b, loose) => compare(a, b, loose) === 0;
		module.exports = eq;
	},
	"functions/compare": function (module, exports, require) {
		"use strict";

		const SemVer = require("classes/semver");
		const compare = (a, b, loose) => new SemVer(a, loose).compare(new SemVer(b, loose));

		module.exports = compare;
	},
	"classes/semver": function (module, exports, require) {
		"use strict";

		const debug = require("internal/debug");
		const { MAX_LENGTH, MAX_SAFE_INTEGER } = require("internal/constants");
		const { safeRe: re, t } = require("internal/re");

		const parseOptions = require("internal/parse-options");
		const { compareIdentifiers } = require("internal/identifiers");

		const isPrereleaseIdentifier = (prerelease, identifier) => {
			const identifiers = identifier.split(".");
			if (identifiers.length > prerelease.length) {
				return false;
			}

			for (let i = 0; i < identifiers.length; i++) {
				if (compareIdentifiers(prerelease[i], identifiers[i]) !== 0) {
					return false;
				}
			}

			return true;
		};

		class SemVer {
			constructor(version, options) {
				options = parseOptions(options);

				if (version instanceof SemVer) {
					if (version.loose === !!options.loose && version.includePrerelease === !!options.includePrerelease) {
						return version;
					} else {
						version = version.version;
					}
				} else if (typeof version !== "string") {
					throw new TypeError(`Invalid version. Must be a string. Got type "${typeof version}".`);
				}

				if (version.length > MAX_LENGTH) {
					throw new TypeError(`version is longer than ${MAX_LENGTH} characters`);
				}

				debug("SemVer", version, options);
				this.options = options;
				this.loose = !!options.loose;
				// this isn't actually relevant for versions, but keep it so that we
				// don't run into trouble passing this.options around.
				this.includePrerelease = !!options.includePrerelease;

				const m = version.trim().match(options.loose ? re[t.LOOSE] : re[t.FULL]);

				if (!m) {
					throw new TypeError(`Invalid Version: ${version}`);
				}

				this.raw = version;

				// these are actually numbers
				this.major = +m[1];
				this.minor = +m[2];
				this.patch = +m[3];

				if (this.major > MAX_SAFE_INTEGER || this.major < 0) {
					throw new TypeError("Invalid major version");
				}

				if (this.minor > MAX_SAFE_INTEGER || this.minor < 0) {
					throw new TypeError("Invalid minor version");
				}

				if (this.patch > MAX_SAFE_INTEGER || this.patch < 0) {
					throw new TypeError("Invalid patch version");
				}

				// numberify any prerelease numeric ids
				if (!m[4]) {
					this.prerelease = [];
				} else {
					this.prerelease = m[4].split(".").map((id) => {
						if (/^[0-9]+$/.test(id)) {
							const num = +id;
							if (num >= 0 && num < MAX_SAFE_INTEGER) {
								return num;
							}
						}
						return id;
					});
				}

				this.build = m[5] ? m[5].split(".") : [];
				this.format();
			}

			format() {
				this.version = `${this.major}.${this.minor}.${this.patch}`;
				if (this.prerelease.length) {
					this.version += `-${this.prerelease.join(".")}`;
				}
				return this.version;
			}

			toString() {
				return this.version;
			}

			compare(other) {
				debug("SemVer.compare", this.version, this.options, other);
				if (!(other instanceof SemVer)) {
					if (typeof other === "string" && other === this.version) {
						return 0;
					}
					other = new SemVer(other, this.options);
				}

				if (other.version === this.version) {
					return 0;
				}

				return this.compareMain(other) || this.comparePre(other);
			}

			compareMain(other) {
				if (!(other instanceof SemVer)) {
					other = new SemVer(other, this.options);
				}

				if (this.major < other.major) {
					return -1;
				}
				if (this.major > other.major) {
					return 1;
				}
				if (this.minor < other.minor) {
					return -1;
				}
				if (this.minor > other.minor) {
					return 1;
				}
				if (this.patch < other.patch) {
					return -1;
				}
				if (this.patch > other.patch) {
					return 1;
				}
				return 0;
			}

			comparePre(other) {
				if (!(other instanceof SemVer)) {
					other = new SemVer(other, this.options);
				}

				// NOT having a prerelease is > having one
				if (this.prerelease.length && !other.prerelease.length) {
					return -1;
				} else if (!this.prerelease.length && other.prerelease.length) {
					return 1;
				} else if (!this.prerelease.length && !other.prerelease.length) {
					return 0;
				}

				let i = 0;
				do {
					const a = this.prerelease[i];
					const b = other.prerelease[i];
					debug("prerelease compare", i, a, b);
					if (a === undefined && b === undefined) {
						return 0;
					} else if (b === undefined) {
						return 1;
					} else if (a === undefined) {
						return -1;
					} else if (a === b) {
						continue;
					} else {
						return compareIdentifiers(a, b);
					}
				} while (++i);
			}

			compareBuild(other) {
				if (!(other instanceof SemVer)) {
					other = new SemVer(other, this.options);
				}

				let i = 0;
				do {
					const a = this.build[i];
					const b = other.build[i];
					debug("build compare", i, a, b);
					if (a === undefined && b === undefined) {
						return 0;
					} else if (b === undefined) {
						return 1;
					} else if (a === undefined) {
						return -1;
					} else if (a === b) {
						continue;
					} else {
						return compareIdentifiers(a, b);
					}
				} while (++i);
			}

			// preminor will bump the version up to the next minor release, and immediately
			// down to pre-release. premajor and prepatch work the same way.
			inc(release, identifier, identifierBase) {
				if (release.startsWith("pre")) {
					if (!identifier && identifierBase === false) {
						throw new Error("invalid increment argument: identifier is empty");
					}
					// Avoid an invalid semver results
					if (identifier) {
						const match = `-${identifier}`.match(this.options.loose ? re[t.PRERELEASELOOSE] : re[t.PRERELEASE]);
						if (!match || match[1] !== identifier) {
							throw new Error(`invalid identifier: ${identifier}`);
						}
					}
				}

				switch (release) {
					case "premajor":
						this.prerelease.length = 0;
						this.patch = 0;
						this.minor = 0;
						this.major++;
						this.inc("pre", identifier, identifierBase);
						break;
					case "preminor":
						this.prerelease.length = 0;
						this.patch = 0;
						this.minor++;
						this.inc("pre", identifier, identifierBase);
						break;
					case "prepatch":
						// If this is already a prerelease, it will bump to the next version
						// drop any prereleases that might already exist, since they are not
						// relevant at this point.
						this.prerelease.length = 0;
						this.inc("patch", identifier, identifierBase);
						this.inc("pre", identifier, identifierBase);
						break;
					// If the input is a non-prerelease version, this acts the same as
					// prepatch.
					case "prerelease":
						if (this.prerelease.length === 0) {
							this.inc("patch", identifier, identifierBase);
						}
						this.inc("pre", identifier, identifierBase);
						break;
					case "release":
						if (this.prerelease.length === 0) {
							throw new Error(`version ${this.raw} is not a prerelease`);
						}
						this.prerelease.length = 0;
						break;

					case "major":
						// If this is a pre-major version, bump up to the same major version.
						// Otherwise increment major.
						// 1.0.0-5 bumps to 1.0.0
						// 1.1.0 bumps to 2.0.0
						if (this.minor !== 0 || this.patch !== 0 || this.prerelease.length === 0) {
							this.major++;
						}
						this.minor = 0;
						this.patch = 0;
						this.prerelease = [];
						break;
					case "minor":
						// If this is a pre-minor version, bump up to the same minor version.
						// Otherwise increment minor.
						// 1.2.0-5 bumps to 1.2.0
						// 1.2.1 bumps to 1.3.0
						if (this.patch !== 0 || this.prerelease.length === 0) {
							this.minor++;
						}
						this.patch = 0;
						this.prerelease = [];
						break;
					case "patch":
						// If this is not a pre-release version, it will increment the patch.
						// If it is a pre-release it will bump up to the same patch version.
						// 1.2.0-5 patches to 1.2.0
						// 1.2.0 patches to 1.2.1
						if (this.prerelease.length === 0) {
							this.patch++;
						}
						this.prerelease = [];
						break;
					// This probably shouldn't be used publicly.
					// 1.0.0 'pre' would become 1.0.0-0 which is the wrong direction.
					case "pre": {
						const base = Number(identifierBase) ? 1 : 0;

						if (this.prerelease.length === 0) {
							this.prerelease = [base];
						} else {
							let i = this.prerelease.length;
							while (--i >= 0) {
								if (typeof this.prerelease[i] === "number") {
									this.prerelease[i]++;
									i = -2;
								}
							}
							if (i === -1) {
								// didn't increment anything
								if (identifier === this.prerelease.join(".") && identifierBase === false) {
									throw new Error("invalid increment argument: identifier already exists");
								}
								this.prerelease.push(base);
							}
						}
						if (identifier) {
							// 1.2.0-beta.1 bumps to 1.2.0-beta.2,
							// 1.2.0-beta.fooblz or 1.2.0-beta bumps to 1.2.0-beta.0
							let prerelease = [identifier, base];
							if (identifierBase === false) {
								prerelease = [identifier];
							}
							if (isPrereleaseIdentifier(this.prerelease, identifier)) {
								const prereleaseBase = this.prerelease[identifier.split(".").length];
								if (isNaN(prereleaseBase)) {
									this.prerelease = prerelease;
								}
							} else {
								this.prerelease = prerelease;
							}
						}
						break;
					}
					default:
						throw new Error(`invalid increment argument: ${release}`);
				}
				this.raw = this.format();
				if (this.build.length) {
					this.raw += `+${this.build.join(".")}`;
				}
				return this;
			}
		}

		module.exports = SemVer;
	},
	"internal/identifiers": function (module, exports, require) {
		"use strict";

		const numeric = /^[0-9]+$/;
		const compareIdentifiers = (a, b) => {
			if (typeof a === "number" && typeof b === "number") {
				return a === b ? 0 : a < b ? -1 : 1;
			}

			const anum = numeric.test(a);
			const bnum = numeric.test(b);

			if (anum && bnum) {
				a = +a;
				b = +b;
			}

			return a === b ? 0 : anum && !bnum ? -1 : bnum && !anum ? 1 : a < b ? -1 : 1;
		};

		const rcompareIdentifiers = (a, b) => compareIdentifiers(b, a);

		module.exports = {
			compareIdentifiers,
			rcompareIdentifiers,
		};
	},
	"functions/neq": function (module, exports, require) {
		"use strict";

		const compare = require("functions/compare");
		const neq = (a, b, loose) => compare(a, b, loose) !== 0;
		module.exports = neq;
	},
	"functions/gt": function (module, exports, require) {
		"use strict";

		const compare = require("functions/compare");
		const gt = (a, b, loose) => compare(a, b, loose) > 0;
		module.exports = gt;
	},
	"functions/gte": function (module, exports, require) {
		"use strict";

		const compare = require("functions/compare");
		const gte = (a, b, loose) => compare(a, b, loose) >= 0;
		module.exports = gte;
	},
	"functions/lt": function (module, exports, require) {
		"use strict";

		const compare = require("functions/compare");
		const lt = (a, b, loose) => compare(a, b, loose) < 0;
		module.exports = lt;
	},
	"functions/lte": function (module, exports, require) {
		"use strict";

		const compare = require("functions/compare");
		const lte = (a, b, loose) => compare(a, b, loose) <= 0;
		module.exports = lte;
	},
};
const cache: Record<string, CommonJSModule> = Object.create(null);
function requireModule(id: string): unknown {
	if (cache[id]) return cache[id].exports;
	const module: CommonJSModule = { exports: {} };
	cache[id] = module;
	factories[id](module, module.exports as Record<string, unknown>, requireModule);
	return module.exports;
}
export function satisfies(version: string, range: string): boolean {
	return (requireModule("functions/satisfies") as (v: string, r: string) => boolean)(version, range);
}
export function compare(a: string, b: string): -1 | 0 | 1 {
	return (requireModule("functions/compare") as (a: string, b: string) => -1 | 0 | 1)(a, b);
}
