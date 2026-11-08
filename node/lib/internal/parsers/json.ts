// JSONC stays strict JSON: stripping comments must not admit JSON5 syntax.
export function parseJSONC(input: string): unknown {
	const text = String(input);
	const out = text.split("");
	let quoted = false;
	for (let i = 0; i < text.length; i++) {
		const c = text[i];
		if (quoted) {
			if (c === "\\") i++;
			else if (c === '"') quoted = false;
			continue;
		}
		if (c === '"') {
			quoted = true;
			continue;
		}
		if (c === "/" && text[i + 1] === "/") {
			out[i++] = " ";
			for (; i < text.length && text[i] !== "\n" && text[i] !== "\r"; i++) out[i] = " ";
			i--;
		} else if (c === "/" && text[i + 1] === "*") {
			const start = i;
			out[i++] = " ";
			out[i++] = " ";
			for (; i < text.length && !(text[i] === "*" && text[i + 1] === "/"); i++)
				if (text[i] !== "\n" && text[i] !== "\r") out[i] = " ";
			if (i === text.length) throw new SyntaxError(`Unterminated comment at position ${start}`);
			out[i++] = " ";
			out[i] = " ";
		}
	}
	quoted = false;
	for (let i = 0; i < out.length; i++) {
		if (quoted) {
			if (out[i] === "\\") i++;
			else if (out[i] === '"') quoted = false;
		} else if (out[i] === '"') quoted = true;
		else if (out[i] === ",") {
			let next = i + 1;
			while (/[\t\r\n ]/.test(out[next] ?? "x")) next++;
			let previous = i - 1;
			while (previous >= 0 && /[\t\r\n ]/.test(out[previous])) previous--;
			if ((out[next] === "]" || out[next] === "}") && previous >= 0 && !"[{,:".includes(out[previous])) out[i] = " ";
		}
	}
	if (out[0] === "\ufeff") out[0] = " ";
	return JSON.parse(out.join(""));
}

const incomplete = Symbol("incomplete JSON value");
class JSONScanner {
	position = 0;
	constructor(readonly text: string) {}
	whitespace(): void {
		while (/[\t\r\n ]/.test(this.text[this.position] ?? "x")) this.position++;
	}
	fail(message: string): never {
		const prefix = this.text.slice(0, this.position);
		const line = prefix.split("\n").length;
		const column = this.position - prefix.lastIndexOf("\n");
		throw new SyntaxError(`${message} at line ${line}, column ${column}`);
	}
	expect(c: string): void {
		if (this.position === this.text.length) throw incomplete;
		if (this.text[this.position] !== c) this.fail(`Expected '${c}'`);
		this.position++;
	}
	string(): void {
		this.expect('"');
		while (this.position < this.text.length) {
			const c = this.text.charCodeAt(this.position++);
			if (c === 34) return;
			if (c < 32) this.fail("Unescaped control character in JSON string");
			if (c !== 92) continue;
			if (this.position === this.text.length) throw incomplete;
			const escape = this.text[this.position++];
			if (escape === "u") {
				for (let n = 0; n < 4; n++) {
					if (this.position === this.text.length) throw incomplete;
					if (!/[0-9a-f]/i.test(this.text[this.position++])) this.fail("Invalid Unicode escape");
				}
			} else if (!'"\\/bfnrt'.includes(escape)) this.fail("Invalid JSON escape");
		}
		throw incomplete;
	}
	value(depth = 0): void {
		if (depth > 1000) this.fail("JSON nesting is too deep");
		this.whitespace();
		if (this.position === this.text.length) throw incomplete;
		const c = this.text[this.position];
		if (c === '"') return this.string();
		if (c === "{" || c === "[") {
			this.position++;
			const close = c === "{" ? "}" : "]";
			this.whitespace();
			if (this.text[this.position] === close) {
				this.position++;
				return;
			}
			while (true) {
				this.whitespace();
				if (c === "{") {
					this.string();
					this.whitespace();
					this.expect(":");
				}
				this.value(depth + 1);
				this.whitespace();
				if (this.position === this.text.length) throw incomplete;
				if (this.text[this.position] === close) {
					this.position++;
					return;
				}
				this.expect(",");
			}
		}
		const literal = c === "t" ? "true" : c === "f" ? "false" : c === "n" ? "null" : undefined;
		if (literal) {
			for (const character of literal) this.expect(character);
			return;
		}
		if (c === "-" || /[0-9]/.test(c)) {
			if (c === "-") {
				this.position++;
				if (this.position === this.text.length) throw incomplete;
			}
			if (this.text[this.position] === "0") this.position++;
			else {
				if (!/[1-9]/.test(this.text[this.position])) this.fail("Invalid JSON number");
				while (/[0-9]/.test(this.text[this.position] ?? "x")) this.position++;
			}
			if (this.text[this.position] === ".") {
				this.position++;
				if (this.position === this.text.length) throw incomplete;
				if (!/[0-9]/.test(this.text[this.position])) this.fail("Expected digit after decimal point");
				while (/[0-9]/.test(this.text[this.position] ?? "x")) this.position++;
			}
			if (/[eE]/.test(this.text[this.position] ?? "x")) {
				this.position++;
				if (/[+-]/.test(this.text[this.position] ?? "x")) this.position++;
				if (this.position === this.text.length) throw incomplete;
				if (!/[0-9]/.test(this.text[this.position])) this.fail("Expected digit in exponent");
				while (/[0-9]/.test(this.text[this.position] ?? "x")) this.position++;
			}
			return;
		}
		this.fail("Unexpected token in JSON");
	}
}

export interface ChunkResult {
	values: unknown[];
	read: number;
	done: boolean;
	error: SyntaxError | null;
}
export function parseJSONLChunk(text: string): ChunkResult {
	const scanner = new JSONScanner(text);
	const result: ChunkResult = { values: [], read: 0, done: false, error: null };
	try {
		while (true) {
			scanner.whitespace();
			if (scanner.position === text.length) {
				result.done = true;
				if (!result.values.length) result.read = text.length;
				break;
			}
			const start = scanner.position;
			scanner.value();
			if (scanner.position < text.length && !/[\t\r\n ]/.test(text[scanner.position]))
				scanner.fail("Expected whitespace between JSON values");
			result.values.push(JSON.parse(text.slice(start, scanner.position)));
			result.read = scanner.position;
		}
	} catch (error) {
		if (error !== incomplete) result.error = error instanceof SyntaxError ? error : new SyntaxError(String(error));
	}
	return result;
}

export function parseJSONL(text: string): unknown[] {
	const result = parseJSONLChunk(text);
	if (result.error) throw result.error;
	if (!result.done) throw new SyntaxError("Unexpected end of JSONL input");
	return result.values;
}
