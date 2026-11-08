import * as native from "qbun:sqlite";
import { constants as bunConstants, type NativeDatabase, type NativeStatement, type Changes } from "bun:sqlite";
import { codedError, invalidArgType } from "internal:errors";

export const constants = {
	...bunConstants,
	SQLITE_CHANGESET_OMIT: 0,
	SQLITE_CHANGESET_REPLACE: 1,
	SQLITE_CHANGESET_ABORT: 2,
	SQLITE_CHANGESET_DATA: 1,
	SQLITE_CHANGESET_NOTFOUND: 2,
	SQLITE_CHANGESET_CONFLICT: 3,
	SQLITE_CHANGESET_CONSTRAINT: 4,
	SQLITE_CHANGESET_FOREIGN_KEY: 5,
};
type Row = Record<string, unknown>;
interface Options {
	open?: boolean;
	readOnly?: boolean;
	enableForeignKeyConstraints?: boolean;
	enableDoubleQuotedStringLiterals?: boolean;
	allowExtension?: boolean;
	timeout?: number;
	readBigInts?: boolean;
	returnArrays?: boolean;
}
interface FunctionOptions {
	deterministic?: boolean;
	directOnly?: boolean;
	useBigIntArguments?: boolean;
	varargs?: boolean;
}
export class StatementSync {
	readonly #native: NativeStatement;
	#arrays = false;
	constructor(handle: NativeStatement) {
		this.#native = handle;
		handle.setAllowBareNamedParameters(true);
	}
	#params(args: unknown[]): unknown {
		if (!args.length) return [];
		const first = args[0];
		if (first !== null && typeof first === "object" && !ArrayBuffer.isView(first) && !(first instanceof ArrayBuffer)) {
			if (args.length > 1) return args;
			return first;
		}
		return args;
	}
	all(...args: unknown[]): Row[] | unknown[][] {
		return this.#arrays ? this.#native.values(this.#params(args)) : this.#native.all(this.#params(args));
	}
	get(...args: unknown[]): Row | unknown[] | undefined {
		if (!this.#arrays) return this.#native.get(this.#params(args)) ?? undefined;
		this.#native.bind(this.#params(args));
		try {
			return this.#native.stepArray() ?? undefined;
		} finally {
			this.#native.reset();
		}
	}
	run(...args: unknown[]): Changes {
		return this.#native.run(this.#params(args));
	}
	*iterate(...args: unknown[]): IterableIterator<Row | unknown[]> {
		this.#native.bind(this.#params(args));
		try {
			for (;;) {
				const row = this.#arrays ? this.#native.stepArray() : this.#native.step();
				if (row === null) break;
				yield row;
			}
		} finally {
			this.#native.reset();
		}
	}
	setReadBigInts(enabled: boolean): void {
		if (typeof enabled !== "boolean") throw invalidArgType("enabled", "a boolean", enabled);
		this.#native.setReadBigInts(enabled);
	}
	setAllowBareNamedParameters(enabled: boolean): void {
		if (typeof enabled !== "boolean") throw invalidArgType("enabled", "a boolean", enabled);
		this.#native.setAllowBareNamedParameters(enabled);
	}
	setReturnArrays(enabled: boolean): void {
		if (typeof enabled !== "boolean") throw invalidArgType("enabled", "a boolean", enabled);
		this.#arrays = enabled;
	}
	get sourceSQL(): string {
		return this.#native.sourceSQL;
	}
	get expandedSQL(): string {
		return this.#native.expandedSQL;
	}
	columns(): {
		column: string | null;
		database: string | null;
		name: string;
		table: string | null;
		type: string | null;
	}[] {
		return this.#native.columns();
	}
}
export class DatabaseSync {
	#handle: NativeDatabase | undefined;
	readonly #path: string;
	readonly #options: Options;
	#extensionAllowed: boolean;
	constructor(path: string | URL | Uint8Array, options: Options = {}) {
		if (path instanceof URL) {
			if (path.protocol !== "file:") throw codedError("ERR_INVALID_URL_SCHEME", "The URL must be of scheme file");
			path = decodeURIComponent(path.pathname);
		}
		if (path instanceof Uint8Array) path = new TextDecoder().decode(path);
		if (typeof path !== "string") throw invalidArgType("path", "a string, Buffer, or URL", path);
		this.#path = path;
		this.#options = options;
		this.#extensionAllowed = !!options.allowExtension;
		if (options.open !== false) this.open();
	}
	open(): void {
		if (this.#handle?.isOpen) throw codedError("ERR_INVALID_STATE", "Database is already open");
		const opts = this.#options;
		this.#handle = native.open(this.#path, (opts.readOnly ? 1 : 6) | 64, !!opts.readBigInts) as NativeDatabase;
		this.#handle.exec(`PRAGMA foreign_keys=${opts.enableForeignKeyConstraints === false ? "OFF" : "ON"}`);
		this.#handle.enableLoadExtension(this.#extensionAllowed);
		if (opts.timeout !== undefined) this.#handle.busyTimeout(opts.timeout);
		this.#handle.configureDoubleQuotedStrings(!!opts.enableDoubleQuotedStringLiterals);
	}
	#db(): NativeDatabase {
		if (!this.#handle?.isOpen) throw codedError("ERR_INVALID_STATE", "Database is not open");
		return this.#handle;
	}
	close(): void {
		this.#db().close(false);
	}
	exec(sql: string): void {
		if (typeof sql !== "string") throw invalidArgType("sql", "a string", sql);
		this.#db().exec(sql);
	}
	prepare(sql: string): StatementSync {
		if (typeof sql !== "string") throw invalidArgType("sql", "a string", sql);
		const stmt = new StatementSync(this.#db().prepare(sql));
		if (this.#options.returnArrays) stmt.setReturnArrays(true);
		return stmt;
	}
	function(
		name: string,
		options: FunctionOptions | ((...args: unknown[]) => unknown),
		fn?: (...args: unknown[]) => unknown,
	): void {
		if (typeof options === "function") {
			fn = options;
			options = {};
		}
		if (typeof name !== "string") throw invalidArgType("name", "a string", name);
		if (typeof fn !== "function") throw invalidArgType("function", "a function", fn);
		this.#db().function(name, options as Record<string, unknown>, fn);
	}
	get isOpen(): boolean {
		return !!this.#handle?.isOpen;
	}
	get isTransaction(): boolean {
		return this.#db().inTransaction;
	}
	location(dbName = "main"): string | null {
		return this.#db().location(dbName);
	}
	enableLoadExtension(allow: boolean): void {
		if (allow && !this.#extensionAllowed)
			throw codedError("ERR_INVALID_STATE", "Extension loading cannot be enabled because it was disabled at creation");
		this.#db().enableLoadExtension(allow);
	}
	loadExtension(path: string): void {
		this.#db().loadExtension(path);
	}
	[Symbol.dispose](): void {
		if (this.isOpen) this.close();
	}
}
DatabaseSync.prototype[Symbol.for("sqlite-type")] = "node:sqlite";
export default { DatabaseSync, StatementSync, constants };
