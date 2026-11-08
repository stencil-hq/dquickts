// Vendored from sucrase 3.35.1 (dist/esm), MIT License, Copyright (c) 2012-2018 various
// contributors (see https://github.com/alangpierce/sucrase). Imports rewritten to internal: specifiers.
import { ContextualKeyword } from "internal:thirdparty/sucrase/parser/tokenizer/keywords";
import { TokenType as tt } from "internal:thirdparty/sucrase/parser/tokenizer/types";

/**
 * Starting at a potential `with` or (legacy) `assert` token, remove the import
 * attributes if they exist.
 */
export function removeMaybeImportAttributes(tokens) {
	if (
		tokens.matches2(tt._with, tt.braceL) ||
		(tokens.matches2(tt.name, tt.braceL) && tokens.matchesContextual(ContextualKeyword._assert))
	) {
		// assert
		tokens.removeToken();
		// {
		tokens.removeToken();
		tokens.removeBalancedCode();
		// }
		tokens.removeToken();
	}
}
