// Vendored from sucrase 3.35.1 (dist/esm), MIT License, Copyright (c) 2012-2018 various
// contributors (see https://github.com/alangpierce/sucrase). Imports rewritten to internal: specifiers.

export default function getJSXPragmaInfo(options) {
	const [base, suffix] = splitPragma(options.jsxPragma || "React.createElement");
	const [fragmentBase, fragmentSuffix] = splitPragma(options.jsxFragmentPragma || "React.Fragment");
	return { base, suffix, fragmentBase, fragmentSuffix };
}

function splitPragma(pragma) {
	let dotIndex = pragma.indexOf(".");
	if (dotIndex === -1) {
		dotIndex = pragma.length;
	}
	return [pragma.slice(0, dotIndex), pragma.slice(dotIndex)];
}
