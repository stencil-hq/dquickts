import { builtinModules } from "node:module";
import { notSupported } from "internal:errors";
export { builtinModules };
export function start(): never {
	throw notSupported("REPL");
}
export class Recoverable extends SyntaxError {
	err: Error;
	constructor(error: Error) {
		super(error.message);
		this.err = error;
	}
}
export const REPL_MODE_SLOPPY = Symbol("repl-sloppy"),
	REPL_MODE_STRICT = Symbol("repl-strict");
export default { start, builtinModules, Recoverable, REPL_MODE_SLOPPY, REPL_MODE_STRICT };
