// Vendored from sucrase 3.35.1 (dist/esm), MIT License, Copyright (c) 2012-2018 various
// contributors (see https://github.com/alangpierce/sucrase). Imports rewritten to internal: specifiers.

import { TokenType as tt } from "internal:thirdparty/sucrase/parser/tokenizer/types";

import Transformer from "internal:thirdparty/sucrase/transformers/Transformer";

export default class OptionalCatchBindingTransformer extends Transformer {
	constructor(tokens, nameManager) {
		super();
		this.tokens = tokens;
		this.nameManager = nameManager;
	}

	process() {
		if (this.tokens.matches2(tt._catch, tt.braceL)) {
			this.tokens.copyToken();
			this.tokens.appendCode(` (${this.nameManager.claimFreeName("e")})`);
			return true;
		}
		return false;
	}
}
