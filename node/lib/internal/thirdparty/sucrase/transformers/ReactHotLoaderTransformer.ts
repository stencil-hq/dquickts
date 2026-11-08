// Vendored from sucrase 3.35.1 (dist/esm), MIT License, Copyright (c) 2012-2018 various
// contributors (see https://github.com/alangpierce/sucrase). Imports rewritten to internal: specifiers.
import { IdentifierRole, isTopLevelDeclaration } from "internal:thirdparty/sucrase/parser/tokenizer/index";

import Transformer from "internal:thirdparty/sucrase/transformers/Transformer";

export default class ReactHotLoaderTransformer extends Transformer {
	__init() {
		this.extractedDefaultExportName = null;
	}

	constructor(tokens, filePath) {
		super();
		this.tokens = tokens;
		this.filePath = filePath;
		ReactHotLoaderTransformer.prototype.__init.call(this);
	}

	setExtractedDefaultExportName(extractedDefaultExportName) {
		this.extractedDefaultExportName = extractedDefaultExportName;
	}

	getPrefixCode() {
		return `
      (function () {
        var enterModule = require('react-hot-loader').enterModule;
        enterModule && enterModule(module);
      })();`
			.replace(/\s+/g, " ")
			.trim();
	}

	getSuffixCode() {
		const topLevelNames = new Set();
		for (const token of this.tokens.tokens) {
			if (!token.isType && isTopLevelDeclaration(token) && token.identifierRole !== IdentifierRole.ImportDeclaration) {
				topLevelNames.add(this.tokens.identifierNameForToken(token));
			}
		}
		const namesToRegister = Array.from(topLevelNames).map((name) => ({
			variableName: name,
			uniqueLocalName: name,
		}));
		if (this.extractedDefaultExportName) {
			namesToRegister.push({
				variableName: this.extractedDefaultExportName,
				uniqueLocalName: "default",
			});
		}
		return `
;(function () {
  var reactHotLoader = require('react-hot-loader').default;
  var leaveModule = require('react-hot-loader').leaveModule;
  if (!reactHotLoader) {
    return;
  }
${namesToRegister
	.map(
		({ variableName, uniqueLocalName }) =>
			`  reactHotLoader.register(${variableName}, "${uniqueLocalName}", ${JSON.stringify(this.filePath || "")});`,
	)
	.join("\n")}
  leaveModule(module);
})();`;
	}

	process() {
		return false;
	}
}
