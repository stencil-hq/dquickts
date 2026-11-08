// internal:transpile/transformers — qbun transformers plugged in front of sucrase's
// RootTransformer pipeline: `define` replacement and Bun.Transpiler `exports` options.

import Transformer from "internal:thirdparty/sucrase/transformers/Transformer";
import { TokenType as tt } from "internal:thirdparty/sucrase/parser/tokenizer/types";
import { IdentifierRole, isDeclaration } from "internal:thirdparty/sucrase/parser/tokenizer/index";
import type { DefineEntry } from "internal:transpile/options";

/** The subset of a sucrase token used here. */
export interface SucraseToken {
	type: number;
	contextualKeyword: number;
	start: number;
	end: number;
	scopeDepth: number;
	isType: boolean;
	identifierRole: number | null;
	rhsEndIndex: number | null;
}

/** The subset of sucrase's TokenProcessor used here. */
export interface SucraseTokenProcessor {
	code: string;
	tokens: SucraseToken[];
	currentIndex(): number;
	currentToken(): SucraseToken;
	identifierNameForToken(token: SucraseToken): string;
	replaceToken(code: string): void;
	removeToken(): void;
	removeInitialToken(): void;
	appendCode(code: string): void;
}

function tokenText(tp: SucraseTokenProcessor, token: SucraseToken): string {
	return tp.code.slice(token.start, token.end);
}

/** A define value used as an expression: literals and identifier chains as is, others in parens. */
function defineExpression(value: string): string {
	const v = value.trim();
	if (/^[A-Za-z_$][\w$]*(?:\.[A-Za-z_$][\w$]*)*$/.test(v)) return v;
	if (!v.startsWith("-")) {
		try {
			const parsed: unknown = JSON.parse(v);
			// Non-negative numbers, strings, booleans and null are safe in any expression position.
			if (parsed === null || typeof parsed !== "object") return v;
		} catch {
			// Not JSON: arbitrary code, parenthesized below.
		}
	}
	return `(${v})`;
}

/**
 * `define`: replace global identifiers and member chains (`process.env.NODE_ENV`) with the given
 * code, except where they are assigned, declared or used as property names.
 */
export class DefineTransformer extends Transformer {
	readonly tokens: SucraseTokenProcessor;
	readonly defines: { parts: string[]; value: string }[];

	constructor(tokens: SucraseTokenProcessor, defines: DefineEntry[]) {
		super();
		this.tokens = tokens;
		this.defines = defines.map((d) => ({ parts: d.parts, value: defineExpression(d.value) }));
	}

	process(): boolean {
		const tp = this.tokens;
		const tokens = tp.tokens;
		const index = tp.currentIndex();
		const token = tokens[index];
		if (token.type !== tt.name || token.isType) return false;
		if (index > 0) {
			const prev = tokens[index - 1].type;
			if (prev === tt.dot || prev === tt.questionDot || prev === tt.preIncDec) return false;
		}
		const role = token.identifierRole;
		if (
			role === IdentifierRole.ObjectKey ||
			role === IdentifierRole.ExportAccess ||
			role === IdentifierRole.ImportDeclaration ||
			isDeclaration(token)
		) {
			return false;
		}
		const name = tokenText(tp, token);
		for (const define of this.defines) {
			const parts = define.parts;
			if (parts[0] !== name) continue;
			let end = index + 1;
			let matched = true;
			for (let k = 1; k < parts.length; k++) {
				if (end + 1 >= tokens.length || tokens[end].type !== tt.dot || tokenText(tp, tokens[end + 1]) !== parts[k]) {
					matched = false;
					break;
				}
				end += 2;
			}
			if (!matched) continue;
			if (end < tokens.length) {
				const after = tokens[end].type;
				// Assignment targets keep the original reference.
				if (after === tt.eq || after === tt.assign || after === tt.postIncDec) continue;
			}
			if (role === IdentifierRole.ObjectShorthand && parts.length === 1) {
				tp.replaceToken(`${name}: ${define.value}`);
			} else {
				tp.replaceToken(define.value);
			}
			for (let k = index + 1; k < end; k++) tp.removeToken();
			return true;
		}
		return false;
	}
}

/**
 * Bun.Transpiler `exports`: remove (`eliminate`) or replace by `export var name = value`
 * (`replace`) top-level exported declarations of a single binding.
 */
export class ExportsTransformer extends Transformer {
	readonly tokens: SucraseTokenProcessor;
	readonly eliminate: Set<string>;
	readonly replace: Map<string, { name: string; value: unknown }>;

	constructor(
		tokens: SucraseTokenProcessor,
		eliminate: Set<string>,
		replace: Map<string, { name: string; value: unknown }>,
	) {
		super();
		this.tokens = tokens;
		this.eliminate = eliminate;
		this.replace = replace;
	}

	process(): boolean {
		const tp = this.tokens;
		const tokens = tp.tokens;
		const index = tp.currentIndex();
		const token = tokens[index];
		if (token.type !== tt._export || token.isType || token.scopeDepth !== 0 || token.rhsEndIndex == null) {
			return false;
		}
		const end = token.rhsEndIndex;
		const names = exportedDeclarationNames(tp, index, end);
		if (names.length !== 1) return false;
		const name = names[0];
		const replacement = this.replace.get(name);
		if (!this.eliminate.has(name) && replacement === undefined) return false;
		tp.removeInitialToken();
		while (tp.currentIndex() < end) tp.removeToken();
		if (replacement !== undefined) {
			const value = replacement.value === undefined ? "undefined" : JSON.stringify(replacement.value);
			tp.appendCode(`export var ${replacement.name} = ${value ?? "undefined"};`);
		}
		return true;
	}
}

/** Names declared by `export var/let/const/function/class/enum ...` (tokens [index, end)). */
export function exportedDeclarationNames(
	tp: Pick<SucraseTokenProcessor, "tokens" | "identifierNameForToken">,
	index: number,
	end: number,
): string[] {
	const tokens = tp.tokens;
	const exportToken = tokens[index];
	const next = tokens[index + 1];
	const names: string[] = [];
	if (next === undefined) return names;
	if (
		(next.type === tt._var || next.type === tt._let || next.type === tt._const) &&
		tokens[index + 2]?.type !== tt._enum
	) {
		for (let i = index + 2; i < end; i++) {
			const t = tokens[i];
			if (t.type === tt.name && !t.isType && t.scopeDepth === exportToken.scopeDepth && isDeclaration(t)) {
				names.push(tp.identifierNameForToken(t));
			}
		}
		return names;
	}
	for (let i = index + 1; i < end; i++) {
		const type = tokens[i].type;
		if (type === tt._function || type === tt._class || type === tt._enum) {
			let nameIndex = i + 1;
			if (nameIndex < end && tokens[nameIndex].type === tt.star) nameIndex++;
			if (nameIndex < end && tokens[nameIndex].type === tt.name)
				names.push(tp.identifierNameForToken(tokens[nameIndex]));
			break;
		}
		if (type !== tt._abstract && type !== tt._const && type !== tt._async && type !== tt.name) break;
	}
	return names;
}
