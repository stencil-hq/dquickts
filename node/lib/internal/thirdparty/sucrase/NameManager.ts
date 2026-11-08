// Vendored from sucrase 3.35.1 (dist/esm), MIT License, Copyright (c) 2012-2018 various
// contributors (see https://github.com/alangpierce/sucrase). Imports rewritten to internal: specifiers.

import getIdentifierNames from "internal:thirdparty/sucrase/util/getIdentifierNames";

export default class NameManager {
	__init() {
		this.usedNames = new Set();
	}

	constructor(code, tokens) {
		NameManager.prototype.__init.call(this);
		this.usedNames = new Set(getIdentifierNames(code, tokens));
	}

	claimFreeName(name) {
		const newName = this.findFreeName(name);
		this.usedNames.add(newName);
		return newName;
	}

	findFreeName(name) {
		if (!this.usedNames.has(name)) {
			return name;
		}
		let suffixNum = 2;
		while (this.usedNames.has(name + String(suffixNum))) {
			suffixNum++;
		}
		return name + String(suffixNum);
	}
}
