// node:module — the CommonJS Module class and createRequire().

import Module from "internal:cjs";
import { builtinModules as builtins, isBuiltin as isNodeBuiltin } from "internal:resolve";

export const createRequire = Module.createRequire;
export const builtinModules = builtins;
export const isBuiltin = (name: string): boolean => isNodeBuiltin(name) || name === "bun" || name.startsWith("bun:");
export const findSourceMap = Module.findSourceMap;
export const register = Module.register;
export const syncBuiltinESMExports = Module.syncBuiltinESMExports;
export const enableCompileCache = Module.enableCompileCache;
export const getCompileCacheDir = Module.getCompileCacheDir;
export const constants = { compileCacheStatus: { FAILED: 0, ENABLED: 1, ALREADY_ENABLED: 2, DISABLED: 3 } };

/** Source maps are not generated: SourceMap only stores the payload. */
export class SourceMap {
	payload: unknown;
	constructor(payload: unknown) {
		this.payload = payload;
	}
	findEntry(): Record<string, never> {
		return {};
	}
}

export { Module };
export default Module;
