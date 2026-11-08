import { notSupported } from "internal:errors";
export class WASI {
	readonly wasiImport = {};
	constructor(_options: object = {}) {}
	start(_instance: unknown): never {
		throw notSupported("WebAssembly WASI");
	}
	initialize(_instance: unknown): never {
		throw notSupported("WebAssembly WASI");
	}
	getImportObject(): object {
		return { wasi_snapshot_preview1: this.wasiImport };
	}
}
export default { WASI };
