// Vendored from sucrase 3.35.1 (dist/esm), MIT License, Copyright (c) 2012-2018 various
// contributors (see https://github.com/alangpierce/sucrase). Imports rewritten to internal: specifiers.

import { ContextualKeyword } from "internal:thirdparty/sucrase/parser/tokenizer/keywords";
import { TokenType as tt } from "internal:thirdparty/sucrase/parser/tokenizer/types";

import getClassInfo, {} from "internal:thirdparty/sucrase/util/getClassInfo";
import CJSImportTransformer from "internal:thirdparty/sucrase/transformers/CJSImportTransformer";
import ESMImportTransformer from "internal:thirdparty/sucrase/transformers/ESMImportTransformer";
import FlowTransformer from "internal:thirdparty/sucrase/transformers/FlowTransformer";
import JestHoistTransformer from "internal:thirdparty/sucrase/transformers/JestHoistTransformer";
import JSXTransformer from "internal:thirdparty/sucrase/transformers/JSXTransformer";
import NumericSeparatorTransformer from "internal:thirdparty/sucrase/transformers/NumericSeparatorTransformer";
import OptionalCatchBindingTransformer from "internal:thirdparty/sucrase/transformers/OptionalCatchBindingTransformer";
import OptionalChainingNullishTransformer from "internal:thirdparty/sucrase/transformers/OptionalChainingNullishTransformer";
import ReactDisplayNameTransformer from "internal:thirdparty/sucrase/transformers/ReactDisplayNameTransformer";
import ReactHotLoaderTransformer from "internal:thirdparty/sucrase/transformers/ReactHotLoaderTransformer";

import TypeScriptTransformer from "internal:thirdparty/sucrase/transformers/TypeScriptTransformer";

// qbun: tokens that can precede the name of a class member.
const LEGACY_MEMBER_MODIFIERS = [
	tt._public,
	tt._private,
	tt._protected,
	tt._readonly,
	tt._override,
	tt._abstract,
	tt._declare,
	tt._static,
	tt._async,
	tt._get,
	tt._set,
	tt.star,
];

/**
 * qbun: describe the class member starting at token `start` (after its decorators) for the
 * legacy decorators transform: {isStatic, key (JS expression or null for private names),
 * isMethod (methods and accessors), isConstructor}.
 */
function getLegacyMemberInfo(tokenProcessor, start) {
	const tokens = tokenProcessor.tokens;
	const code = tokenProcessor.code;
	let index = start;
	let isStatic = false;
	while (index < tokens.length) {
		const token = tokens[index];
		if (LEGACY_MEMBER_MODIFIERS.includes(token.type)) {
			if (token.type === tt._static) {
				isStatic = true;
			}
			index++;
			continue;
		}
		if (
			token.type === tt.name &&
			token.contextualKeyword === ContextualKeyword._accessor &&
			token.contextId != null &&
			index + 1 < tokens.length &&
			tokens[index + 1].contextId === token.contextId &&
			tokens[index + 1].type !== tt.braceR
		) {
			index++;
			continue;
		}
		break;
	}
	const keyToken = tokens[index];
	let key;
	let after = index + 1;
	let isConstructorName = false;
	if (keyToken.type === tt.bracketL) {
		let end = index + 1;
		while (end < tokens.length && !(tokens[end].type === tt.bracketR && tokens[end].contextId === keyToken.contextId)) {
			end++;
		}
		key = code.slice(tokens[index + 1].start, tokens[end - 1].end);
		after = end + 1;
	} else if (keyToken.type === tt.hash) {
		key = null;
		after = index + 2;
	} else if (keyToken.type === tt.string || keyToken.type === tt.num || keyToken.type === tt.bigint) {
		key = code.slice(keyToken.start, keyToken.end);
		isConstructorName = code.slice(keyToken.start + 1, keyToken.end - 1) === "constructor";
	} else {
		const name = code.slice(keyToken.start, keyToken.end);
		key = JSON.stringify(name);
		isConstructorName = name === "constructor";
	}
	while (after < tokens.length && tokens[after].isType) {
		after++;
	}
	const isMethod = after < tokens.length && tokens[after].type === tt.parenL;
	return { isStatic, key, isMethod, isConstructor: isMethod && !isStatic && isConstructorName };
}

export default class RootTransformer {
	__init() {
		this.transformers = [];
	}

	__init2() {
		this.generatedVariables = [];
	}

	constructor(sucraseContext, transforms, enableLegacyBabel5ModuleInterop, options) {
		RootTransformer.prototype.__init.call(this);
		RootTransformer.prototype.__init2.call(this);
		this.nameManager = sucraseContext.nameManager;
		this.helperManager = sucraseContext.helperManager;
		const { tokenProcessor, importProcessor } = sucraseContext;
		this.tokens = tokenProcessor;
		this.isImportsTransformEnabled = transforms.includes("imports");
		this.isReactHotLoaderTransformEnabled = transforms.includes("react-hot-loader");
		this.disableESTransforms = Boolean(options.disableESTransforms);
		// qbun: lower TypeScript legacy (experimentalDecorators) decorators.
		this.legacyDecorators = Boolean(options.legacyDecorators);

		if (!options.disableESTransforms) {
			this.transformers.push(new OptionalChainingNullishTransformer(tokenProcessor, this.nameManager));
			this.transformers.push(new NumericSeparatorTransformer(tokenProcessor));
			this.transformers.push(new OptionalCatchBindingTransformer(tokenProcessor, this.nameManager));
		}

		if (transforms.includes("jsx")) {
			if (options.jsxRuntime !== "preserve") {
				this.transformers.push(new JSXTransformer(this, tokenProcessor, importProcessor, this.nameManager, options));
			}
			this.transformers.push(new ReactDisplayNameTransformer(this, tokenProcessor, importProcessor, options));
		}

		let reactHotLoaderTransformer = null;
		if (transforms.includes("react-hot-loader")) {
			if (!options.filePath) {
				throw new Error("filePath is required when using the react-hot-loader transform.");
			}
			reactHotLoaderTransformer = new ReactHotLoaderTransformer(tokenProcessor, options.filePath);
			this.transformers.push(reactHotLoaderTransformer);
		}

		// Note that we always want to enable the imports transformer, even when the import transform
		// itself isn't enabled, since we need to do type-only import pruning for both Flow and
		// TypeScript.
		if (transforms.includes("imports")) {
			if (importProcessor === null) {
				throw new Error("Expected non-null importProcessor with imports transform enabled.");
			}
			this.transformers.push(
				new CJSImportTransformer(
					this,
					tokenProcessor,
					importProcessor,
					this.nameManager,
					this.helperManager,
					reactHotLoaderTransformer,
					enableLegacyBabel5ModuleInterop,
					Boolean(options.enableLegacyTypeScriptModuleInterop),
					transforms.includes("typescript"),
					transforms.includes("flow"),
					Boolean(options.preserveDynamicImport),
					Boolean(options.keepUnusedImports),
				),
			);
		} else {
			this.transformers.push(
				new ESMImportTransformer(
					tokenProcessor,
					this.nameManager,
					this.helperManager,
					reactHotLoaderTransformer,
					transforms.includes("typescript"),
					transforms.includes("flow"),
					Boolean(options.keepUnusedImports),
					options,
				),
			);
		}

		if (transforms.includes("flow")) {
			this.transformers.push(new FlowTransformer(this, tokenProcessor, transforms.includes("imports")));
		}
		if (transforms.includes("typescript")) {
			this.transformers.push(new TypeScriptTransformer(this, tokenProcessor, transforms.includes("imports")));
		}
		if (transforms.includes("jest")) {
			this.transformers.push(new JestHoistTransformer(this, tokenProcessor, this.nameManager, importProcessor));
		}
	}

	transform() {
		this.tokens.reset();
		this.processBalancedCode();
		const shouldAddUseStrict = this.isImportsTransformEnabled;
		// "use strict" always needs to be first, so override the normal transformer order.
		let prefix = shouldAddUseStrict ? '"use strict";' : "";
		for (const transformer of this.transformers) {
			prefix += transformer.getPrefixCode();
		}
		prefix += this.helperManager.emitHelpers();
		prefix += this.generatedVariables.map((v) => ` var ${v};`).join("");
		for (const transformer of this.transformers) {
			prefix += transformer.getHoistedCode();
		}
		let suffix = "";
		for (const transformer of this.transformers) {
			suffix += transformer.getSuffixCode();
		}
		const result = this.tokens.finish();
		let { code } = result;
		if (code.startsWith("#!")) {
			let newlineIndex = code.indexOf("\n");
			if (newlineIndex === -1) {
				newlineIndex = code.length;
				code += "\n";
			}
			return {
				code: code.slice(0, newlineIndex + 1) + prefix + code.slice(newlineIndex + 1) + suffix,
				// The hashbang line has no tokens, so shifting the tokens to account
				// for prefix can happen normally.
				mappings: this.shiftMappings(result.mappings, prefix.length),
			};
		} else {
			return {
				code: prefix + code + suffix,
				mappings: this.shiftMappings(result.mappings, prefix.length),
			};
		}
	}

	processBalancedCode() {
		let braceDepth = 0;
		let parenDepth = 0;
		while (!this.tokens.isAtEnd()) {
			if (this.tokens.matches1(tt.braceL) || this.tokens.matches1(tt.dollarBraceL)) {
				braceDepth++;
			} else if (this.tokens.matches1(tt.braceR)) {
				if (braceDepth === 0) {
					return;
				}
				braceDepth--;
			}
			if (this.tokens.matches1(tt.parenL)) {
				parenDepth++;
			} else if (this.tokens.matches1(tt.parenR)) {
				if (parenDepth === 0) {
					return;
				}
				parenDepth--;
			}
			this.processToken();
		}
	}

	processToken() {
		if (this.legacyDecorators && this.processPossibleLegacyDecoratedClass()) {
			return;
		}
		if (this.tokens.matches1(tt._class)) {
			this.processClass();
			return;
		}
		for (const transformer of this.transformers) {
			const wasProcessed = transformer.process();
			if (wasProcessed) {
				return;
			}
		}
		this.tokens.copyToken();
	}

	/**
	 * Skip past a class with a name and return that name.
	 */
	processNamedClass() {
		if (!this.tokens.matches2(tt._class, tt.name)) {
			throw new Error("Expected identifier for exported class name.");
		}
		const name = this.tokens.identifierNameAtIndex(this.tokens.currentIndex() + 1);
		this.processClass();
		return name;
	}

	/**
	 * qbun: `@dec class C {}`, `@dec export class C {}`, `export @dec class C {}` and the
	 * `export default` forms with legacy decorators become
	 * `let C = class C {}; C = _decorate([dec], C); export {C};`.
	 */
	processPossibleLegacyDecoratedClass() {
		const tokens = this.tokens.tokens;
		let index = this.tokens.currentIndex();
		let exportKind = null;
		if (tokens[index].type === tt._export) {
			exportKind = "named";
			index++;
			if (index < tokens.length && tokens[index].type === tt._default) {
				exportKind = "default";
				index++;
			}
		}
		if (
			index >= tokens.length ||
			tokens[index].type !== tt.at ||
			tokens[index].decoratorEnd == null ||
			tokens[index].decoratorOwner != null
		) {
			return false;
		}
		while (index < tokens.length && tokens[index].type === tt.at && tokens[index].decoratorEnd != null) {
			index = tokens[index].decoratorEnd;
		}
		if (exportKind === null && index < tokens.length && tokens[index].type === tt._export) {
			exportKind = "named";
			index++;
			if (index < tokens.length && tokens[index].type === tt._default) {
				exportKind = "default";
				index++;
			}
		}
		while (index < tokens.length && tokens[index].type === tt._abstract) {
			index++;
		}
		if (index >= tokens.length || tokens[index].type !== tt._class || tokens[index].isExpression) {
			return false;
		}

		if (this.tokens.matches1(tt._export)) {
			this.tokens.removeInitialToken();
			if (this.tokens.matches1(tt._default)) {
				this.tokens.removeToken();
			}
		}
		const decorators = [];
		while (this.tokens.matches1(tt.at)) {
			decorators.push(this.consumeLegacyDecorator());
		}
		if (this.tokens.matches1(tt._export)) {
			this.tokens.removeToken();
			if (this.tokens.matches1(tt._default)) {
				this.tokens.removeToken();
			}
		}
		while (this.tokens.matches1(tt._abstract)) {
			this.tokens.removeToken();
		}
		const nameToken = this.tokens.tokenAtRelativeIndex(1);
		const name =
			nameToken.type === tt.name && !nameToken.isType
				? this.tokens.identifierNameForToken(nameToken)
				: this.nameManager.claimFreeName("default_1");
		this.tokens.appendCode(`let ${name} = `);
		this.processClass({ decorators, name });
		if (exportKind === "named") {
			this.tokens.appendCode(` export {${name}};`);
		} else if (exportKind === "default") {
			this.tokens.appendCode(` export default ${name};`);
		}
		return true;
	}

	/**
	 * qbun: remove the decorator at the current `@` token and return its (transformed) expression.
	 */
	consumeLegacyDecorator() {
		const end = this.tokens.currentToken().decoratorEnd;
		this.tokens.removeInitialToken();
		const snapshot = this.tokens.snapshot();
		while (this.tokens.currentIndex() < end) {
			this.processToken();
		}
		const code = this.tokens.dangerouslyGetAndRemoveCodeSinceSnapshot(snapshot);
		// Keep the line structure of the output.
		this.tokens.appendCode(code.replace(/[^\n]/g, ""));
		return code.trim();
	}

	/**
	 * qbun: a class member or parameter decorator inside the class being processed.
	 */
	processLegacyMemberDecorator(legacy) {
		const token = this.tokens.currentToken();
		const owner = token.decoratorOwner;
		const paramIndex = token.decoratorParamIndex;
		const code = this.consumeLegacyDecorator();
		let member = legacy.byOwner.get(owner);
		if (member === undefined) {
			member = { info: getLegacyMemberInfo(this.tokens, owner), decorators: [] };
			legacy.byOwner.set(owner, member);
			legacy.members.push(member);
		}
		member.decorators.push(
			paramIndex == null ? code : `${this.helperManager.getHelperName("param")}(${paramIndex}, ${code})`,
		);
	}

	/**
	 * qbun: the `_decorate(...)` statements applying the collected legacy decorators, in tsc order:
	 * instance members, static members, then the class (with its constructor parameters).
	 */
	getLegacyDecorationStatements(legacy, legacyStatement) {
		const decorate = this.helperManager.getHelperName("decorate");
		const statements = [];
		const constructorParams = [];
		for (const isStatic of [false, true]) {
			for (const member of legacy.members) {
				if (member.info.isConstructor) {
					if (!isStatic) {
						constructorParams.push(...member.decorators);
					}
					continue;
				}
				if (member.info.isStatic !== isStatic || member.info.key === null) {
					continue;
				}
				const target = isStatic ? legacy.ref : `${legacy.ref}.prototype`;
				statements.push(
					`${decorate}([${member.decorators.join(", ")}], ${target}, ${member.info.key}, ${
						member.info.isMethod ? "null" : "void 0"
					})`,
				);
			}
		}
		const classDecorators = [...(legacyStatement !== null ? legacyStatement.decorators : []), ...constructorParams];
		if (classDecorators.length > 0) {
			statements.push(`${legacy.ref} = ${decorate}([${classDecorators.join(", ")}], ${legacy.ref})`);
		}
		return statements;
	}

	processClass(legacyStatement = null) {
		const classInfo = getClassInfo(this, this.tokens, this.nameManager, this.disableESTransforms);

		// Both static and instance initializers need a class name to use to invoke the initializer, so
		// assign to one if necessary.
		const needsCommaExpression =
			(classInfo.headerInfo.isExpression || !classInfo.headerInfo.className) &&
			classInfo.staticInitializerNames.length + classInfo.instanceInitializerNames.length > 0;

		let className = classInfo.headerInfo.className;
		if (needsCommaExpression) {
			className = this.nameManager.claimFreeName("_class");
			this.generatedVariables.push(className);
			this.tokens.appendCode(` (${className} =`);
		}

		const classToken = this.tokens.currentToken();
		const contextId = classToken.contextId;
		if (contextId == null) {
			throw new Error("Expected class to have a context ID.");
		}

		// qbun: legacy decorators of the members (and of the class itself for a decorated class
		// statement). Anonymous and expression classes are wrapped in a comma expression.
		let legacy = null;
		if (this.legacyDecorators) {
			let braceIndex = this.tokens.currentIndex();
			while (
				!(this.tokens.tokens[braceIndex].type === tt.braceL && this.tokens.tokens[braceIndex].contextId === contextId)
			) {
				braceIndex++;
			}
			if (legacyStatement !== null || this.tokens.tokens[braceIndex].hasMemberDecorators) {
				legacy = { byOwner: new Map(), members: [], ref: null, comma: false };
				if (legacyStatement !== null) {
					legacy.ref = legacyStatement.name;
				} else if (needsCommaExpression) {
					legacy.ref = className;
				} else if (!classInfo.headerInfo.isExpression && className) {
					legacy.ref = className;
				} else {
					legacy.ref = this.nameManager.claimFreeName("_class");
					this.generatedVariables.push(legacy.ref);
					legacy.comma = true;
					this.tokens.appendCode(` (${legacy.ref} =`);
				}
			}
		}

		this.tokens.copyExpectedToken(tt._class);
		while (!this.tokens.matchesContextIdAndLabel(tt.braceL, contextId)) {
			this.processToken();
		}

		this.processClassBody(classInfo, className, legacy);

		const legacyStatements = legacy !== null ? this.getLegacyDecorationStatements(legacy, legacyStatement) : [];
		const staticInitializerStatements = classInfo.staticInitializerNames.map((name) => `${className}.${name}()`);
		if (needsCommaExpression) {
			this.tokens.appendCode(
				`, ${[...legacyStatements, ...staticInitializerStatements].map((s) => `${s}, `).join("")}${className})`,
			);
		} else {
			if (legacy !== null && legacy.comma) {
				this.tokens.appendCode(`, ${legacyStatements.map((s) => `${s}, `).join("")}${legacy.ref})`);
			}
			if (classInfo.staticInitializerNames.length > 0) {
				this.tokens.appendCode(` ${staticInitializerStatements.map((s) => `${s};`).join(" ")}`);
			}
			if (legacy !== null && !legacy.comma && legacyStatements.length > 0) {
				this.tokens.appendCode(`; ${legacyStatements.map((s) => `${s};`).join(" ")}`);
			} else if (legacyStatement !== null) {
				this.tokens.appendCode(";");
			}
		}
	}

	/**
	 * We want to just handle class fields in all contexts, since TypeScript supports them. Later,
	 * when some JS implementations support class fields, this should be made optional.
	 */
	processClassBody(classInfo, className, legacy = null) {
		const {
			headerInfo,
			constructorInsertPos,
			constructorInitializerStatements,
			fields,
			instanceInitializerNames,
			rangesToRemove,
		} = classInfo;
		let fieldIndex = 0;
		let rangeToRemoveIndex = 0;
		const classContextId = this.tokens.currentToken().contextId;
		if (classContextId == null) {
			throw new Error("Expected non-null context ID on class.");
		}
		this.tokens.copyExpectedToken(tt.braceL);
		if (this.isReactHotLoaderTransformEnabled) {
			this.tokens.appendCode("__reactstandin__regenerateByEval(key, code) {this[key] = eval(code);}");
		}

		const needsConstructorInit = constructorInitializerStatements.length + instanceInitializerNames.length > 0;

		if (constructorInsertPos === null && needsConstructorInit) {
			const constructorInitializersCode = this.makeConstructorInitCode(
				constructorInitializerStatements,
				instanceInitializerNames,
				className,
			);
			if (headerInfo.hasSuperclass) {
				const argsName = this.nameManager.claimFreeName("args");
				this.tokens.appendCode(
					`constructor(...${argsName}) { super(...${argsName}); ${constructorInitializersCode}; }`,
				);
			} else {
				this.tokens.appendCode(`constructor() { ${constructorInitializersCode}; }`);
			}
		}

		while (!this.tokens.matchesContextIdAndLabel(tt.braceR, classContextId)) {
			if (legacy !== null && this.tokens.matches1(tt.at) && this.tokens.currentToken().decoratorOwner != null) {
				this.processLegacyMemberDecorator(legacy);
			} else if (fieldIndex < fields.length && this.tokens.currentIndex() === fields[fieldIndex].start) {
				let needsCloseBrace = false;
				if (this.tokens.matches1(tt.bracketL)) {
					this.tokens.copyTokenWithPrefix(`${fields[fieldIndex].initializerName}() {this`);
				} else if (this.tokens.matches1(tt.string) || this.tokens.matches1(tt.num)) {
					this.tokens.copyTokenWithPrefix(`${fields[fieldIndex].initializerName}() {this[`);
					needsCloseBrace = true;
				} else {
					this.tokens.copyTokenWithPrefix(`${fields[fieldIndex].initializerName}() {this.`);
				}
				while (this.tokens.currentIndex() < fields[fieldIndex].end) {
					if (needsCloseBrace && this.tokens.currentIndex() === fields[fieldIndex].equalsIndex) {
						this.tokens.appendCode("]");
					}
					this.processToken();
				}
				this.tokens.appendCode("}");
				fieldIndex++;
			} else if (
				rangeToRemoveIndex < rangesToRemove.length &&
				this.tokens.currentIndex() >= rangesToRemove[rangeToRemoveIndex].start
			) {
				if (this.tokens.currentIndex() < rangesToRemove[rangeToRemoveIndex].end) {
					this.tokens.removeInitialToken();
				}
				while (this.tokens.currentIndex() < rangesToRemove[rangeToRemoveIndex].end) {
					this.tokens.removeToken();
				}
				rangeToRemoveIndex++;
			} else if (this.tokens.currentIndex() === constructorInsertPos) {
				this.tokens.copyToken();
				if (needsConstructorInit) {
					this.tokens.appendCode(
						`;${this.makeConstructorInitCode(constructorInitializerStatements, instanceInitializerNames, className)};`,
					);
				}
				this.processToken();
			} else {
				this.processToken();
			}
		}
		this.tokens.copyExpectedToken(tt.braceR);
	}

	makeConstructorInitCode(constructorInitializerStatements, instanceInitializerNames, className) {
		return [
			...constructorInitializerStatements,
			...instanceInitializerNames.map((name) => `${className}.prototype.${name}.call(this)`),
		].join(";");
	}

	/**
	 * Normally it's ok to simply remove type tokens, but we need to be more careful when dealing with
	 * arrow function return types since they can confuse the parser. In that case, we want to move
	 * the close-paren to the same line as the arrow.
	 *
	 * See https://github.com/alangpierce/sucrase/issues/391 for more details.
	 */
	processPossibleArrowParamEnd() {
		if (this.tokens.matches2(tt.parenR, tt.colon) && this.tokens.tokenAtRelativeIndex(1).isType) {
			let nextNonTypeIndex = this.tokens.currentIndex() + 1;
			// Look ahead to see if this is an arrow function or something else.
			while (this.tokens.tokens[nextNonTypeIndex].isType) {
				nextNonTypeIndex++;
			}
			if (this.tokens.matches1AtIndex(nextNonTypeIndex, tt.arrow)) {
				this.tokens.removeInitialToken();
				while (this.tokens.currentIndex() < nextNonTypeIndex) {
					this.tokens.removeToken();
				}
				this.tokens.replaceTokenTrimmingLeftWhitespace(") =>");
				return true;
			}
		}
		return false;
	}

	/**
	 * An async arrow function might be of the form:
	 *
	 * async <
	 *   T
	 * >() => {}
	 *
	 * in which case, removing the type parameters will cause a syntax error. Detect this case and
	 * move the open-paren earlier.
	 */
	processPossibleAsyncArrowWithTypeParams() {
		if (!this.tokens.matchesContextual(ContextualKeyword._async) && !this.tokens.matches1(tt._async)) {
			return false;
		}
		const nextToken = this.tokens.tokenAtRelativeIndex(1);
		if (nextToken.type !== tt.lessThan || !nextToken.isType) {
			return false;
		}

		let nextNonTypeIndex = this.tokens.currentIndex() + 1;
		// Look ahead to see if this is an arrow function or something else.
		while (this.tokens.tokens[nextNonTypeIndex].isType) {
			nextNonTypeIndex++;
		}
		if (this.tokens.matches1AtIndex(nextNonTypeIndex, tt.parenL)) {
			this.tokens.replaceToken("async (");
			this.tokens.removeInitialToken();
			while (this.tokens.currentIndex() < nextNonTypeIndex) {
				this.tokens.removeToken();
			}
			this.tokens.removeToken();
			// We ate a ( token, so we need to process the tokens in between and then the ) token so that
			// we remain balanced.
			this.processBalancedCode();
			this.processToken();
			return true;
		}
		return false;
	}

	processPossibleTypeRange() {
		if (this.tokens.currentToken().isType) {
			this.tokens.removeInitialToken();
			while (this.tokens.currentToken().isType) {
				this.tokens.removeToken();
			}
			return true;
		}
		return false;
	}

	shiftMappings(mappings, prefixLength) {
		for (let i = 0; i < mappings.length; i++) {
			const mapping = mappings[i];
			if (mapping !== undefined) {
				mappings[i] = mapping + prefixLength;
			}
		}
		return mappings;
	}
}
