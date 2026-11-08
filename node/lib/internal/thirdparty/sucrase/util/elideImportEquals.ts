// Vendored from sucrase 3.35.1 (dist/esm), MIT License, Copyright (c) 2012-2018 various
// contributors (see https://github.com/alangpierce/sucrase). Imports rewritten to internal: specifiers.
import { TokenType as tt } from "internal:thirdparty/sucrase/parser/tokenizer/types";

export default function elideImportEquals(tokens) {
	// import
	tokens.removeInitialToken();
	// name
	tokens.removeToken();
	// =
	tokens.removeToken();
	// name or require
	tokens.removeToken();
	// Handle either `import A = require('A')` or `import A = B.C.D`.
	if (tokens.matches1(tt.parenL)) {
		// (
		tokens.removeToken();
		// path string
		tokens.removeToken();
		// )
		tokens.removeToken();
	} else {
		while (tokens.matches1(tt.dot)) {
			// .
			tokens.removeToken();
			// name
			tokens.removeToken();
		}
	}
}
