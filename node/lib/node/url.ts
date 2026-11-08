/* WHATWG URL parsing adapted from whatwg-url v14.2.0.
Legacy URL resolution adapted from Node.js.
Copyright Joyent, Inc. and other Node contributors.
The MIT License (MIT)

Copyright (c) Sebastian Mayr

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*/
import { toASCII, toUnicode } from "node:punycode";
import * as core from "qbun:core";
import { codedError, invalidArgType } from "internal:errors";
import { posix, win32 } from "node:path";
// module-level instances: node:url can load during the bootstrap, before
// the TextEncoder/TextDecoder globals exist
import { TextDecoder, TextEncoder } from "internal:encoding";
interface URLRecord {
	scheme: string;
	username: string;
	password: string;
	host: string | number | number[] | null;
	port: number | null;
	path: string | string[];
	query: string | null;
	fragment: string | null;
}
interface ParseOptions {
	baseURL?: URLRecord | null;
	encodingOverride?: string;
	url?: URLRecord;
	stateOverride?: string;
}
const utf8Encoder = new TextEncoder();
const utf8Decoder = new TextDecoder("utf-8", { ignoreBOM: true });
function utf8Encode(value: string): Uint8Array {
	return utf8Encoder.encode(value);
}
function utf8DecodeWithoutBOM(value: Uint8Array): string {
	return utf8Decoder.decode(value);
}

const infra = (() => {
	// Note that we take code points as JS numbers, not JS strings.

	function isASCIIDigit(c) {
		return c >= 0x30 && c <= 0x39;
	}

	function isASCIIAlpha(c) {
		return (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a);
	}

	function isASCIIAlphanumeric(c) {
		return isASCIIAlpha(c) || isASCIIDigit(c);
	}

	function isASCIIHex(c) {
		return isASCIIDigit(c) || (c >= 0x41 && c <= 0x46) || (c >= 0x61 && c <= 0x66);
	}

	return {
		isASCIIDigit,
		isASCIIAlpha,
		isASCIIAlphanumeric,
		isASCIIHex,
	};
})();
const percent = (() => {
	const { isASCIIHex } = infra;

	function p(char) {
		return char.codePointAt(0);
	}

	// https://url.spec.whatwg.org/#percent-encode
	function percentEncode(c) {
		let hex = c.toString(16).toUpperCase();
		if (hex.length === 1) {
			hex = `0${hex}`;
		}

		return `%${hex}`;
	}

	// https://url.spec.whatwg.org/#percent-decode
	function percentDecodeBytes(input) {
		const output = new Uint8Array(input.byteLength);
		let outputIndex = 0;
		for (let i = 0; i < input.byteLength; ++i) {
			const byte = input[i];
			if (byte !== 0x25) {
				output[outputIndex++] = byte;
			} else if (byte === 0x25 && (!isASCIIHex(input[i + 1]) || !isASCIIHex(input[i + 2]))) {
				output[outputIndex++] = byte;
			} else {
				const bytePoint = parseInt(String.fromCodePoint(input[i + 1], input[i + 2]), 16);
				output[outputIndex++] = bytePoint;
				i += 2;
			}
		}

		return output.slice(0, outputIndex);
	}

	// https://url.spec.whatwg.org/#string-percent-decode
	function percentDecodeString(input) {
		const bytes = utf8Encode(input);
		return percentDecodeBytes(bytes);
	}

	// https://url.spec.whatwg.org/#c0-control-percent-encode-set
	function isC0ControlPercentEncode(c) {
		return c <= 0x1f || c > 0x7e;
	}

	// https://url.spec.whatwg.org/#fragment-percent-encode-set
	const extraFragmentPercentEncodeSet = new Set([p(" "), p('"'), p("<"), p(">"), p("`")]);
	function isFragmentPercentEncode(c) {
		return isC0ControlPercentEncode(c) || extraFragmentPercentEncodeSet.has(c);
	}

	// https://url.spec.whatwg.org/#query-percent-encode-set
	const extraQueryPercentEncodeSet = new Set([p(" "), p('"'), p("#"), p("<"), p(">")]);
	function isQueryPercentEncode(c) {
		return isC0ControlPercentEncode(c) || extraQueryPercentEncodeSet.has(c);
	}

	// https://url.spec.whatwg.org/#special-query-percent-encode-set
	function isSpecialQueryPercentEncode(c) {
		return isQueryPercentEncode(c) || c === p("'");
	}

	// https://url.spec.whatwg.org/#path-percent-encode-set
	const extraPathPercentEncodeSet = new Set([p("?"), p("`"), p("{"), p("}"), p("^")]);
	function isPathPercentEncode(c) {
		return isQueryPercentEncode(c) || extraPathPercentEncodeSet.has(c);
	}

	// https://url.spec.whatwg.org/#userinfo-percent-encode-set
	const extraUserinfoPercentEncodeSet = new Set([
		p("/"),
		p(":"),
		p(";"),
		p("="),
		p("@"),
		p("["),
		p("\\"),
		p("]"),
		p("|"),
	]);
	function isUserinfoPercentEncode(c) {
		return isPathPercentEncode(c) || extraUserinfoPercentEncodeSet.has(c);
	}

	// https://url.spec.whatwg.org/#component-percent-encode-set
	const extraComponentPercentEncodeSet = new Set([p("$"), p("%"), p("&"), p("+"), p(",")]);
	function isComponentPercentEncode(c) {
		return isUserinfoPercentEncode(c) || extraComponentPercentEncodeSet.has(c);
	}

	// https://url.spec.whatwg.org/#application-x-www-form-urlencoded-percent-encode-set
	const extraURLEncodedPercentEncodeSet = new Set([p("!"), p("'"), p("("), p(")"), p("~")]);
	function isURLEncodedPercentEncode(c) {
		return isComponentPercentEncode(c) || extraURLEncodedPercentEncodeSet.has(c);
	}

	// https://url.spec.whatwg.org/#code-point-percent-encode-after-encoding
	// https://url.spec.whatwg.org/#utf-8-percent-encode
	// The "-Internal" variant here has code points as JS strings. The external version used by other files has code points
	// as JS numbers, like the rest of the codebase.
	function utf8PercentEncodeCodePointInternal(codePoint, percentEncodePredicate) {
		const bytes = utf8Encode(codePoint);
		let output = "";
		for (const byte of bytes) {
			// Our percentEncodePredicate operates on bytes, not code points, so this is slightly different from the spec.
			if (!percentEncodePredicate(byte)) {
				output += String.fromCharCode(byte);
			} else {
				output += percentEncode(byte);
			}
		}

		return output;
	}

	function utf8PercentEncodeCodePoint(codePoint, percentEncodePredicate) {
		return utf8PercentEncodeCodePointInternal(String.fromCodePoint(codePoint), percentEncodePredicate);
	}

	// https://url.spec.whatwg.org/#string-percent-encode-after-encoding
	// https://url.spec.whatwg.org/#string-utf-8-percent-encode
	function utf8PercentEncodeString(input, percentEncodePredicate, spaceAsPlus = false) {
		let output = "";
		for (const codePoint of input) {
			if (spaceAsPlus && codePoint === " ") {
				output += "+";
			} else {
				output += utf8PercentEncodeCodePointInternal(codePoint, percentEncodePredicate);
			}
		}
		return output;
	}

	return {
		isC0ControlPercentEncode,
		isFragmentPercentEncode,
		isQueryPercentEncode,
		isSpecialQueryPercentEncode,
		isPathPercentEncode,
		isUserinfoPercentEncode,
		isURLEncodedPercentEncode,
		percentDecodeString,
		percentDecodeBytes,
		utf8PercentEncodeString,
		utf8PercentEncodeCodePoint,
	};
})();
const parser = (() => {
	const parser = {} as {
		serializeURL(url: URLRecord, excludeFragment?: boolean): string;
		serializePath(url: URLRecord): string;
		serializeURLOrigin(url: URLRecord): string;
		basicURLParse(input: string, options?: ParseOptions): URLRecord | null;
		setTheUsername(url: URLRecord, name: string): void;
		setThePassword(url: URLRecord, password: string): void;
		serializeHost(host: URLRecord["host"]): string;
		cannotHaveAUsernamePasswordPort(url: URLRecord): boolean;
		hasAnOpaquePath(url: URLRecord): boolean;
		serializeInteger(value: number): string;
		parseURL(input: string, options?: ParseOptions): URLRecord | null;
	};

	const {
		percentDecodeString,
		utf8PercentEncodeCodePoint,
		utf8PercentEncodeString,
		isC0ControlPercentEncode,
		isFragmentPercentEncode,
		isQueryPercentEncode,
		isSpecialQueryPercentEncode,
		isPathPercentEncode,
		isUserinfoPercentEncode,
	} = percent;

	function p(char) {
		return char.codePointAt(0);
	}

	const specialSchemes = {
		ftp: 21,
		file: null,
		http: 80,
		https: 443,
		ws: 80,
		wss: 443,
	};

	const failure = Symbol("failure");

	function countSymbols(str) {
		return [...str].length;
	}

	function at(input, idx) {
		const c = input[idx];
		return isNaN(c) ? undefined : String.fromCodePoint(c);
	}

	function isSingleDot(buffer) {
		return buffer === "." || buffer.toLowerCase() === "%2e";
	}

	function isDoubleDot(buffer) {
		buffer = buffer.toLowerCase();
		return buffer === ".." || buffer === "%2e." || buffer === ".%2e" || buffer === "%2e%2e";
	}

	function isWindowsDriveLetterCodePoints(cp1, cp2) {
		return infra.isASCIIAlpha(cp1) && (cp2 === p(":") || cp2 === p("|"));
	}

	function isWindowsDriveLetterString(string) {
		return string.length === 2 && infra.isASCIIAlpha(string.codePointAt(0)) && (string[1] === ":" || string[1] === "|");
	}

	function isNormalizedWindowsDriveLetterString(string) {
		return string.length === 2 && infra.isASCIIAlpha(string.codePointAt(0)) && string[1] === ":";
	}

	function containsForbiddenHostCodePoint(string) {
		return string.search(/\u0000|\u0009|\u000A|\u000D|\u0020|#|\/|:|<|>|\?|@|\[|\\|\]|\^|\|/u) !== -1;
	}

	function containsForbiddenDomainCodePoint(string) {
		return containsForbiddenHostCodePoint(string) || string.search(/[\u0000-\u001F]|%|\u007F/u) !== -1;
	}

	function isSpecialScheme(scheme: string): boolean {
		return Object.prototype.hasOwnProperty.call(specialSchemes, scheme);
	}

	function isSpecial(url) {
		return isSpecialScheme(url.scheme);
	}

	function isNotSpecial(url) {
		return !isSpecialScheme(url.scheme);
	}

	function defaultPort(scheme) {
		return specialSchemes[scheme];
	}

	function parseIPv4Number(input) {
		if (input === "") {
			return failure;
		}

		let R = 10;

		if (input.length >= 2 && input.charAt(0) === "0" && input.charAt(1).toLowerCase() === "x") {
			input = input.substring(2);
			R = 16;
		} else if (input.length >= 2 && input.charAt(0) === "0") {
			input = input.substring(1);
			R = 8;
		}

		if (input === "") {
			return 0;
		}

		let regex = /[^0-7]/u;
		if (R === 10) {
			regex = /[^0-9]/u;
		}
		if (R === 16) {
			regex = /[^0-9A-Fa-f]/u;
		}

		if (regex.test(input)) {
			return failure;
		}

		return parseInt(input, R);
	}

	function parseIPv4(input) {
		const parts = input.split(".");
		if (parts[parts.length - 1] === "") {
			if (parts.length > 1) {
				parts.pop();
			}
		}

		if (parts.length > 4) {
			return failure;
		}

		const numbers = [];
		for (const part of parts) {
			const n = parseIPv4Number(part);
			if (n === failure) {
				return failure;
			}

			numbers.push(n);
		}

		for (let i = 0; i < numbers.length - 1; ++i) {
			if (numbers[i] > 255) {
				return failure;
			}
		}
		if (numbers[numbers.length - 1] >= 256 ** (5 - numbers.length)) {
			return failure;
		}

		let ipv4 = numbers.pop();
		let counter = 0;

		for (const n of numbers) {
			ipv4 += n * 256 ** (3 - counter);
			++counter;
		}

		return ipv4;
	}

	function serializeIPv4(address) {
		let output = "";
		let n = address;

		for (let i = 1; i <= 4; ++i) {
			output = String(n % 256) + output;
			if (i !== 4) {
				output = `.${output}`;
			}
			n = Math.floor(n / 256);
		}

		return output;
	}

	function parseIPv6(input) {
		const address = [0, 0, 0, 0, 0, 0, 0, 0];
		let pieceIndex = 0;
		let compress = null;
		let pointer = 0;

		input = Array.from(input, (c) => c.codePointAt(0));

		if (input[pointer] === p(":")) {
			if (input[pointer + 1] !== p(":")) {
				return failure;
			}

			pointer += 2;
			++pieceIndex;
			compress = pieceIndex;
		}

		while (pointer < input.length) {
			if (pieceIndex === 8) {
				return failure;
			}

			if (input[pointer] === p(":")) {
				if (compress !== null) {
					return failure;
				}
				++pointer;
				++pieceIndex;
				compress = pieceIndex;
				continue;
			}

			let value = 0;
			let length = 0;

			while (length < 4 && infra.isASCIIHex(input[pointer])) {
				value = value * 0x10 + parseInt(at(input, pointer), 16);
				++pointer;
				++length;
			}

			if (input[pointer] === p(".")) {
				if (length === 0) {
					return failure;
				}

				pointer -= length;

				if (pieceIndex > 6) {
					return failure;
				}

				let numbersSeen = 0;

				while (input[pointer] !== undefined) {
					let ipv4Piece = null;

					if (numbersSeen > 0) {
						if (input[pointer] === p(".") && numbersSeen < 4) {
							++pointer;
						} else {
							return failure;
						}
					}

					if (!infra.isASCIIDigit(input[pointer])) {
						return failure;
					}

					while (infra.isASCIIDigit(input[pointer])) {
						const number = parseInt(at(input, pointer));
						if (ipv4Piece === null) {
							ipv4Piece = number;
						} else if (ipv4Piece === 0) {
							return failure;
						} else {
							ipv4Piece = ipv4Piece * 10 + number;
						}
						if (ipv4Piece > 255) {
							return failure;
						}
						++pointer;
					}

					address[pieceIndex] = address[pieceIndex] * 0x100 + ipv4Piece;

					++numbersSeen;

					if (numbersSeen === 2 || numbersSeen === 4) {
						++pieceIndex;
					}
				}

				if (numbersSeen !== 4) {
					return failure;
				}

				break;
			} else if (input[pointer] === p(":")) {
				++pointer;
				if (input[pointer] === undefined) {
					return failure;
				}
			} else if (input[pointer] !== undefined) {
				return failure;
			}

			address[pieceIndex] = value;
			++pieceIndex;
		}

		if (compress !== null) {
			let swaps = pieceIndex - compress;
			pieceIndex = 7;
			while (pieceIndex !== 0 && swaps > 0) {
				const temp = address[compress + swaps - 1];
				address[compress + swaps - 1] = address[pieceIndex];
				address[pieceIndex] = temp;
				--pieceIndex;
				--swaps;
			}
		} else if (compress === null && pieceIndex !== 8) {
			return failure;
		}

		return address;
	}

	function serializeIPv6(address) {
		let output = "";
		const compress = findTheIPv6AddressCompressedPieceIndex(address);
		let ignore0 = false;

		for (let pieceIndex = 0; pieceIndex <= 7; ++pieceIndex) {
			if (ignore0 && address[pieceIndex] === 0) {
				continue;
			} else if (ignore0) {
				ignore0 = false;
			}

			if (compress === pieceIndex) {
				const separator = pieceIndex === 0 ? "::" : ":";
				output += separator;
				ignore0 = true;
				continue;
			}

			output += address[pieceIndex].toString(16);

			if (pieceIndex !== 7) {
				output += ":";
			}
		}

		return output;
	}

	function parseHost(input, isOpaque = false) {
		if (input[0] === "[") {
			if (input[input.length - 1] !== "]") {
				return failure;
			}

			return parseIPv6(input.substring(1, input.length - 1));
		}

		if (isOpaque) {
			return parseOpaqueHost(input);
		}

		const domain = utf8DecodeWithoutBOM(percentDecodeString(input));
		const asciiDomain = domainToASCII(domain);
		if (asciiDomain === failure) {
			return failure;
		}

		if (endsInANumber(asciiDomain)) {
			return parseIPv4(asciiDomain);
		}

		return asciiDomain;
	}

	function endsInANumber(input) {
		const parts = input.split(".");
		if (parts[parts.length - 1] === "") {
			if (parts.length === 1) {
				return false;
			}
			parts.pop();
		}

		const last = parts[parts.length - 1];
		if (parseIPv4Number(last) !== failure) {
			return true;
		}

		if (/^[0-9]+$/u.test(last)) {
			return true;
		}

		return false;
	}

	function parseOpaqueHost(input) {
		if (containsForbiddenHostCodePoint(input)) {
			return failure;
		}

		return utf8PercentEncodeString(input, isC0ControlPercentEncode);
	}

	function findTheIPv6AddressCompressedPieceIndex(address) {
		let longestIndex = null;
		let longestSize = 1; // only find elements > 1
		let foundIndex = null;
		let foundSize = 0;

		for (let pieceIndex = 0; pieceIndex < address.length; ++pieceIndex) {
			if (address[pieceIndex] !== 0) {
				if (foundSize > longestSize) {
					longestIndex = foundIndex;
					longestSize = foundSize;
				}

				foundIndex = null;
				foundSize = 0;
			} else {
				if (foundIndex === null) {
					foundIndex = pieceIndex;
				}
				++foundSize;
			}
		}

		if (foundSize > longestSize) {
			return foundIndex;
		}

		return longestIndex;
	}

	function serializeHost(host) {
		if (typeof host === "number") {
			return serializeIPv4(host);
		}

		// IPv6 serializer
		if (host instanceof Array) {
			return `[${serializeIPv6(host)}]`;
		}

		return host;
	}

	function domainToASCII(domain: string): string | typeof failure {
		try {
			const mapped = domain
				.normalize("NFKC")
				.toLowerCase()
				.replace(/[\u3002\uFF0E\uFF61]/g, ".");
			if (/[\u0000-\u0020\u007F-\u009F\uD800-\uDFFF]/u.test(mapped)) return failure;
			const result = toASCII(mapped).toLowerCase();
			if (!result || containsForbiddenDomainCodePoint(result)) return failure;
			for (const label of result.split(".")) {
				if (label.startsWith("xn--")) {
					const decoded = toUnicode(label);
					if (decoded === label || toASCII(decoded).toLowerCase() !== label) return failure;
				}
			}
			return result;
		} catch {
			return failure;
		}
	}

	function trimControlChars(string) {
		// Avoid using regexp because of this V8 bug: https://issues.chromium.org/issues/42204424

		let start = 0;
		let end = string.length;
		for (; start < end; ++start) {
			if (string.charCodeAt(start) > 0x20) {
				break;
			}
		}
		for (; end > start; --end) {
			if (string.charCodeAt(end - 1) > 0x20) {
				break;
			}
		}
		return string.substring(start, end);
	}

	function trimTabAndNewline(url) {
		return url.replace(/\u0009|\u000A|\u000D/gu, "");
	}

	function shortenPath(url) {
		const { path } = url;
		if (path.length === 0) {
			return;
		}
		if (url.scheme === "file" && path.length === 1 && isNormalizedWindowsDriveLetter(path[0])) {
			return;
		}

		path.pop();
	}

	function includesCredentials(url) {
		return url.username !== "" || url.password !== "";
	}

	function cannotHaveAUsernamePasswordPort(url) {
		return url.host === null || url.host === "" || url.scheme === "file";
	}

	function hasAnOpaquePath(url) {
		return typeof url.path === "string";
	}

	function isNormalizedWindowsDriveLetter(string) {
		return /^[A-Za-z]:$/u.test(string);
	}

	function URLStateMachine(input, base, encodingOverride, url, stateOverride) {
		this.pointer = 0;
		this.input = input;
		this.base = base || null;
		this.encodingOverride = encodingOverride || "utf-8";
		this.stateOverride = stateOverride;
		this.url = url;
		this.failure = false;
		this.parseError = false;

		if (!this.url) {
			this.url = {
				scheme: "",
				username: "",
				password: "",
				host: null,
				port: null,
				path: [],
				query: null,
				fragment: null,
			};

			const res = trimControlChars(this.input);
			if (res !== this.input) {
				this.parseError = true;
			}
			this.input = res;
		}

		const res = trimTabAndNewline(this.input);
		if (res !== this.input) {
			this.parseError = true;
		}
		this.input = res;

		this.state = stateOverride || "scheme start";

		this.buffer = "";
		this.atFlag = false;
		this.arrFlag = false;
		this.passwordTokenSeenFlag = false;

		this.input = Array.from(this.input, (c) => c.codePointAt(0));

		for (; this.pointer <= this.input.length; ++this.pointer) {
			const c = this.input[this.pointer];
			const cStr = isNaN(c) ? undefined : String.fromCodePoint(c);

			// exec state machine
			const ret = this[`parse ${this.state}`](c, cStr);
			if (!ret) {
				break; // terminate algorithm
			} else if (ret === failure) {
				this.failure = true;
				break;
			}
		}
	}

	URLStateMachine.prototype["parse scheme start"] = function parseSchemeStart(c, cStr) {
		if (infra.isASCIIAlpha(c)) {
			this.buffer += cStr.toLowerCase();
			this.state = "scheme";
		} else if (!this.stateOverride) {
			this.state = "no scheme";
			--this.pointer;
		} else {
			this.parseError = true;
			return failure;
		}

		return true;
	};

	URLStateMachine.prototype["parse scheme"] = function parseScheme(c, cStr) {
		if (infra.isASCIIAlphanumeric(c) || c === p("+") || c === p("-") || c === p(".")) {
			this.buffer += cStr.toLowerCase();
		} else if (c === p(":")) {
			if (this.stateOverride) {
				if (isSpecial(this.url) && !isSpecialScheme(this.buffer)) {
					return false;
				}

				if (!isSpecial(this.url) && isSpecialScheme(this.buffer)) {
					return false;
				}

				if ((includesCredentials(this.url) || this.url.port !== null) && this.buffer === "file") {
					return false;
				}

				if (this.url.scheme === "file" && this.url.host === "") {
					return false;
				}
			}
			this.url.scheme = this.buffer;
			if (this.stateOverride) {
				if (this.url.port === defaultPort(this.url.scheme)) {
					this.url.port = null;
				}
				return false;
			}
			this.buffer = "";
			if (this.url.scheme === "file") {
				if (this.input[this.pointer + 1] !== p("/") || this.input[this.pointer + 2] !== p("/")) {
					this.parseError = true;
				}
				this.state = "file";
			} else if (isSpecial(this.url) && this.base !== null && this.base.scheme === this.url.scheme) {
				this.state = "special relative or authority";
			} else if (isSpecial(this.url)) {
				this.state = "special authority slashes";
			} else if (this.input[this.pointer + 1] === p("/")) {
				this.state = "path or authority";
				++this.pointer;
			} else {
				this.url.path = "";
				this.state = "opaque path";
			}
		} else if (!this.stateOverride) {
			this.buffer = "";
			this.state = "no scheme";
			this.pointer = -1;
		} else {
			this.parseError = true;
			return failure;
		}

		return true;
	};

	URLStateMachine.prototype["parse no scheme"] = function parseNoScheme(c) {
		if (this.base === null || (hasAnOpaquePath(this.base) && c !== p("#"))) {
			return failure;
		} else if (hasAnOpaquePath(this.base) && c === p("#")) {
			this.url.scheme = this.base.scheme;
			this.url.path = this.base.path;
			this.url.query = this.base.query;
			this.url.fragment = "";
			this.state = "fragment";
		} else if (this.base.scheme === "file") {
			this.state = "file";
			--this.pointer;
		} else {
			this.state = "relative";
			--this.pointer;
		}

		return true;
	};

	URLStateMachine.prototype["parse special relative or authority"] = function parseSpecialRelativeOrAuthority(c) {
		if (c === p("/") && this.input[this.pointer + 1] === p("/")) {
			this.state = "special authority ignore slashes";
			++this.pointer;
		} else {
			this.parseError = true;
			this.state = "relative";
			--this.pointer;
		}

		return true;
	};

	URLStateMachine.prototype["parse path or authority"] = function parsePathOrAuthority(c) {
		if (c === p("/")) {
			this.state = "authority";
		} else {
			this.state = "path";
			--this.pointer;
		}

		return true;
	};

	URLStateMachine.prototype["parse relative"] = function parseRelative(c) {
		this.url.scheme = this.base.scheme;
		if (c === p("/")) {
			this.state = "relative slash";
		} else if (isSpecial(this.url) && c === p("\\")) {
			this.parseError = true;
			this.state = "relative slash";
		} else {
			this.url.username = this.base.username;
			this.url.password = this.base.password;
			this.url.host = this.base.host;
			this.url.port = this.base.port;
			this.url.path = this.base.path.slice();
			this.url.query = this.base.query;
			if (c === p("?")) {
				this.url.query = "";
				this.state = "query";
			} else if (c === p("#")) {
				this.url.fragment = "";
				this.state = "fragment";
			} else if (!isNaN(c)) {
				this.url.query = null;
				this.url.path.pop();
				this.state = "path";
				--this.pointer;
			}
		}

		return true;
	};

	URLStateMachine.prototype["parse relative slash"] = function parseRelativeSlash(c) {
		if (isSpecial(this.url) && (c === p("/") || c === p("\\"))) {
			if (c === p("\\")) {
				this.parseError = true;
			}
			this.state = "special authority ignore slashes";
		} else if (c === p("/")) {
			this.state = "authority";
		} else {
			this.url.username = this.base.username;
			this.url.password = this.base.password;
			this.url.host = this.base.host;
			this.url.port = this.base.port;
			this.state = "path";
			--this.pointer;
		}

		return true;
	};

	URLStateMachine.prototype["parse special authority slashes"] = function parseSpecialAuthoritySlashes(c) {
		if (c === p("/") && this.input[this.pointer + 1] === p("/")) {
			this.state = "special authority ignore slashes";
			++this.pointer;
		} else {
			this.parseError = true;
			this.state = "special authority ignore slashes";
			--this.pointer;
		}

		return true;
	};

	URLStateMachine.prototype["parse special authority ignore slashes"] = function parseSpecialAuthorityIgnoreSlashes(c) {
		if (c !== p("/") && c !== p("\\")) {
			this.state = "authority";
			--this.pointer;
		} else {
			this.parseError = true;
		}

		return true;
	};

	URLStateMachine.prototype["parse authority"] = function parseAuthority(c, cStr) {
		if (c === p("@")) {
			this.parseError = true;
			if (this.atFlag) {
				this.buffer = `%40${this.buffer}`;
			}
			this.atFlag = true;

			// careful, this is based on buffer and has its own pointer (this.pointer != pointer) and inner chars
			const codePoints = Array.from(this.buffer);
			const len = codePoints.length;
			for (let pointer = 0; pointer < len; ++pointer) {
				const codePoint = codePoints[pointer].codePointAt(0);

				if (codePoint === p(":") && !this.passwordTokenSeenFlag) {
					this.passwordTokenSeenFlag = true;
					continue;
				}
				const encodedCodePoints = utf8PercentEncodeCodePoint(codePoint, isUserinfoPercentEncode);
				if (this.passwordTokenSeenFlag) {
					this.url.password += encodedCodePoints;
				} else {
					this.url.username += encodedCodePoints;
				}
			}
			this.buffer = "";
		} else if (isNaN(c) || c === p("/") || c === p("?") || c === p("#") || (isSpecial(this.url) && c === p("\\"))) {
			if (this.atFlag && this.buffer === "") {
				this.parseError = true;
				return failure;
			}
			this.pointer -= countSymbols(this.buffer) + 1;
			this.buffer = "";
			this.state = "host";
		} else {
			this.buffer += cStr;
		}

		return true;
	};

	URLStateMachine.prototype["parse hostname"] = URLStateMachine.prototype["parse host"] = function parseHostName(
		c,
		cStr,
	) {
		if (this.stateOverride && this.url.scheme === "file") {
			--this.pointer;
			this.state = "file host";
		} else if (c === p(":") && !this.arrFlag) {
			if (this.buffer === "") {
				this.parseError = true;
				return failure;
			}

			if (this.stateOverride === "hostname") {
				return false;
			}

			const host = parseHost(this.buffer, isNotSpecial(this.url));
			if (host === failure) {
				return failure;
			}

			this.url.host = host;
			this.buffer = "";
			this.state = "port";
		} else if (isNaN(c) || c === p("/") || c === p("?") || c === p("#") || (isSpecial(this.url) && c === p("\\"))) {
			--this.pointer;
			if (isSpecial(this.url) && this.buffer === "") {
				this.parseError = true;
				return failure;
			} else if (
				this.stateOverride &&
				this.buffer === "" &&
				(includesCredentials(this.url) || this.url.port !== null)
			) {
				this.parseError = true;
				return false;
			}

			const host = parseHost(this.buffer, isNotSpecial(this.url));
			if (host === failure) {
				return failure;
			}

			this.url.host = host;
			this.buffer = "";
			this.state = "path start";
			if (this.stateOverride) {
				return false;
			}
		} else {
			if (c === p("[")) {
				this.arrFlag = true;
			} else if (c === p("]")) {
				this.arrFlag = false;
			}
			this.buffer += cStr;
		}

		return true;
	};

	URLStateMachine.prototype["parse port"] = function parsePort(c, cStr) {
		if (infra.isASCIIDigit(c)) {
			this.buffer += cStr;
		} else if (
			isNaN(c) ||
			c === p("/") ||
			c === p("?") ||
			c === p("#") ||
			(isSpecial(this.url) && c === p("\\")) ||
			this.stateOverride
		) {
			if (this.buffer !== "") {
				const port = parseInt(this.buffer);
				if (port > 2 ** 16 - 1) {
					this.parseError = true;
					return failure;
				}
				this.url.port = port === defaultPort(this.url.scheme) ? null : port;
				this.buffer = "";
			}
			if (this.stateOverride) {
				return false;
			}
			this.state = "path start";
			--this.pointer;
		} else {
			this.parseError = true;
			return failure;
		}

		return true;
	};

	const fileOtherwiseCodePoints = new Set([p("/"), p("\\"), p("?"), p("#")]);

	function startsWithWindowsDriveLetter(input, pointer) {
		const length = input.length - pointer;
		return (
			length >= 2 &&
			isWindowsDriveLetterCodePoints(input[pointer], input[pointer + 1]) &&
			(length === 2 || fileOtherwiseCodePoints.has(input[pointer + 2]))
		);
	}

	URLStateMachine.prototype["parse file"] = function parseFile(c) {
		this.url.scheme = "file";
		this.url.host = "";

		if (c === p("/") || c === p("\\")) {
			if (c === p("\\")) {
				this.parseError = true;
			}
			this.state = "file slash";
		} else if (this.base !== null && this.base.scheme === "file") {
			this.url.host = this.base.host;
			this.url.path = this.base.path.slice();
			this.url.query = this.base.query;
			if (c === p("?")) {
				this.url.query = "";
				this.state = "query";
			} else if (c === p("#")) {
				this.url.fragment = "";
				this.state = "fragment";
			} else if (!isNaN(c)) {
				this.url.query = null;
				if (!startsWithWindowsDriveLetter(this.input, this.pointer)) {
					shortenPath(this.url);
				} else {
					this.parseError = true;
					this.url.path = [];
				}

				this.state = "path";
				--this.pointer;
			}
		} else {
			this.state = "path";
			--this.pointer;
		}

		return true;
	};

	URLStateMachine.prototype["parse file slash"] = function parseFileSlash(c) {
		if (c === p("/") || c === p("\\")) {
			if (c === p("\\")) {
				this.parseError = true;
			}
			this.state = "file host";
		} else {
			if (this.base !== null && this.base.scheme === "file") {
				if (
					!startsWithWindowsDriveLetter(this.input, this.pointer) &&
					isNormalizedWindowsDriveLetterString(this.base.path[0])
				) {
					this.url.path.push(this.base.path[0]);
				}
				this.url.host = this.base.host;
			}
			this.state = "path";
			--this.pointer;
		}

		return true;
	};

	URLStateMachine.prototype["parse file host"] = function parseFileHost(c, cStr) {
		if (isNaN(c) || c === p("/") || c === p("\\") || c === p("?") || c === p("#")) {
			--this.pointer;
			if (!this.stateOverride && isWindowsDriveLetterString(this.buffer)) {
				this.parseError = true;
				this.state = "path";
			} else if (this.buffer === "") {
				this.url.host = "";
				if (this.stateOverride) {
					return false;
				}
				this.state = "path start";
			} else {
				let host = parseHost(this.buffer, isNotSpecial(this.url));
				if (host === failure) {
					return failure;
				}
				if (host === "localhost") {
					host = "";
				}
				this.url.host = host;

				if (this.stateOverride) {
					return false;
				}

				this.buffer = "";
				this.state = "path start";
			}
		} else {
			this.buffer += cStr;
		}

		return true;
	};

	URLStateMachine.prototype["parse path start"] = function parsePathStart(c) {
		if (isSpecial(this.url)) {
			if (c === p("\\")) {
				this.parseError = true;
			}
			this.state = "path";

			if (c !== p("/") && c !== p("\\")) {
				--this.pointer;
			}
		} else if (!this.stateOverride && c === p("?")) {
			this.url.query = "";
			this.state = "query";
		} else if (!this.stateOverride && c === p("#")) {
			this.url.fragment = "";
			this.state = "fragment";
		} else if (c !== undefined) {
			this.state = "path";
			if (c !== p("/")) {
				--this.pointer;
			}
		} else if (this.stateOverride && this.url.host === null) {
			this.url.path.push("");
		}

		return true;
	};

	URLStateMachine.prototype["parse path"] = function parsePath(c) {
		if (
			isNaN(c) ||
			c === p("/") ||
			(isSpecial(this.url) && c === p("\\")) ||
			(!this.stateOverride && (c === p("?") || c === p("#")))
		) {
			if (isSpecial(this.url) && c === p("\\")) {
				this.parseError = true;
			}

			if (isDoubleDot(this.buffer)) {
				shortenPath(this.url);
				if (c !== p("/") && !(isSpecial(this.url) && c === p("\\"))) {
					this.url.path.push("");
				}
			} else if (isSingleDot(this.buffer) && c !== p("/") && !(isSpecial(this.url) && c === p("\\"))) {
				this.url.path.push("");
			} else if (!isSingleDot(this.buffer)) {
				if (this.url.scheme === "file" && this.url.path.length === 0 && isWindowsDriveLetterString(this.buffer)) {
					this.buffer = `${this.buffer[0]}:`;
				}
				this.url.path.push(this.buffer);
			}
			this.buffer = "";
			if (c === p("?")) {
				this.url.query = "";
				this.state = "query";
			}
			if (c === p("#")) {
				this.url.fragment = "";
				this.state = "fragment";
			}
		} else {
			if (
				c === p("%") &&
				(!infra.isASCIIHex(this.input[this.pointer + 1]) || !infra.isASCIIHex(this.input[this.pointer + 2]))
			) {
				this.parseError = true;
			}

			this.buffer += utf8PercentEncodeCodePoint(c, isPathPercentEncode);
		}

		return true;
	};

	URLStateMachine.prototype["parse opaque path"] = function parseOpaquePath(c) {
		if (c === p("?")) {
			this.url.query = "";
			this.state = "query";
		} else if (c === p("#")) {
			this.url.fragment = "";
			this.state = "fragment";
		} else if (c === p(" ")) {
			const remaining = this.input[this.pointer + 1];
			if (remaining === p("?") || remaining === p("#")) {
				this.url.path += "%20";
			} else {
				this.url.path += " ";
			}
		} else {
			if (!isNaN(c) && c !== p("%")) {
				this.parseError = true;
			}

			if (
				c === p("%") &&
				(!infra.isASCIIHex(this.input[this.pointer + 1]) || !infra.isASCIIHex(this.input[this.pointer + 2]))
			) {
				this.parseError = true;
			}

			if (!isNaN(c)) {
				this.url.path += utf8PercentEncodeCodePoint(c, isC0ControlPercentEncode);
			}
		}

		return true;
	};

	URLStateMachine.prototype["parse query"] = function parseQuery(c, cStr) {
		if (!isSpecial(this.url) || this.url.scheme === "ws" || this.url.scheme === "wss") {
			this.encodingOverride = "utf-8";
		}

		if ((!this.stateOverride && c === p("#")) || isNaN(c)) {
			const queryPercentEncodePredicate = isSpecial(this.url) ? isSpecialQueryPercentEncode : isQueryPercentEncode;
			this.url.query += utf8PercentEncodeString(this.buffer, queryPercentEncodePredicate);

			this.buffer = "";

			if (c === p("#")) {
				this.url.fragment = "";
				this.state = "fragment";
			}
		} else if (!isNaN(c)) {
			if (
				c === p("%") &&
				(!infra.isASCIIHex(this.input[this.pointer + 1]) || !infra.isASCIIHex(this.input[this.pointer + 2]))
			) {
				this.parseError = true;
			}

			this.buffer += cStr;
		}

		return true;
	};

	URLStateMachine.prototype["parse fragment"] = function parseFragment(c) {
		if (!isNaN(c)) {
			if (
				c === p("%") &&
				(!infra.isASCIIHex(this.input[this.pointer + 1]) || !infra.isASCIIHex(this.input[this.pointer + 2]))
			) {
				this.parseError = true;
			}

			this.url.fragment += utf8PercentEncodeCodePoint(c, isFragmentPercentEncode);
		}

		return true;
	};

	function serializeURL(url, excludeFragment) {
		let output = `${url.scheme}:`;
		if (url.host !== null) {
			output += "//";

			if (url.username !== "" || url.password !== "") {
				output += url.username;
				if (url.password !== "") {
					output += `:${url.password}`;
				}
				output += "@";
			}

			output += serializeHost(url.host);

			if (url.port !== null) {
				output += `:${url.port}`;
			}
		}

		if (url.host === null && !hasAnOpaquePath(url) && url.path.length > 1 && url.path[0] === "") {
			output += "/.";
		}
		output += serializePath(url);

		if (url.query !== null) {
			output += `?${url.query}`;
		}

		if (!excludeFragment && url.fragment !== null) {
			output += `#${url.fragment}`;
		}

		return output;
	}

	function serializeOrigin(tuple) {
		let result = `${tuple.scheme}://`;
		result += serializeHost(tuple.host);

		if (tuple.port !== null) {
			result += `:${tuple.port}`;
		}

		return result;
	}

	function serializePath(url) {
		if (hasAnOpaquePath(url)) {
			return url.path;
		}

		let output = "";
		for (const segment of url.path) {
			output += `/${segment}`;
		}
		return output;
	}

	parser.serializeURL = serializeURL;

	parser.serializePath = serializePath;

	parser.serializeURLOrigin = function (url) {
		// https://url.spec.whatwg.org/#concept-url-origin
		switch (url.scheme) {
			case "blob": {
				const pathURL = parser.parseURL(serializePath(url));
				if (pathURL === null) {
					return "null";
				}
				if (pathURL.scheme !== "http" && pathURL.scheme !== "https") {
					return "null";
				}
				return parser.serializeURLOrigin(pathURL);
			}
			case "ftp":
			case "http":
			case "https":
			case "ws":
			case "wss":
				return serializeOrigin({
					scheme: url.scheme,
					host: url.host,
					port: url.port,
				});
			case "file":
				// The spec says:
				// > Unfortunate as it is, this is left as an exercise to the reader. When in doubt, return a new opaque origin.
				// Browsers tested so far:
				// - Chrome says "file://", but treats file: URLs as cross-origin for most (all?) purposes; see e.g.
				//   https://bugs.chromium.org/p/chromium/issues/detail?id=37586
				// - Firefox says "null", but treats file: URLs as same-origin sometimes based on directory stuff; see
				//   https://developer.mozilla.org/en-US/docs/Archive/Misc_top_level/Same-origin_policy_for_file:_URIs
				return "null";
			default:
				// serializing an opaque origin returns "null"
				return "null";
		}
	};

	parser.basicURLParse = function (input, options) {
		if (options === undefined) {
			options = {};
		}

		const usm = new URLStateMachine(
			input,
			options.baseURL,
			options.encodingOverride,
			options.url,
			options.stateOverride,
		);
		if (usm.failure) {
			return null;
		}

		return usm.url;
	};

	parser.setTheUsername = function (url, username) {
		url.username = utf8PercentEncodeString(username, isUserinfoPercentEncode);
	};

	parser.setThePassword = function (url, password) {
		url.password = utf8PercentEncodeString(password, isUserinfoPercentEncode);
	};

	parser.serializeHost = serializeHost;

	parser.cannotHaveAUsernamePasswordPort = cannotHaveAUsernamePasswordPort;

	parser.hasAnOpaquePath = hasAnOpaquePath;

	parser.serializeInteger = function (integer) {
		return String(integer);
	};

	parser.parseURL = function (input, options) {
		if (options === undefined) {
			options = {};
		}

		// We don't handle blobs, so this just delegates:
		return parser.basicURLParse(input, { baseURL: options.baseURL, encodingOverride: options.encodingOverride });
	};

	return parser;
})();

function usv(value: unknown): string {
	if (typeof value === "symbol") throw new TypeError("Cannot convert a Symbol value to a string");
	const string = String(value);
	return string.replace(/[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/g, "\uFFFD");
}
function required(count: number, actual: number): void {
	if (actual < count)
		throw codedError(
			"ERR_MISSING_ARGS",
			`The ${count === 1 ? '"name"' : '"name" and "value"'} arguments must be specified`,
			TypeError,
		);
}
type ParamPair = [string, string];
type ParamsInit = string | Iterable<Iterable<unknown>> | Record<string, unknown>;
const kParams = Symbol("URLSearchParams pairs");
const kUpdate = Symbol("URLSearchParams update");
function parseParams(input: string): ParamPair[] {
	const result: ParamPair[] = [];
	for (const part of input.replace(/^\?/, "").split("&")) {
		if (!part) continue;
		const index = part.indexOf("=");
		const name = index < 0 ? part : part.slice(0, index);
		const value = index < 0 ? "" : part.slice(index + 1);
		result.push([
			utf8DecodeWithoutBOM(percent.percentDecodeString(name.replace(/\+/g, " "))),
			utf8DecodeWithoutBOM(percent.percentDecodeString(value.replace(/\+/g, " "))),
		]);
	}
	return result;
}
export class URLSearchParams {
	[kParams]: ParamPair[] = [];
	[kUpdate]: (() => void) | undefined;
	constructor(init: ParamsInit = "") {
		Object.defineProperty(this, kParams, { enumerable: false });
		Object.defineProperty(this, kUpdate, { value: undefined, writable: true, configurable: true, enumerable: false });
		if (init === null || init === undefined) return;
		if (typeof init === "object" && Symbol.iterator in init) {
			for (const item of init as Iterable<Iterable<unknown>>) {
				if (item === null || typeof item !== "object" || !(Symbol.iterator in item))
					throw codedError("ERR_INVALID_TUPLE", "Each query pair must be an iterable [name, value] tuple", TypeError);
				const pair = Array.from(item);
				if (pair.length !== 2)
					throw codedError("ERR_INVALID_TUPLE", "Each query pair must be an iterable [name, value] tuple", TypeError);
				this[kParams].push([usv(pair[0]), usv(pair[1])]);
			}
		} else if (typeof init === "object") {
			for (const name in init)
				if (Object.prototype.hasOwnProperty.call(init, name)) this[kParams].push([usv(name), usv(init[name])]);
		} else this[kParams] = parseParams(usv(init));
	}
	get size(): number {
		return this[kParams].length;
	}
	append(name: string, value: string): void {
		required(2, arguments.length);
		this[kParams].push([usv(name), usv(value)]);
		this[kUpdate]?.();
	}
	delete(name: string, value?: string): void {
		required(1, arguments.length);
		name = usv(name);
		const match = value === undefined ? undefined : usv(value);
		this[kParams] = this[kParams].filter((pair) => pair[0] !== name || (match !== undefined && pair[1] !== match));
		this[kUpdate]?.();
	}
	get(name: string): string | null {
		required(1, arguments.length);
		name = usv(name);
		return this[kParams].find((pair) => pair[0] === name)?.[1] ?? null;
	}
	getAll(name: string): string[] {
		required(1, arguments.length);
		name = usv(name);
		return this[kParams].filter((pair) => pair[0] === name).map((pair) => pair[1]);
	}
	has(name: string, value?: string): boolean {
		required(1, arguments.length);
		name = usv(name);
		const match = value === undefined ? undefined : usv(value);
		return this[kParams].some((pair) => pair[0] === name && (match === undefined || pair[1] === match));
	}
	set(name: string, value: string): void {
		required(2, arguments.length);
		name = usv(name);
		value = usv(value);
		let found = false;
		this[kParams] = this[kParams].filter((pair) => {
			if (pair[0] !== name) return true;
			if (found) return false;
			pair[1] = value;
			found = true;
			return true;
		});
		if (!found) this[kParams].push([name, value]);
		this[kUpdate]?.();
	}
	sort(): void {
		this[kParams].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
		this[kUpdate]?.();
	}
	forEach(callback: (value: string, name: string, params: URLSearchParams) => void, thisArg?: unknown): void {
		if (typeof callback !== "function") throw invalidArgType("callback", "a function", callback);
		for (let i = 0; i < this[kParams].length; i++) {
			const pair = this[kParams][i];
			callback.call(thisArg, pair[1], pair[0], this);
		}
	}
	*keys(): IterableIterator<string> {
		for (let i = 0; i < this[kParams].length; i++) yield this[kParams][i][0];
	}
	*values(): IterableIterator<string> {
		for (let i = 0; i < this[kParams].length; i++) yield this[kParams][i][1];
	}
	*entries(): IterableIterator<ParamPair> {
		for (let i = 0; i < this[kParams].length; i++) yield [...this[kParams][i]];
	}
	[Symbol.iterator](): IterableIterator<ParamPair> {
		return this.entries();
	}
	toString(): string {
		return this[kParams]
			.map(
				([name, value]) =>
					`${percent.utf8PercentEncodeString(name, percent.isURLEncodedPercentEncode, true)}=${percent.utf8PercentEncodeString(value, percent.isURLEncodedPercentEncode, true)}`,
			)
			.join("&");
	}
	get [Symbol.toStringTag](): string {
		return "URLSearchParams";
	}
}
const kURL = Symbol("URL record");
const kSearchParams = Symbol("URL search params");
const blobs = new Map<string, Blob>();
function invalidURL(input: unknown): Error {
	const error = codedError("ERR_INVALID_URL", "Invalid URL", TypeError);
	Object.defineProperty(error, "input", { value: String(input), enumerable: true, writable: true, configurable: true });
	return error;
}
function stripOpaqueSpaces(record: URLRecord): void {
	if (typeof record.path === "string" && record.query === null && record.fragment === null)
		record.path = record.path.replace(/ +$/, "");
}
export class URL {
	[kURL]: URLRecord;
	[kSearchParams]: URLSearchParams;
	constructor(input: string | URL, base?: string | URL) {
		if (arguments.length === 0) throw invalidURL(undefined);
		const string = usv(input);
		const baseRecord = base === undefined ? undefined : parser.basicURLParse(usv(base));
		if (base !== undefined && baseRecord === null) throw invalidURL(base);
		const record = parser.basicURLParse(string, { baseURL: baseRecord });
		if (!record) throw invalidURL(string);
		Object.defineProperty(this, kURL, { value: record, writable: true, configurable: true, enumerable: false });
		Object.defineProperty(this, kSearchParams, {
			value: new URLSearchParams(record.query ?? ""),
			writable: true,
			configurable: true,
			enumerable: false,
		});
		this[kSearchParams][kUpdate] = () => {
			this[kURL].query = this[kSearchParams].toString() || null;
			stripOpaqueSpaces(this[kURL]);
		};
	}
	get href(): string {
		return parser.serializeURL(this[kURL]);
	}
	set href(value: string) {
		const record = parser.basicURLParse(usv(value));
		if (!record) throw invalidURL(value);
		this[kURL] = record;
		this[kSearchParams][kParams] = parseParams(record.query ?? "");
	}
	get origin(): string {
		return parser.serializeURLOrigin(this[kURL]);
	}
	get protocol(): string {
		return this[kURL].scheme + ":";
	}
	set protocol(value: string) {
		parser.basicURLParse(usv(value) + ":", { url: this[kURL], stateOverride: "scheme start" });
	}
	get username(): string {
		return this[kURL].username;
	}
	set username(value: string) {
		value = usv(value);
		if (!parser.cannotHaveAUsernamePasswordPort(this[kURL])) parser.setTheUsername(this[kURL], value);
	}
	get password(): string {
		return this[kURL].password;
	}
	set password(value: string) {
		value = usv(value);
		if (!parser.cannotHaveAUsernamePasswordPort(this[kURL])) parser.setThePassword(this[kURL], value);
	}
	get host(): string {
		const r = this[kURL];
		return r.host === null ? "" : parser.serializeHost(r.host) + (r.port === null ? "" : ":" + r.port);
	}
	set host(value: string) {
		value = usv(value);
		if (!parser.hasAnOpaquePath(this[kURL])) parser.basicURLParse(value, { url: this[kURL], stateOverride: "host" });
	}
	get hostname(): string {
		return this[kURL].host === null ? "" : parser.serializeHost(this[kURL].host);
	}
	set hostname(value: string) {
		value = usv(value);
		if (!parser.hasAnOpaquePath(this[kURL]))
			parser.basicURLParse(value, { url: this[kURL], stateOverride: "hostname" });
	}
	get port(): string {
		return this[kURL].port === null ? "" : String(this[kURL].port);
	}
	set port(value: string) {
		value = usv(value);
		if (parser.cannotHaveAUsernamePasswordPort(this[kURL])) return;
		if (!value) this[kURL].port = null;
		else parser.basicURLParse(value, { url: this[kURL], stateOverride: "port" });
	}
	get pathname(): string {
		return parser.serializePath(this[kURL]);
	}
	set pathname(value: string) {
		value = usv(value);
		if (parser.hasAnOpaquePath(this[kURL])) return;
		this[kURL].path = [];
		parser.basicURLParse(value, { url: this[kURL], stateOverride: "path start" });
	}
	get search(): string {
		return this[kURL].query ? "?" + this[kURL].query : "";
	}
	set search(value: string) {
		value = usv(value);
		if (!value) {
			this[kURL].query = null;
			stripOpaqueSpaces(this[kURL]);
		} else {
			this[kURL].query = "";
			parser.basicURLParse(value.replace(/^\?/, ""), { url: this[kURL], stateOverride: "query" });
		}
		this[kSearchParams][kParams] = parseParams(this[kURL].query ?? "");
	}
	get searchParams(): URLSearchParams {
		return this[kSearchParams];
	}
	get hash(): string {
		return this[kURL].fragment ? "#" + this[kURL].fragment : "";
	}
	set hash(value: string) {
		value = usv(value);
		if (!value) {
			this[kURL].fragment = null;
			stripOpaqueSpaces(this[kURL]);
		} else {
			this[kURL].fragment = "";
			parser.basicURLParse(value.replace(/^#/, ""), { url: this[kURL], stateOverride: "fragment" });
		}
	}
	toString(): string {
		return this.href;
	}
	toJSON(): string {
		return this.href;
	}
	get [Symbol.toStringTag](): string {
		return "URL";
	}
	[Symbol.for("nodejs.util.inspect.custom")](
		_depth: number,
		options: object,
		inspect: (value: unknown, options?: object) => string,
	): string {
		const fields: Record<string, string> = {
			href: this.href,
			origin: this.origin,
			protocol: this.protocol,
			username: this.username,
			password: this.password,
			host: this.host,
			hostname: this.hostname,
			port: this.port,
			pathname: this.pathname,
			search: this.search,
			hash: this.hash,
		};
		return "URL " + inspect(fields, options);
	}
	static canParse(input: string, base?: string): boolean {
		if (arguments.length === 0) throw codedError("ERR_MISSING_ARGS", 'The "url" argument must be specified', TypeError);
		const text = usv(input),
			baseText = base === undefined ? undefined : usv(base);
		const baseURL = baseText === undefined ? undefined : parser.basicURLParse(baseText);
		return baseURL !== null && parser.basicURLParse(text, { baseURL }) !== null;
	}
	static parse(input: string, base?: string): URL | null {
		if (arguments.length === 0) throw codedError("ERR_MISSING_ARGS", 'The "url" argument must be specified', TypeError);
		const text = usv(input),
			baseText = base === undefined ? undefined : usv(base);
		try {
			return new URL(text, baseText);
		} catch {
			return null;
		}
	}
	static createObjectURL(blob: Blob): string {
		if (typeof Blob === "undefined" || !(blob instanceof Blob))
			throw invalidArgType("obj", "an instance of Blob", blob);
		const id = `blob:nodedata:${crypto.randomUUID()}`;
		blobs.set(id, blob);
		return id;
	}
	static revokeObjectURL(id: string): void {
		blobs.delete(usv(id));
	}
}
export function resolveObjectURL(id: string): Blob | undefined {
	return blobs.get(usv(id));
}
export function domainToASCII(domain: string): string {
	required(1, arguments.length);
	const value = parser.basicURLParse("http://" + usv(domain));
	return value ? parser.serializeHost(value.host) : "";
}
export function domainToUnicode(domain: string): string {
	required(1, arguments.length);
	const value = domainToASCII(domain);
	try {
		return toUnicode(value);
	} catch {
		return "";
	}
}

interface FileOptions {
	windows?: boolean;
}
function windowsOption(options?: FileOptions): boolean {
	const info = core.info() as { platform: string };
	return options?.windows ?? info.platform === "win32";
}
export function fileURLToPath(input: string | URL, options?: FileOptions): string {
	const url = typeof input === "string" ? new URL(input) : input;
	if (!(url instanceof URL)) throw invalidArgType("path", "of type string or an instance of URL", input);
	if (url.protocol !== "file:") throw codedError("ERR_INVALID_URL_SCHEME", "The URL must be of scheme file", TypeError);
	const windows = windowsOption(options);
	if (/%2f/i.test(url.pathname) || (windows && /%5c/i.test(url.pathname)))
		throw codedError(
			"ERR_INVALID_FILE_URL_PATH",
			"File URL path must not include encoded / or \\ characters",
			TypeError,
		);
	let pathname = decodeURIComponent(url.pathname);
	if (windows) {
		pathname = pathname.replace(/\//g, "\\");
		if (url.hostname) return "\\\\" + domainToUnicode(url.hostname) + pathname;
		if (!/^\\[A-Za-z]:/.test(pathname))
			throw codedError("ERR_INVALID_FILE_URL_PATH", "File URL path must be absolute", TypeError);
		return pathname.slice(1);
	}
	if (url.hostname)
		throw codedError(
			"ERR_INVALID_FILE_URL_HOST",
			'File URL host must be "localhost" or empty on this platform',
			TypeError,
		);
	return pathname;
}
export function pathToFileURL(path: string, options?: FileOptions): URL {
	if (typeof path !== "string") throw invalidArgType("path", "of type string", path);
	const windows = windowsOption(options),
		trailing = /[\\/]$/.test(path);
	let absolute = windows ? win32.resolve(path) : posix.resolve(path);
	if (trailing && !/[\\/]$/.test(absolute)) absolute += windows ? "\\" : "/";
	let host = "";
	if (windows) {
		absolute = absolute.replace(/\\/g, "/");
		if (absolute.startsWith("//")) {
			const end = absolute.indexOf("/", 2);
			host = absolute.slice(2, end < 0 ? undefined : end);
			absolute = end < 0 ? "/" : absolute.slice(end);
		} else absolute = "/" + absolute;
	}
	const encoded = absolute.replace(
		/[%\\\n\r\t#?]/g,
		(c) => "%" + c.charCodeAt(0).toString(16).toUpperCase().padStart(2, "0"),
	);
	return new URL("file://" + host + encoded);
}
export function urlToHttpOptions(url: URL): Record<string | symbol, unknown> {
	const options: Record<string | symbol, unknown> = {
		...url,
		protocol: url.protocol,
		hostname: url.hostname.replace(/^\[|\]$/g, ""),
		hash: url.hash,
		search: url.search,
		pathname: url.pathname,
		path: url.pathname + url.search,
		href: url.href,
	};
	if (url.port) options.port = Number(url.port);
	if (url.username || url.password)
		options.auth = decodeURIComponent(url.username) + ":" + decodeURIComponent(url.password);
	return options;
}
interface LegacyURL {
	protocol?: string | null;
	slashes?: boolean | null;
	auth?: string | null;
	host?: string | null;
	port?: string | number | null;
	hostname?: string | null;
	hash?: string | null;
	search?: string | null;
	query?: string | Record<string, unknown> | null;
	pathname?: string | null;
	path?: string | null;
	href?: string | null;
}
const slashed = /^(https?|ftp|gopher|file|wss?):$/;
const hostless = /^javascript:$/;
export class Url implements LegacyURL {
	protocol: string | null = null;
	slashes: boolean | null = null;
	auth: string | null = null;
	host: string | null = null;
	port: string | null = null;
	hostname: string | null = null;
	hash: string | null = null;
	search: string | null = null;
	query: string | Record<string, unknown> | null = null;
	pathname: string | null = null;
	path: string | null = null;
	href: string | null = null;
	parse(input: string, parseQueryString = false, slashesDenoteHost = false): this {
		if (typeof input !== "string") throw invalidArgType("url", "of type string", input);
		let rest = input.replace(/^[\u0000-\u0020\u00A0\uFEFF]+|[\u0000-\u0020\u00A0\uFEFF]+$/g, "");
		const beforeQuery = rest.search(/[?#]/);
		rest =
			(beforeQuery < 0 ? rest : rest.slice(0, beforeQuery)).replace(/\\/g, "/") +
			(beforeQuery < 0 ? "" : rest.slice(beforeQuery));
		const protocol = /^([a-z0-9.+-]+:)/i.exec(rest);
		if (protocol) {
			this.protocol = protocol[1].toLowerCase();
			rest = rest.slice(protocol[1].length);
		}
		const hasSlashes = rest.startsWith("//");
		if (
			!hostless.test(this.protocol ?? "") &&
			(slashesDenoteHost || this.protocol || /^\/\/[^/@]+@[^/@]+/.test(rest)) &&
			hasSlashes
		) {
			this.slashes = true;
			rest = rest.slice(2);
		}
		if (!hostless.test(this.protocol ?? "") && (this.slashes || (this.protocol && !slashed.test(this.protocol)))) {
			let nonHost = -1,
				at = -1;
			for (let i = 0; i < rest.length; i++) {
				const c = rest[i];
				if (c === "\t" || c === "\r" || c === "\n") {
					rest = rest.slice(0, i) + rest.slice(i + 1);
					i--;
					continue;
				}
				if (c === "/" || c === "?" || c === "#") {
					if (nonHost < 0) nonHost = i;
					break;
				}
				if (c === "@") {
					at = i;
					nonHost = -1;
				} else if (nonHost < 0 && /[ "%';<>\\^`{|}]/.test(c)) nonHost = i;
			}
			let start = 0;
			if (at >= 0) {
				this.auth = decodeURIComponent(rest.slice(0, at));
				start = at + 1;
			}
			this.host = rest.slice(start, nonHost < 0 ? undefined : nonHost);
			rest = nonHost < 0 ? "" : rest.slice(nonHost);
			this.parseHost();
			this.hostname ??= "";
			const ipv6 = this.hostname.startsWith("[") && this.hostname.endsWith("]");
			if (!ipv6) {
				const invalid = this.hostname.search(/[\\/#?:]/);
				if (invalid >= 0) {
					if (this.hostname[invalid] === ":")
						throw codedError(
							"ERR_INVALID_ARG_VALUE",
							`The argument 'url' is invalid. Invalid port in url '${input}'`,
							TypeError,
						);
					rest = "/" + this.hostname.slice(invalid) + rest;
					this.hostname = this.hostname.slice(0, invalid);
				}
			}
			this.hostname = this.hostname.length > 255 ? "" : this.hostname.toLowerCase();
			if (this.hostname) {
				if (ipv6) {
					if (/[\0\t\n\r #%/<>?@\\^|]/.test(this.hostname)) throw invalidURL(input);
				} else {
					this.hostname = toASCII(this.hostname);
					if (!this.hostname || /[\0\t\n\r #%/:<>?@[\\\]^|]/.test(this.hostname)) throw invalidURL(input);
				}
			}
			this.host = this.hostname + (this.port ? ":" + this.port : "");
			if (ipv6) {
				this.hostname = this.hostname.slice(1, -1);
				if (!rest.startsWith("/")) rest = "/" + rest;
			}
		}
		if (!hostless.test(this.protocol ?? ""))
			rest = rest.replace(
				/[<>"` \r\n\t{}|\\^']/g,
				(c) => "%" + c.charCodeAt(0).toString(16).toUpperCase().padStart(2, "0"),
			);
		const hash = rest.indexOf("#");
		if (hash >= 0) {
			this.hash = rest.slice(hash);
			rest = rest.slice(0, hash);
		}
		const query = rest.indexOf("?");
		if (query >= 0) {
			this.search = rest.slice(query);
			this.query = rest.slice(query + 1);
			rest = rest.slice(0, query);
		}
		if (parseQueryString) {
			const result: Record<string, string | string[]> = Object.create(null);
			for (const [name, value] of new URLSearchParams(this.query ?? "")) {
				const old = result[name];
				if (old === undefined) result[name] = value;
				else if (typeof old === "string") result[name] = [old, value];
				else old.push(value);
			}
			this.query = result;
		}
		if (rest) this.pathname = rest;
		if (slashed.test(this.protocol ?? "") && this.hostname && !this.pathname) this.pathname = "/";
		if (this.pathname || this.search) this.path = (this.pathname ?? "") + (this.search ?? "");
		this.href = this.format();
		return this;
	}
	format(): string {
		return format(this);
	}
	resolve(relative: string | Url): string {
		return this.resolveObject(relative).format();
	}
	resolveObject(relative: string | Url): Url {
		const rel = typeof relative === "string" ? parse(relative, false, true) : relative;
		const result = new Url();
		Object.assign(result, this);
		result.hash = rel.hash;
		if (rel.href === "") {
			result.href = result.format();
			return result;
		}
		if (rel.slashes && !rel.protocol) {
			const protocol = result.protocol;
			Object.assign(result, rel);
			result.protocol = protocol;
			if (slashed.test(result.protocol ?? "") && result.hostname && !result.pathname)
				result.path = result.pathname = "/";
			result.href = result.format();
			return result;
		}
		if (rel.protocol && rel.protocol !== result.protocol) {
			if (!slashed.test(rel.protocol)) {
				Object.assign(result, rel);
				result.href = result.format();
				return result;
			}
			result.protocol = rel.protocol;
			if (!rel.host && !/^file:?$/.test(rel.protocol) && !hostless.test(rel.protocol)) {
				const path = (rel.pathname ?? "").split("/");
				while (path.length && !(rel.host = path.shift() ?? "")) {}
				rel.host ||= "";
				rel.hostname ||= "";
				if (path[0] !== "") path.unshift("");
				if (path.length < 2) path.unshift("");
				result.pathname = path.join("/");
			} else result.pathname = rel.pathname;
			result.search = rel.search;
			result.query = rel.query;
			result.host = rel.host || "";
			result.auth = rel.auth;
			result.hostname = rel.hostname || rel.host;
			result.port = rel.port;
			if (result.pathname || result.search) result.path = (result.pathname || "") + (result.search || "");
			result.slashes ||= rel.slashes;
			result.href = result.format();
			return result;
		}
		const sourceAbsolute = !!result.pathname?.startsWith("/");
		const relativeAbsolute = !!(rel.host || rel.pathname?.startsWith("/"));
		let mustEndAbsolute = !!(relativeAbsolute || sourceAbsolute || (result.host && rel.pathname));
		const removeAllDots = mustEndAbsolute;
		let sourcePath = result.pathname?.split("/") ?? [];
		const relativePath = rel.pathname?.split("/") ?? [];
		const noLeadingSlashes = !!result.protocol && !slashed.test(result.protocol);
		if (noLeadingSlashes) {
			result.hostname = "";
			result.port = null;
			if (result.host) {
				if (sourcePath[0] === "") sourcePath[0] = result.host;
				else sourcePath.unshift(result.host);
			}
			result.host = "";
			if (rel.protocol) {
				rel.hostname = null;
				rel.port = null;
				result.auth = null;
				if (rel.host) {
					if (relativePath[0] === "") relativePath[0] = rel.host;
					else relativePath.unshift(rel.host);
				}
				rel.host = null;
			}
			mustEndAbsolute &&= relativePath[0] === "" || sourcePath[0] === "";
		}
		if (relativeAbsolute) {
			if (rel.host || rel.host === "") {
				if (result.host !== rel.host) result.auth = null;
				result.host = rel.host;
				result.port = rel.port;
			}
			if (rel.hostname || rel.hostname === "") {
				if (result.hostname !== rel.hostname) result.auth = null;
				result.hostname = rel.hostname;
			}
			result.search = rel.search;
			result.query = rel.query;
			sourcePath = relativePath;
		} else if (relativePath.length) {
			sourcePath.pop();
			sourcePath = sourcePath.concat(relativePath);
			result.search = rel.search;
			result.query = rel.query;
		} else if (rel.search !== null && rel.search !== undefined) {
			if (noLeadingSlashes) {
				result.hostname = result.host = sourcePath.shift() ?? "";
				if (result.host && result.host.indexOf("@") > 0) {
					const auth = result.host.split("@");
					result.auth = auth.shift() ?? "";
					result.hostname = result.host = auth.shift() ?? "";
				}
			}
			result.search = rel.search;
			result.query = rel.query;
			if (result.pathname !== null || result.search !== null)
				result.path = (result.pathname || "") + (result.search || "");
			result.href = result.format();
			return result;
		}
		if (!sourcePath.length) {
			result.pathname = null;
			result.path = result.search ? "/" + result.search : null;
			result.href = result.format();
			return result;
		}
		const last = sourcePath[sourcePath.length - 1];
		const trailingSlash = !!(
			((result.host || rel.host || sourcePath.length > 1) && (last === "." || last === "..")) ||
			last === ""
		);
		let up = 0;
		for (let i = sourcePath.length - 1; i >= 0; i--) {
			if (sourcePath[i] === ".") sourcePath.splice(i, 1);
			else if (sourcePath[i] === "..") {
				sourcePath.splice(i, 1);
				up++;
			} else if (up) {
				sourcePath.splice(i, 1);
				up--;
			}
		}
		if (!mustEndAbsolute && !removeAllDots) while (up--) sourcePath.unshift("..");
		if (mustEndAbsolute && sourcePath[0] !== "" && !sourcePath[0]?.startsWith("/")) sourcePath.unshift("");
		if (trailingSlash && !sourcePath.join("/").endsWith("/")) sourcePath.push("");
		const isAbsolute = sourcePath[0] === "" || !!sourcePath[0]?.startsWith("/");
		if (noLeadingSlashes) {
			result.hostname = result.host = isAbsolute ? "" : (sourcePath.shift() ?? "");
			if (result.host && result.host.indexOf("@") > 0) {
				const auth = result.host.split("@");
				result.auth = auth.shift() ?? "";
				result.hostname = result.host = auth.shift() ?? "";
			}
		}
		mustEndAbsolute ||= !!(result.host && sourcePath.length);
		if (mustEndAbsolute && !isAbsolute) sourcePath.unshift("");
		if (!sourcePath.length) {
			result.pathname = null;
			result.path = null;
		} else result.pathname = sourcePath.join("/");
		if (result.pathname !== null || result.search !== null)
			result.path = (result.pathname || "") + (result.search || "");
		result.auth = rel.auth || result.auth;
		result.slashes ||= rel.slashes;
		result.href = result.format();
		return result;
	}
	parseHost(): void {
		const host = this.host ?? "",
			port = /:[0-9]*$/.exec(host);
		if (port) {
			if (port[0] !== ":") this.port = port[0].slice(1);
			this.hostname = host.slice(0, -port[0].length);
		} else this.hostname = host;
	}
}
export function parse(url: string | Url, parseQueryString = false, slashesDenoteHost = false): Url {
	return url instanceof Url ? url : new Url().parse(url, parseQueryString, slashesDenoteHost);
}
export function format(
	input: string | URL | LegacyURL,
	options: { auth?: boolean; fragment?: boolean; search?: boolean; unicode?: boolean } = {},
): string {
	if (typeof input === "string") return parse(input).format();
	if (input instanceof URL) {
		const url = new URL(input.href);
		if (options.auth === false) {
			url.username = "";
			url.password = "";
		}
		if (options.fragment === false) url.hash = "";
		if (options.search === false) url.search = "";
		if (options.unicode) {
			const record = { ...url[kURL] };
			if (typeof record.host === "string") record.host = toUnicode(record.host);
			return parser.serializeURL(record);
		}
		return url.href;
	}
	if (!input || typeof input !== "object") throw invalidArgType("urlObject", "an object or string", input);
	let protocol = input.protocol ?? "";
	if (protocol && !protocol.endsWith(":")) protocol += ":";
	let auth = input.auth ? encodeURIComponent(input.auth).replace(/%3A/gi, ":") + "@" : "";
	let host =
		input.host ??
		(input.hostname
			? (input.hostname.includes(":") && !input.hostname.startsWith("[")
					? "[" + input.hostname + "]"
					: input.hostname) + (input.port ? ":" + input.port : "")
			: "");
	let pathname = input.pathname ?? "",
		search = input.search;
	if (!search && input.query && typeof input.query === "object") {
		const pairs: ParamPair[] = [];
		for (const name in input.query)
			if (Object.prototype.hasOwnProperty.call(input.query, name))
				for (const value of Array.isArray(input.query[name]) ? (input.query[name] as unknown[]) : [input.query[name]])
					pairs.push([
						name,
						typeof value === "string" ||
						typeof value === "number" ||
						typeof value === "boolean" ||
						typeof value === "bigint"
							? String(value)
							: "",
					]);
		search = pairs.map(([name, value]) => encodeURIComponent(name) + "=" + encodeURIComponent(value)).join("&");
	}
	if (search && !search.startsWith("?")) search = "?" + search;
	let hash = input.hash ?? "";
	if (hash && !hash.startsWith("#")) hash = "#" + hash;
	if (input.slashes || ((!protocol || slashed.test(protocol)) && host)) {
		host = "//" + auth + host;
		auth = "";
		if (pathname && !pathname.startsWith("/")) pathname = "/" + pathname;
	}
	return (
		protocol +
		auth +
		host +
		pathname.replace(/[?#]/g, (c) => (c === "?" ? "%3F" : "%23")) +
		(search ?? "").replace(/#/g, "%23") +
		hash
	);
}
export function resolve(from: string, to: string): string {
	return parse(from, false, true).resolve(to);
}
export function resolveObject(from: string | Url, to: string): Url {
	return !from ? parse(to, false, true) : parse(from, false, true).resolveObject(to);
}
export default {
	URL,
	URLSearchParams,
	Url,
	parse,
	format,
	resolve,
	resolveObject,
	domainToASCII,
	domainToUnicode,
	pathToFileURL,
	fileURLToPath,
	urlToHttpOptions,
	resolveObjectURL,
};
