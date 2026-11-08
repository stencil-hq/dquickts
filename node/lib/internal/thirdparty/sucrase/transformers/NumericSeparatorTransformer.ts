// Vendored from sucrase 3.35.1 (dist/esm), MIT License, Copyright (c) 2012-2018 various
// contributors (see https://github.com/alangpierce/sucrase). Imports rewritten to internal: specifiers.
import { TokenType as tt } from "internal:thirdparty/sucrase/parser/tokenizer/types";

import Transformer from "internal:thirdparty/sucrase/transformers/Transformer";

export default class NumericSeparatorTransformer extends Transformer {
	constructor(tokens) {
		super();
		this.tokens = tokens;
	}

	process() {
		if (this.tokens.matches1(tt.num)) {
			const code = this.tokens.currentTokenCode();
			if (code.includes("_")) {
				this.tokens.replaceToken(code.replace(/_/g, ""));
				return true;
			}
		}
		return false;
	}
}
