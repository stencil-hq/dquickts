// internal:transpile/errors — Bun's BuildMessage (transpiler diagnostics). Kept apart from the
// transformer so it can back a cheap lazy global.

export interface BuildMessagePosition {
	file: string;
	namespace: string;
	/** 1-based. */
	line: number;
	/** 0-based. */
	column: number;
	length: number;
	lineText: string;
	offset: number;
}

/** A transpiler/bundler diagnostic, thrown by Bun.Transpiler for syntax errors. */
export class BuildMessage extends Error {
	readonly level: "error" | "warning" | "info" | "debug" | "verbose" = "error";
	readonly notes: { text: string; position: BuildMessagePosition | null }[] = [];
	readonly position: BuildMessagePosition | null;

	constructor(message: string, position: BuildMessagePosition | null) {
		super(message);
		this.name = "BuildMessage";
		this.position = position;
	}
}
