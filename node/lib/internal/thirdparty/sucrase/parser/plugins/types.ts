// Vendored from sucrase 3.35.1 (dist/esm), MIT License, Copyright (c) 2012-2018 various
// contributors (see https://github.com/alangpierce/sucrase). Imports rewritten to internal: specifiers.
import { eatTypeToken, lookaheadType, match } from "internal:thirdparty/sucrase/parser/tokenizer/index";
import { TokenType as tt } from "internal:thirdparty/sucrase/parser/tokenizer/types";
import { isFlowEnabled, isTypeScriptEnabled } from "internal:thirdparty/sucrase/parser/traverser/base";
import { baseParseConditional } from "internal:thirdparty/sucrase/parser/traverser/expression";
import { flowParseTypeAnnotation } from "internal:thirdparty/sucrase/parser/plugins/flow";
import { tsParseTypeAnnotation } from "internal:thirdparty/sucrase/parser/plugins/typescript";

/**
 * Common parser code for TypeScript and Flow.
 */

// An apparent conditional expression could actually be an optional parameter in an arrow function.
export function typedParseConditional(noIn) {
	// If we see ?:, this can't possibly be a valid conditional. typedParseParenItem will be called
	// later to finish off the arrow parameter. We also need to handle bare ? tokens for optional
	// parameters without type annotations, i.e. ?, and ?) .
	if (match(tt.question)) {
		const nextType = lookaheadType();
		if (nextType === tt.colon || nextType === tt.comma || nextType === tt.parenR) {
			return;
		}
	}
	baseParseConditional(noIn);
}

// Note: These "type casts" are *not* valid TS expressions.
// But we parse them here and change them when completing the arrow function.
export function typedParseParenItem() {
	eatTypeToken(tt.question);
	if (match(tt.colon)) {
		if (isTypeScriptEnabled) {
			tsParseTypeAnnotation();
		} else if (isFlowEnabled) {
			flowParseTypeAnnotation();
		}
	}
}
