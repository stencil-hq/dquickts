// Vendored from sucrase 3.35.1 (dist/esm), MIT License, Copyright (c) 2012-2018 various
// contributors (see https://github.com/alangpierce/sucrase). Imports rewritten to internal: specifiers.

import { nextToken, skipLineComment } from "internal:thirdparty/sucrase/parser/tokenizer/index";
import { charCodes } from "internal:thirdparty/sucrase/parser/util/charcodes";
import { input, state } from "internal:thirdparty/sucrase/parser/traverser/base";
import { parseTopLevel } from "internal:thirdparty/sucrase/parser/traverser/statement";

export function parseFile() {
	// If enabled, skip leading hashbang line.
	if (
		state.pos === 0 &&
		input.charCodeAt(0) === charCodes.numberSign &&
		input.charCodeAt(1) === charCodes.exclamationMark
	) {
		skipLineComment(2);
	}
	nextToken();
	return parseTopLevel();
}
