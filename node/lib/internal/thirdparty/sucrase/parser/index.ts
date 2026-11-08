// Vendored from sucrase 3.35.1 (dist/esm), MIT License, Copyright (c) 2012-2018 various
// contributors (see https://github.com/alangpierce/sucrase). Imports rewritten to internal: specifiers.

import { augmentError, initParser, state } from "internal:thirdparty/sucrase/parser/traverser/base";
import { parseFile } from "internal:thirdparty/sucrase/parser/traverser/index";

export class File {
	constructor(tokens, scopes) {
		this.tokens = tokens;
		this.scopes = scopes;
	}
}

export function parse(input, isJSXEnabled, isTypeScriptEnabled, isFlowEnabled) {
	if (isFlowEnabled && isTypeScriptEnabled) {
		throw new Error("Cannot combine flow and typescript plugins.");
	}
	initParser(input, isJSXEnabled, isTypeScriptEnabled, isFlowEnabled);
	const result = parseFile();
	if (state.error) {
		throw augmentError(state.error);
	}
	return result;
}
