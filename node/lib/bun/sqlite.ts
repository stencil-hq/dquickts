import * as native from "qbun:sqlite";

export type SQLQueryBinding = string | number | bigint | boolean | null | undefined | ArrayBuffer | ArrayBufferView;
export type SQLQueryBindings = SQLQueryBinding | Record<string, SQLQueryBinding>;
type Row = Record<string, unknown>;
export interface Changes {
	changes: number;
	lastInsertRowid: number | bigint;
}
export interface DatabaseOptions {
	readonly?: boolean;
	create?: boolean;
	readwrite?: boolean;
	strict?: boolean;
	safeIntegers?: boolean;
}
export interface NativeStatement {
	all(params?: unknown): Row[];
	get(params?: unknown): Row | null;
	run(params?: unknown): Changes;
	values(params?: unknown): unknown[][];
	bind(params?: unknown): void;
	step(): Row | null;
	stepArray(): unknown[] | null;
	reset(): void;
	finalize(): void;
	setReadBigInts(value: boolean): void;
	setAllowBareNamedParameters(value: boolean): void;
	readonly readonly: boolean;
	readonly paramsCount: number;
	readonly columnNames: string[];
	readonly declaredTypes: (string | null)[];
	readonly columnTypes: string[];
	readonly sourceSQL: string;
	readonly expandedSQL: string;
	columns(): {
		column: string | null;
		database: string | null;
		name: string;
		table: string | null;
		type: string | null;
	}[];
}
export interface NativeDatabase {
	close(throwOnError?: boolean): void;
	exec(sql: string): void;
	prepare(sql: string, strict?: boolean): NativeStatement;
	readonly isOpen: boolean;
	readonly inTransaction: boolean;
	readonly filename: string;
	readonly changes: number;
	readonly lastInsertRowid: number | bigint;
	serialize(schema?: string): Uint8Array;
	deserialize(bytes: ArrayBuffer | ArrayBufferView, readonly: boolean): void;
	loadExtension(path: string, entry?: string): void;
	enableLoadExtension(value: boolean): void;
	busyTimeout(ms: number): void;
	readonly address: number;
	fileControl(op: number, value?: number | bigint | ArrayBufferView | null, schema?: string): number;
	location(schema: string): string | null;
	configureDoubleQuotedStrings(enabled: boolean): void;
	function(name: string, options: Record<string, unknown>, fn: (...args: unknown[]) => unknown): void;
}
function parameters(args: unknown[]): unknown {
	if (!args.length) return undefined;
	if (
		args.length === 1 &&
		args[0] !== null &&
		typeof args[0] === "object" &&
		!ArrayBuffer.isView(args[0]) &&
		!(args[0] instanceof ArrayBuffer)
	)
		return args[0];
	return args;
}
export class SQLiteError extends Error {
	declare errno: number;
	declare code: string;
	declare byteOffset: number;
	static [Symbol.hasInstance](value: unknown): boolean {
		return value instanceof Error && value.name === "SQLiteError";
	}
}
export class Statement<T = Row, P extends unknown[] = SQLQueryBindings[]> {
	readonly native: NativeStatement;
	#class: { prototype: object } | undefined;
	#finalized = false;
	#safe = false;
	constructor(handle: NativeStatement, safe = false) {
		this.native = handle;
		this.#safe = safe;
	}
	#map(row: Row | null): T | null {
		return row === null ? null : ((this.#class ? Object.assign(Object.create(this.#class.prototype), row) : row) as T);
	}
	all(...args: P): T[] {
		const rows = this.native.all(parameters(args));
		return this.#class ? rows.map((row) => this.#map(row) as T) : (rows as unknown as T[]);
	}
	get(...args: P): T | null {
		return this.#map(this.native.get(parameters(args)));
	}
	run(...args: P): Changes {
		return this.native.run(parameters(args));
	}
	values(...args: P): unknown[][] {
		return this.native.values(parameters(args));
	}
	*iterate(...args: P): IterableIterator<T> {
		this.native.bind(parameters(args));
		try {
			for (;;) {
				const row = this.native.step();
				if (row === null) break;
				yield this.#map(row) as T;
			}
		} finally {
			this.native.reset();
		}
	}
	[Symbol.iterator](): IterableIterator<T> {
		return this.iterate(...([] as unknown as P));
	}
	as<U>(Class: { prototype: U }): Statement<U, P> {
		if (typeof Class !== "function") throw new TypeError("Expected a constructor");
		this.#class = Class as { prototype: object };
		return this as unknown as Statement<U, P>;
	}
	finalize(): void {
		this.native.finalize();
		this.#finalized = true;
	}
	get isFinalized(): boolean {
		return this.#finalized;
	}
	get readonly(): boolean {
		return this.native.readonly;
	}
	get paramsCount(): number {
		return this.native.paramsCount;
	}
	get columnNames(): string[] {
		return this.native.columnNames;
	}
	get declaredTypes(): (string | null)[] {
		return this.native.declaredTypes;
	}
	get columnTypes(): string[] {
		return this.native.columnTypes;
	}
	toString(): string {
		return this.native.expandedSQL;
	}
	toJSON(): object {
		return {
			sql: this.toString(),
			isFinalized: this.isFinalized,
			paramsCount: this.paramsCount,
			columnNames: this.columnNames,
		};
	}
	safeIntegers(value?: boolean): boolean | this {
		if (value === undefined) return this.#safe;
		this.#safe = !!value;
		this.native.setReadBigInts(this.#safe);
		return this;
	}
	[Symbol.dispose](): void {
		this.finalize();
	}
}
type Transaction<A extends unknown[], T> = ((...args: A) => T) & {
	deferred: Transaction<A, T>;
	immediate: Transaction<A, T>;
	exclusive: Transaction<A, T>;
	default: Transaction<A, T>;
	database: Database;
};
export class Database {
	static MAX_QUERY_CACHE_SIZE = 100;
	readonly filename: string;
	readonly #handle: NativeDatabase;
	get handle(): number {
		return this.#handle.address;
	}
	#strict: boolean;
	#safe: boolean;
	#cache = new Map<string, Statement>();
	#owned = new Set<Statement>();
	#savepoint = 0;
	constructor(filename: string | ArrayBuffer | ArrayBufferView = ":memory:", options: DatabaseOptions | number = {}) {
		const opts = typeof options === "object" && options !== null ? options : {};
		if ("readOnly" in opts) throw new TypeError('Misspelled option "readOnly" should be "readonly"');
		let flags = 6;
		if (typeof options === "number") flags = options;
		else if (opts.readonly || opts.readwrite === false) {
			if (opts.create || opts.readwrite) throw new TypeError("Read-only databases cannot be created or readwrite");
			flags = 1;
		} else if (opts.create === false || opts.readwrite) flags = 2;
		this.#strict = !!opts.strict;
		this.#safe = !!opts.safeIntegers;
		const bytes = typeof filename !== "string";
		if (bytes && !(filename instanceof ArrayBuffer) && !ArrayBuffer.isView(filename))
			throw new TypeError("Expected filename to be a string or byte buffer");
		this.filename = bytes ? ":memory:" : (filename as string).trim() || ":memory:";
		if (!bytes && this.filename === ":memory:" && flags & 1)
			throw new Error("Cannot open an anonymous database in read-only mode.");
		this.#handle = native.open(this.filename, (bytes ? 6 : flags) | 64, !!opts.safeIntegers) as NativeDatabase;
		if (bytes) this.#handle.deserialize(filename as ArrayBuffer | ArrayBufferView, !!opts.readonly);
	}
	static open(filename?: string, options?: DatabaseOptions | number): Database {
		return new Database(filename, options);
	}
	static deserialize(bytes: ArrayBuffer | ArrayBufferView, options: boolean | DatabaseOptions = false): Database {
		return new Database(bytes, typeof options === "boolean" ? { readonly: options } : options);
	}
	get inTransaction(): boolean {
		return this.#handle.inTransaction;
	}
	prepare<T = Row, P extends unknown[] = SQLQueryBindings[]>(sql: string, params?: unknown): Statement<T, P> {
		if (typeof sql !== "string") throw new TypeError("Expected SQL to be a string");
		const stmt = new Statement<T, P>(this.#handle.prepare(sql, this.#strict), this.#safe);
		if (params !== undefined) stmt.native.bind(params);
		return stmt;
	}
	query<T = Row, P extends unknown[] = SQLQueryBindings[]>(sql: string): Statement<T, P> {
		let stmt = this.#cache.get(sql);
		if (stmt && !stmt.isFinalized) {
			this.#cache.delete(sql);
			this.#cache.set(sql, stmt);
			return stmt as unknown as Statement<T, P>;
		}
		stmt = this.prepare(sql);
		this.#owned.add(stmt);
		if (Database.MAX_QUERY_CACHE_SIZE > 0 && sql.length <= 65536) {
			if (this.#cache.size >= Database.MAX_QUERY_CACHE_SIZE)
				this.#cache.delete(this.#cache.keys().next().value as string);
			this.#cache.set(sql, stmt);
		}
		return stmt as unknown as Statement<T, P>;
	}
	run(sql: string, ...args: unknown[]): Changes {
		if (!args.length) {
			this.#handle.exec(sql);
			return { changes: this.#handle.changes, lastInsertRowid: this.#handle.lastInsertRowid };
		}
		const stmt = this.prepare(sql);
		try {
			return stmt.run(...(args as SQLQueryBindings[]));
		} finally {
			stmt.finalize();
		}
	}
	exec(sql: string, ...args: unknown[]): Changes {
		return this.run(sql, ...args);
	}
	clearQueryCache(): void {
		for (const stmt of this.#cache.values()) {
			stmt.finalize();
			this.#owned.delete(stmt);
		}
		this.#cache.clear();
	}
	close(throwOnError = false): void {
		for (const stmt of this.#owned) stmt.finalize();
		this.#owned.clear();
		this.#cache.clear();
		this.#handle.close(throwOnError);
	}
	serialize(schema = "main"): Uint8Array {
		return this.#handle.serialize(schema);
	}
	loadExtension(path: string, entry?: string): void {
		this.#handle.enableLoadExtension(true);
		this.#handle.loadExtension(path, entry);
	}
	fileControl(
		schemaOrOp: string | number,
		opOrValue?: number | bigint | ArrayBufferView | null,
		value?: number | bigint | ArrayBufferView | null,
	): number {
		return typeof schemaOrOp === "string"
			? this.#handle.fileControl(opOrValue as number, value, schemaOrOp)
			: this.#handle.fileControl(schemaOrOp, opOrValue);
	}
	function(
		name: string,
		options: Record<string, unknown> | ((...args: unknown[]) => unknown),
		fn?: (...args: unknown[]) => unknown,
	): this {
		if (typeof options === "function") {
			fn = options;
			options = {};
		}
		if (!fn) throw new TypeError("Expected a function");
		this.#handle.function(name, { ...options, useBigIntArguments: options.safeIntegers ?? this.#safe }, fn);
		return this;
	}
	transaction<A extends unknown[], T>(fn: (...args: A) => T): Transaction<A, T> {
		if (typeof fn !== "function") throw new TypeError("Expected a function");
		const db = this;
		const wrap = (mode: string) =>
			function (this: unknown, ...args: A): T {
				const nested = db.inTransaction;
				const save = `qbun_tx_${++db.#savepoint}`;
				db.#handle.exec(nested ? `SAVEPOINT ${save}` : `BEGIN ${mode}`);
				try {
					const result = fn.apply(this, args);
					db.#handle.exec(nested ? `RELEASE ${save}` : "COMMIT");
					return result;
				} catch (error) {
					if (db.inTransaction) {
						db.#handle.exec(nested ? `ROLLBACK TO ${save}` : "ROLLBACK");
						if (nested) db.#handle.exec(`RELEASE ${save}`);
					}
					throw error;
				}
			};
		const result = {
			default: wrap("DEFERRED"),
			deferred: wrap("DEFERRED"),
			immediate: wrap("IMMEDIATE"),
			exclusive: wrap("EXCLUSIVE"),
			database: db,
		};
		for (const key of ["default", "deferred", "immediate", "exclusive"] as const) Object.assign(result[key], result);
		return result.default as Transaction<A, T>;
	}
	[Symbol.dispose](): void {
		this.close(true);
	}
}
Database.prototype[Symbol.for("sqlite-type")] = "bun:sqlite";
export const constants = {
	SQLITE_OPEN_READONLY: 1,
	SQLITE_OPEN_READWRITE: 2,
	SQLITE_OPEN_CREATE: 4,
	SQLITE_OPEN_DELETEONCLOSE: 8,
	SQLITE_OPEN_EXCLUSIVE: 16,
	SQLITE_OPEN_AUTOPROXY: 32,
	SQLITE_OPEN_URI: 64,
	SQLITE_OPEN_MEMORY: 128,
	SQLITE_OPEN_MAIN_DB: 256,
	SQLITE_OPEN_TEMP_DB: 512,
	SQLITE_OPEN_TRANSIENT_DB: 1024,
	SQLITE_OPEN_MAIN_JOURNAL: 2048,
	SQLITE_OPEN_TEMP_JOURNAL: 4096,
	SQLITE_OPEN_SUBJOURNAL: 8192,
	SQLITE_OPEN_SUPER_JOURNAL: 16384,
	SQLITE_OPEN_NOMUTEX: 32768,
	SQLITE_OPEN_FULLMUTEX: 65536,
	SQLITE_OPEN_SHAREDCACHE: 131072,
	SQLITE_OPEN_PRIVATECACHE: 262144,
	SQLITE_OPEN_WAL: 524288,
	SQLITE_OPEN_NOFOLLOW: 16777216,
	SQLITE_OPEN_EXRESCODE: 33554432,
	SQLITE_PREPARE_PERSISTENT: 1,
	SQLITE_PREPARE_NORMALIZE: 2,
	SQLITE_PREPARE_NO_VTAB: 4,
	SQLITE_DESERIALIZE_READONLY: 4,
	SQLITE_FCNTL_LOCKSTATE: 1,
	SQLITE_FCNTL_GET_LOCKPROXYFILE: 2,
	SQLITE_FCNTL_SET_LOCKPROXYFILE: 3,
	SQLITE_FCNTL_LAST_ERRNO: 4,
	SQLITE_FCNTL_SIZE_HINT: 5,
	SQLITE_FCNTL_CHUNK_SIZE: 6,
	SQLITE_FCNTL_FILE_POINTER: 7,
	SQLITE_FCNTL_SYNC_OMITTED: 8,
	SQLITE_FCNTL_WIN32_AV_RETRY: 9,
	SQLITE_FCNTL_PERSIST_WAL: 10,
	SQLITE_FCNTL_OVERWRITE: 11,
	SQLITE_FCNTL_VFSNAME: 12,
	SQLITE_FCNTL_POWERSAFE_OVERWRITE: 13,
	SQLITE_FCNTL_PRAGMA: 14,
	SQLITE_FCNTL_BUSYHANDLER: 15,
	SQLITE_FCNTL_TEMPFILENAME: 16,
	SQLITE_FCNTL_MMAP_SIZE: 18,
	SQLITE_FCNTL_TRACE: 19,
	SQLITE_FCNTL_HAS_MOVED: 20,
	SQLITE_FCNTL_SYNC: 21,
	SQLITE_FCNTL_COMMIT_PHASETWO: 22,
	SQLITE_FCNTL_WIN32_SET_HANDLE: 23,
	SQLITE_FCNTL_WAL_BLOCK: 24,
	SQLITE_FCNTL_ZIPVFS: 25,
	SQLITE_FCNTL_RBU: 26,
	SQLITE_FCNTL_VFS_POINTER: 27,
	SQLITE_FCNTL_JOURNAL_POINTER: 28,
	SQLITE_FCNTL_WIN32_GET_HANDLE: 29,
	SQLITE_FCNTL_PDB: 30,
	SQLITE_FCNTL_BEGIN_ATOMIC_WRITE: 31,
	SQLITE_FCNTL_COMMIT_ATOMIC_WRITE: 32,
	SQLITE_FCNTL_ROLLBACK_ATOMIC_WRITE: 33,
	SQLITE_FCNTL_LOCK_TIMEOUT: 34,
	SQLITE_FCNTL_DATA_VERSION: 35,
	SQLITE_FCNTL_SIZE_LIMIT: 36,
	SQLITE_FCNTL_CKPT_DONE: 37,
	SQLITE_FCNTL_RESERVE_BYTES: 38,
	SQLITE_FCNTL_CKPT_START: 39,
	SQLITE_FCNTL_EXTERNAL_READER: 40,
	SQLITE_FCNTL_CKSM_FILE: 41,
	SQLITE_FCNTL_RESET_CACHE: 42,
};
export default Database;
