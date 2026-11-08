// Vendored from sucrase 3.35.1 (dist/esm), MIT License, Copyright (c) 2012-2018 various
// contributors (see https://github.com/alangpierce/sucrase). Imports rewritten to internal: specifiers.

import { TokenType as tt } from "internal:thirdparty/sucrase/parser/tokenizer/types";

/**
 * Get all identifier names in the code, in order, including duplicates.
 */
export default function getIdentifierNames(code, tokens) {
	const names = [];
	for (const token of tokens) {
		if (token.type === tt.name) {
			names.push(code.slice(token.start, token.end));
		}
	}
	return names;
}
