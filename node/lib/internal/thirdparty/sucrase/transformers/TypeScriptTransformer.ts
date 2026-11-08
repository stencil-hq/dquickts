// Vendored from sucrase 3.35.1 (dist/esm), MIT License, Copyright (c) 2012-2018 various
// contributors (see https://github.com/alangpierce/sucrase). Imports rewritten to internal: specifiers.

import { TokenType as tt } from "internal:thirdparty/sucrase/parser/tokenizer/types";
import { isDeclaration } from "internal:thirdparty/sucrase/parser/tokenizer/index";

import isIdentifier from "internal:thirdparty/sucrase/util/isIdentifier";

import Transformer from "internal:thirdparty/sucrase/transformers/Transformer";

export default class TypeScriptTransformer extends Transformer {
	constructor(rootTransformer, tokens, isImportsTransformEnabled) {
		super();
		this.rootTransformer = rootTransformer;
		this.tokens = tokens;
		this.isImportsTransformEnabled = isImportsTransformEnabled;
	}

	process() {
		if (
			this.rootTransformer.processPossibleArrowParamEnd() ||
			this.rootTransformer.processPossibleAsyncArrowWithTypeParams() ||
			this.rootTransformer.processPossibleTypeRange()
		) {
			return true;
		}
		if (this.tokens.currentToken().isValueNamespace) {
			this.processNamespace(false, null);
			return true;
		}
		if (
			this.tokens.matches1(tt._export) &&
			this.tokens.currentIndex() + 1 < this.tokens.tokens.length &&
			this.tokens.tokenAtRelativeIndex(1).isValueNamespace
		) {
			this.processNamespace(true, null);
			return true;
		}
		if (
			this.tokens.matches1(tt._public) ||
			this.tokens.matches1(tt._protected) ||
			this.tokens.matches1(tt._private) ||
			this.tokens.matches1(tt._abstract) ||
			this.tokens.matches1(tt._readonly) ||
			this.tokens.matches1(tt._override) ||
			this.tokens.matches1(tt.nonNullAssertion)
		) {
			this.tokens.removeInitialToken();
			return true;
		}
		if (this.tokens.matches1(tt._enum) || this.tokens.matches2(tt._const, tt._enum)) {
			this.processEnum();
			return true;
		}
		if (this.tokens.matches2(tt._export, tt._enum) || this.tokens.matches3(tt._export, tt._const, tt._enum)) {
			this.processEnum(true);
			return true;
		}
		return false;
	}

	/**
	 * qbun: compile a namespace with values like tsc:
	 *   namespace A.B { body }  ->  var A; (function (A) { let B; (function (B) { body })(B = A.B || (A.B = {})); })(A || (A = {}));
	 * Exported declarations of the body stay local bindings and are also assigned to the namespace
	 * object (`let`/`var` through accessors, so both sides see updates). parentName is the enclosing
	 * namespace when this one is exported from it.
	 */
	processNamespace(isExport, parentName) {
		const keywordIndex = this.tokens.currentIndex() + (isExport ? 1 : 0);
		this.tokens.removeInitialToken();
		if (isExport) {
			this.tokens.removeToken();
		}
		const names = [this.tokens.identifierName()];
		this.tokens.removeToken();
		while (this.tokens.matches1(tt.dot)) {
			this.tokens.removeToken();
			names.push(this.tokens.identifierName());
			this.tokens.removeToken();
		}
		const first = names[0];
		let code;
		if (this.isDeclaredInScope(first, keywordIndex)) {
			code = ";";
		} else {
			code = `${isExport ? "export " : ""}var ${first}; `;
		}
		code += `(function (${first}) {`;
		for (let i = 1; i < names.length; i++) {
			code += ` let ${names[i]}; (function (${names[i]}) {`;
		}
		this.tokens.appendCode(code);
		// {
		this.tokens.removeToken();
		this.processNamespaceBody(names[names.length - 1]);
		// }
		this.tokens.removeToken();
		let suffix = "";
		for (let i = names.length - 1; i >= 1; i--) {
			const outer = names[i - 1];
			suffix += `})(${names[i]} = ${outer}.${names[i]} || (${outer}.${names[i]} = {}));`;
		}
		if (parentName !== null) {
			suffix += `})(${first} = ${parentName}.${first} || (${parentName}.${first} = {}));`;
		} else {
			suffix += `})(${first} || (${first} = {}));`;
		}
		this.tokens.appendCode(suffix);
	}

	/**
	 * qbun: whether `name` is declared as a class or function before the namespace keyword at
	 * keywordIndex in the same scope (the namespace then merges with it and declares no variable).
	 */
	isDeclaredInScope(name, keywordIndex) {
		const tokens = this.tokens.tokens;
		const depth = tokens[keywordIndex].scopeDepth;
		for (let i = keywordIndex - 1; i >= 0; i--) {
			const token = tokens[i];
			if (token.scopeDepth < depth) {
				break;
			}
			if (token.scopeDepth !== depth || token.type !== tt.name || token.isType) {
				continue;
			}
			if (this.tokens.identifierNameForToken(token) !== name) {
				continue;
			}
			let prev = i - 1;
			if (prev >= 0 && tokens[prev].type === tt.star) {
				prev--;
			}
			if (prev >= 0 && (tokens[prev].type === tt._class || tokens[prev].type === tt._function)) {
				return true;
			}
		}
		return false;
	}

	processNamespaceBody(namespaceName) {
		let braceDepth = 0;
		while (!this.tokens.isAtEnd()) {
			if (this.tokens.matches1(tt.braceL) || this.tokens.matches1(tt.dollarBraceL)) {
				braceDepth++;
			} else if (this.tokens.matches1(tt.braceR)) {
				if (braceDepth === 0) {
					return;
				}
				braceDepth--;
			} else if (braceDepth === 0 && this.tokens.matches1(tt._export) && !this.tokens.currentToken().isType) {
				this.processNamespaceExport(namespaceName);
				continue;
			}
			this.rootTransformer.processToken();
		}
	}

	processNamespaceExport(namespaceName) {
		const tokens = this.tokens.tokens;
		const exportIndex = this.tokens.currentIndex();
		const exportToken = tokens[exportIndex];
		const next = tokens[exportIndex + 1];
		if (next.isValueNamespace) {
			this.tokens.removeInitialToken();
			this.processNamespace(false, namespaceName);
			return;
		}
		const end = exportToken.rhsEndIndex;
		if (end == null) {
			this.tokens.copyToken();
			return;
		}
		const names = [];
		let live = false;
		if (next.type === tt._import) {
			// export import A = B.C;
			names.push(this.tokens.identifierNameAtIndex(exportIndex + 2));
			this.tokens.removeInitialToken();
			this.tokens.replaceToken("const");
		} else {
			if (
				(next.type === tt._var || next.type === tt._let || next.type === tt._const) &&
				tokens[exportIndex + 2].type !== tt._enum
			) {
				live = next.type !== tt._const;
				for (let i = exportIndex + 2; i < end; i++) {
					const token = tokens[i];
					if (
						token.type === tt.name &&
						!token.isType &&
						token.scopeDepth === exportToken.scopeDepth &&
						isDeclaration(token)
					) {
						names.push(this.tokens.identifierNameForToken(token));
					}
				}
			} else {
				let declKeyword = -1;
				for (let i = exportIndex + 1; i < end; i++) {
					const type = tokens[i].type;
					if (type === tt._function || type === tt._class || type === tt._enum) {
						declKeyword = i;
						break;
					}
					if (type !== tt._abstract && type !== tt._const && type !== tt.name && type !== tt._async) {
						break;
					}
				}
				if (declKeyword !== -1) {
					let nameIndex = declKeyword + 1;
					if (nameIndex < end && tokens[nameIndex].type === tt.star) {
						nameIndex++;
					}
					if (nameIndex < end && tokens[nameIndex].type === tt.name) {
						names.push(this.tokens.identifierNameForToken(tokens[nameIndex]));
					}
				}
			}
			this.tokens.removeInitialToken();
		}
		while (this.tokens.currentIndex() < end) {
			this.rootTransformer.processToken();
		}
		if (names.length > 0) {
			const ns = namespaceName;
			const assignments = names.map((name) =>
				live
					? `Object.defineProperty(${ns}, "${name}", {enumerable: true, configurable: true, get() { return ${name}; }, set(v) { ${name} = v; }});`
					: `${ns}.${name} = ${name};`,
			);
			const semi = tokens[end - 1].type === tt.semi ? "" : ";";
			this.tokens.appendCode(`${semi} ${assignments.join(" ")}`);
		}
	}

	processEnum(isExport = false) {
		// We might have "export const enum", so just remove all relevant tokens.
		this.tokens.removeInitialToken();
		while (this.tokens.matches1(tt._const) || this.tokens.matches1(tt._enum)) {
			this.tokens.removeToken();
		}
		const enumName = this.tokens.identifierName();
		this.tokens.removeToken();
		if (isExport && !this.isImportsTransformEnabled) {
			this.tokens.appendCode("export ");
		}
		this.tokens.appendCode(`var ${enumName}; (function (${enumName})`);
		this.tokens.copyExpectedToken(tt.braceL);
		this.processEnumBody(enumName);
		this.tokens.copyExpectedToken(tt.braceR);
		if (isExport && this.isImportsTransformEnabled) {
			this.tokens.appendCode(`)(${enumName} || (exports.${enumName} = ${enumName} = {}));`);
		} else {
			this.tokens.appendCode(`)(${enumName} || (${enumName} = {}));`);
		}
	}

	/**
	 * Transform an enum into equivalent JS. This has complexity in a few places:
	 * - TS allows string enums, numeric enums, and a mix of the two styles within an enum.
	 * - Enum keys are allowed to be referenced in later enum values.
	 * - Enum keys are allowed to be strings.
	 * - When enum values are omitted, they should follow an auto-increment behavior.
	 */
	processEnumBody(enumName) {
		// Code that can be used to reference the previous enum member, or null if this is the first
		// enum member.
		let previousValueCode = null;
		while (true) {
			if (this.tokens.matches1(tt.braceR)) {
				break;
			}
			const { nameStringCode, variableName } = this.extractEnumKeyInfo(this.tokens.currentToken());
			this.tokens.removeInitialToken();

			if (this.tokens.matches3(tt.eq, tt.string, tt.comma) || this.tokens.matches3(tt.eq, tt.string, tt.braceR)) {
				this.processStringLiteralEnumMember(enumName, nameStringCode, variableName);
			} else if (this.tokens.matches1(tt.eq)) {
				this.processExplicitValueEnumMember(enumName, nameStringCode, variableName);
			} else {
				this.processImplicitValueEnumMember(enumName, nameStringCode, variableName, previousValueCode);
			}
			if (this.tokens.matches1(tt.comma)) {
				this.tokens.removeToken();
			}

			if (variableName != null) {
				previousValueCode = variableName;
			} else {
				previousValueCode = `${enumName}[${nameStringCode}]`;
			}
		}
	}

	/**
	 * Detect name information about this enum key, which will be used to determine which code to emit
	 * and whether we should declare a variable as part of this declaration.
	 *
	 * Some cases to keep in mind:
	 * - Enum keys can be implicitly referenced later, e.g. `X = 1, Y = X`. In Sucrase, we implement
	 *   this by declaring a variable `X` so that later expressions can use it.
	 * - In addition to the usual identifier key syntax, enum keys are allowed to be string literals,
	 *   e.g. `"hello world" = 3,`. Template literal syntax is NOT allowed.
	 * - Even if the enum key is defined as a string literal, it may still be referenced by identifier
	 *   later, e.g. `"X" = 1, Y = X`. That means that we need to detect whether or not a string
	 *   literal is identifier-like and emit a variable if so, even if the declaration did not use an
	 *   identifier.
	 * - Reserved keywords like `break` are valid enum keys, but are not valid to be referenced later
	 *   and would be a syntax error if we emitted a variable, so we need to skip the variable
	 *   declaration in those cases.
	 *
	 * The variableName return value captures these nuances: if non-null, we can and must emit a
	 * variable declaration, and if null, we can't and shouldn't.
	 */
	extractEnumKeyInfo(nameToken) {
		if (nameToken.type === tt.name) {
			const name = this.tokens.identifierNameForToken(nameToken);
			return {
				nameStringCode: `"${name}"`,
				variableName: isIdentifier(name) ? name : null,
			};
		} else if (nameToken.type === tt.string) {
			const name = this.tokens.stringValueForToken(nameToken);
			return {
				nameStringCode: this.tokens.code.slice(nameToken.start, nameToken.end),
				variableName: isIdentifier(name) ? name : null,
			};
		} else {
			throw new Error("Expected name or string at beginning of enum element.");
		}
	}

	/**
	 * Handle an enum member where the RHS is just a string literal (not omitted, not a number, and
	 * not a complex expression). This is the typical form for TS string enums, and in this case, we
	 * do *not* create a reverse mapping.
	 *
	 * This is called after deleting the key token, when the token processor is at the equals sign.
	 *
	 * Example 1:
	 * someKey = "some value"
	 * ->
	 * const someKey = "some value"; MyEnum["someKey"] = someKey;
	 *
	 * Example 2:
	 * "some key" = "some value"
	 * ->
	 * MyEnum["some key"] = "some value";
	 */
	processStringLiteralEnumMember(enumName, nameStringCode, variableName) {
		if (variableName != null) {
			this.tokens.appendCode(`const ${variableName}`);
			// =
			this.tokens.copyToken();
			// value string
			this.tokens.copyToken();
			this.tokens.appendCode(`; ${enumName}[${nameStringCode}] = ${variableName};`);
		} else {
			this.tokens.appendCode(`${enumName}[${nameStringCode}]`);
			// =
			this.tokens.copyToken();
			// value string
			this.tokens.copyToken();
			this.tokens.appendCode(";");
		}
	}

	/**
	 * Handle an enum member initialized with an expression on the right-hand side (other than a
	 * string literal). In these cases, we should transform the expression and emit code that sets up
	 * a reverse mapping.
	 *
	 * The TypeScript implementation of this operation distinguishes between expressions that can be
	 * "constant folded" at compile time (i.e. consist of number literals and simple math operations
	 * on those numbers) and ones that are dynamic. For constant expressions, it emits the resolved
	 * numeric value, and auto-incrementing is only allowed in that case. Evaluating expressions at
	 * compile time would add significant complexity to Sucrase, so Sucrase instead leaves the
	 * expression as-is, and will later emit something like `MyEnum["previousKey"] + 1` to implement
	 * auto-incrementing.
	 *
	 * This is called after deleting the key token, when the token processor is at the equals sign.
	 *
	 * Example 1:
	 * someKey = 1 + 1
	 * ->
	 * const someKey = 1 + 1; MyEnum[MyEnum["someKey"] = someKey] = "someKey";
	 *
	 * Example 2:
	 * "some key" = 1 + 1
	 * ->
	 * MyEnum[MyEnum["some key"] = 1 + 1] = "some key";
	 */
	processExplicitValueEnumMember(enumName, nameStringCode, variableName) {
		const rhsEndIndex = this.tokens.currentToken().rhsEndIndex;
		if (rhsEndIndex == null) {
			throw new Error("Expected rhsEndIndex on enum assign.");
		}

		if (variableName != null) {
			this.tokens.appendCode(`const ${variableName}`);
			this.tokens.copyToken();
			while (this.tokens.currentIndex() < rhsEndIndex) {
				this.rootTransformer.processToken();
			}
			this.tokens.appendCode(`; ${enumName}[${enumName}[${nameStringCode}] = ${variableName}] = ${nameStringCode};`);
		} else {
			this.tokens.appendCode(`${enumName}[${enumName}[${nameStringCode}]`);
			this.tokens.copyToken();
			while (this.tokens.currentIndex() < rhsEndIndex) {
				this.rootTransformer.processToken();
			}
			this.tokens.appendCode(`] = ${nameStringCode};`);
		}
	}

	/**
	 * Handle an enum member with no right-hand side expression. In this case, the value is the
	 * previous value plus 1, or 0 if there was no previous value. We should also always emit a
	 * reverse mapping.
	 *
	 * Example 1:
	 * someKey2
	 * ->
	 * const someKey2 = someKey1 + 1; MyEnum[MyEnum["someKey2"] = someKey2] = "someKey2";
	 *
	 * Example 2:
	 * "some key 2"
	 * ->
	 * MyEnum[MyEnum["some key 2"] = someKey1 + 1] = "some key 2";
	 */
	processImplicitValueEnumMember(enumName, nameStringCode, variableName, previousValueCode) {
		let valueCode = previousValueCode != null ? `${previousValueCode} + 1` : "0";
		if (variableName != null) {
			this.tokens.appendCode(`const ${variableName} = ${valueCode}; `);
			valueCode = variableName;
		}
		this.tokens.appendCode(`${enumName}[${enumName}[${nameStringCode}] = ${valueCode}] = ${nameStringCode};`);
	}
}
