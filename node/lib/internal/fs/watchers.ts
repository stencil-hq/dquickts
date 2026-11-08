// internal:fs/watchers — fs.watch (FSWatcher), fs.watchFile (StatWatcher)
// and fs.promises.watch, over the native uv_fs_event_t / uv_fs_poll_t handles.

import * as binding from "qbun:fs";
import * as pathMod from "node:path";
import process from "node:process";
import { EventEmitter } from "node:events";
import { Buffer } from "node:buffer";
import { bind } from "internal:async-context";
import { codedError } from "internal:errors";
import {
	type BufferEncodingOpt,
	type StatsArray,
	Stats,
	BigIntStats,
	getStatsFromBinding,
	encodePath,
	abortErr,
} from "internal:fs/utils";
import { sync } from "internal:fs/ops";

interface WatchHandle {
	start(path: string | Uint8Array, flagOrInterval: boolean | number): void;
	close(): void;
	ref(): void;
	unref(): void;
	hasRef(): boolean;
	getPath(): string | undefined;
}

type EventCb = (err: Error | null, eventType?: string, filename?: string | null) => void;
type PollCb = (status: number | null, curr: StatsArray, prev: StatsArray) => void;

const createWatcher = binding.createWatcher as (cb: EventCb) => WatchHandle;
const createStatWatcher = binding.createStatWatcher as (cb: PollCb, bigint: boolean) => WatchHandle;

const nativeRecursive = process.platform === "darwin" || process.platform === "win32";

export interface WatchOptions {
	persistent?: boolean;
	recursive?: boolean;
	encoding?: BufferEncodingOpt;
	signal?: AbortSignal;
}

export class FSWatcher extends EventEmitter {
	#handles = new Map<string, WatchHandle>();
	#root = "";
	#encoding: BufferEncodingOpt = "utf8";
	#persistent = true;
	#closed = false;
	#emulated = false;

	/** start(path, persistent, recursive, encoding) — Node's internal signature. */
	start(
		filename: string | Uint8Array,
		persistent = true,
		recursive = false,
		encoding: BufferEncodingOpt = "utf8",
	): void {
		const root = typeof filename === "string" ? filename : Buffer.from(filename).toString();
		this.#root = root;
		this.#encoding = encoding;
		this.#persistent = persistent;
		if (recursive && !nativeRecursive) {
			let isDir = false;
			try {
				isDir = (getStatsFromBinding(sync<StatsArray>("stat", root, false)) as Stats).isDirectory();
			} catch {
				isDir = false;
			}
			if (isDir) {
				this.#emulated = true;
				this.#watchTree(root);
				return;
			}
		}
		this.#watchOne(root, "", recursive);
	}

	#watchOne(path: string, rel: string, recursive: boolean): void {
		const handle = createWatcher((err, eventType, filename) => {
			if (err) {
				this.#onError(err);
				return;
			}
			let name = filename ?? null;
			if (this.#emulated && name !== null) {
				name = rel ? pathMod.join(rel, name) : name;
				if (eventType === "rename") this.#maybeAddDir(pathMod.join(this.#root, name), name);
			}
			this.emit("change", eventType, name === null ? null : encodePath(name, this.#encoding));
		});
		try {
			handle.start(path, recursive);
		} catch (e) {
			handle.close();
			if (this.#handles.size === 0) throw e;
			return;
		}
		if (!this.#persistent) handle.unref();
		this.#handles.set(path, handle);
	}

	#watchTree(dir: string): void {
		const rel = pathMod.relative(this.#root, dir);
		this.#watchOne(dir, rel, false);
		let entries: [string[], number[]];
		try {
			entries = sync<[string[], number[]]>("scandir", dir);
		} catch {
			return;
		}
		const DIR = binding.constants.UV_DIRENT_DIR as number;
		for (let i = 0; i < entries[0].length; i++) {
			if (entries[1][i] === DIR) this.#watchTree(pathMod.join(dir, entries[0][i]));
		}
	}

	#maybeAddDir(abs: string, _rel: string): void {
		if (this.#handles.has(abs)) {
			try {
				sync("stat", abs, false);
			} catch {
				for (const [p, h] of this.#handles) {
					if (p === abs || p.startsWith(abs + pathMod.sep)) {
						h.close();
						this.#handles.delete(p);
					}
				}
			}
			return;
		}
		try {
			if ((getStatsFromBinding(sync<StatsArray>("lstat", abs, false)) as Stats).isDirectory()) this.#watchTree(abs);
		} catch {
			// gone again
		}
	}

	#onError(err: Error): void {
		this.close();
		this.emit("error", err);
	}

	close(): void {
		if (this.#closed) return;
		this.#closed = true;
		for (const h of this.#handles.values()) h.close();
		this.#handles.clear();
		process.nextTick(() => this.emit("close"));
	}

	ref(): this {
		for (const h of this.#handles.values()) h.ref();
		return this;
	}

	unref(): this {
		for (const h of this.#handles.values()) h.unref();
		return this;
	}
}

export function watchImpl(
	filename: string | Uint8Array,
	options: WatchOptions,
	listener?: (eventType: string, filename: string | Buffer | null) => void,
): FSWatcher {
	const watcher = new FSWatcher();
	watcher.start(filename, options.persistent !== false, !!options.recursive, options.encoding ?? "utf8");
	if (listener) watcher.addListener("change", bind(listener));
	const signal = options.signal;
	if (signal) {
		if (signal.aborted) process.nextTick(() => watcher.close());
		else {
			const onAbort = () => watcher.close();
			signal.addEventListener("abort", onAbort, { once: true });
			watcher.once("close", () => signal.removeEventListener("abort", onAbort));
		}
	}
	return watcher;
}

/** fs.promises.watch: an async iterator of { eventType, filename }. */
export function watchAsync(
	filename: string | Uint8Array,
	options: WatchOptions & { maxQueue?: number; overflow?: "ignore" | "throw" },
): AsyncIterableIterator<{ eventType: string; filename: string | Buffer | null }> {
	type Ev = { eventType: string; filename: string | Buffer | null };
	const queue: Ev[] = [];
	const maxQueue = options.maxQueue ?? 2048;
	let waiter: PromiseWithResolvers<IteratorResult<Ev>> | null = null;
	let error: Error | null = null;
	let done = false;
	const watcher = new FSWatcher();
	watcher.start(filename, options.persistent !== false, !!options.recursive, options.encoding ?? "utf8");
	const finish = (err: Error | null) => {
		if (done) return;
		done = true;
		error = err;
		watcher.close();
		if (waiter) {
			const w = waiter;
			waiter = null;
			if (err) w.reject(err);
			else w.resolve({ value: undefined, done: true });
		}
	};
	watcher.on("change", (eventType: string, name: string | Buffer | null) => {
		const ev = { eventType, filename: name };
		if (waiter) {
			const w = waiter;
			waiter = null;
			w.resolve({ value: ev, done: false });
			return;
		}
		if (queue.length >= maxQueue) {
			if (options.overflow === "throw") {
				finish(codedError("ERR_FS_WATCH_QUEUE_OVERFLOW", `fs.watch maxQueue exceeded`));
			}
			return;
		}
		queue.push(ev);
	});
	watcher.on("error", (err: Error) => finish(err));
	const signal = options.signal;
	if (signal) {
		if (signal.aborted) finish(abortErr(signal));
		else signal.addEventListener("abort", () => finish(abortErr(signal)), { once: true });
	}
	const it: AsyncIterableIterator<Ev> = {
		next(): Promise<IteratorResult<Ev>> {
			const ev = queue.shift();
			if (ev) return Promise.resolve({ value: ev, done: false });
			if (done) return error ? Promise.reject(error) : Promise.resolve({ value: undefined, done: true });
			waiter = Promise.withResolvers<IteratorResult<Ev>>();
			return waiter.promise;
		},
		return(): Promise<IteratorResult<Ev>> {
			finish(null);
			return Promise.resolve({ value: undefined, done: true });
		},
		throw(err: unknown): Promise<IteratorResult<Ev>> {
			finish(err as Error);
			return Promise.reject(err);
		},
		[Symbol.asyncIterator]() {
			return it;
		},
	};
	return it;
}

// ---------------------------------------------------------------- watchFile

export class StatWatcher extends EventEmitter {
	#handle: WatchHandle | null = null;
	#bigint: boolean;

	constructor(bigint = false) {
		super();
		this.#bigint = bigint;
	}

	start(filename: string | Uint8Array, persistent: boolean, interval: number): void {
		if (this.#handle) return;
		const bigint = this.#bigint;
		const handle = createStatWatcher((status, curr, prev) => {
			const c = getStatsFromBinding(curr, bigint);
			const p = getStatsFromBinding(prev, bigint);
			// Node reports zeroed stats on errors and only on transitions
			if (status !== null && status < 0 && Number(p.nlink) === 0 && Number(c.nlink) === 0 && this.#reportedMissing)
				return;
			this.#reportedMissing = status !== null && status < 0;
			this.emit("change", c, p);
		}, bigint);
		handle.start(filename, interval);
		if (!persistent) handle.unref();
		this.#handle = handle;
	}

	#reportedMissing = false;

	stop(): void {
		if (!this.#handle) return;
		this.#handle.close();
		this.#handle = null;
		process.nextTick(() => this.emit("stop"));
	}

	ref(): this {
		this.#handle?.ref();
		return this;
	}

	unref(): this {
		this.#handle?.unref();
		return this;
	}
}

const statWatchers = new Map<string, StatWatcher>();

export function watchFileImpl(
	filename: string,
	options: { persistent?: boolean; interval?: number; bigint?: boolean },
	listener: (curr: Stats | BigIntStats, prev: Stats | BigIntStats) => void,
): StatWatcher {
	const key = pathMod.resolve(filename);
	let watcher = statWatchers.get(key);
	if (!watcher) {
		watcher = new StatWatcher(!!options.bigint);
		watcher.start(filename, options.persistent !== false, options.interval ?? 5007);
		statWatchers.set(key, watcher);
	}
	watcher.addListener("change", listener);
	return watcher;
}

export function unwatchFileImpl(filename: string, listener?: (...args: unknown[]) => void): void {
	const key = pathMod.resolve(filename);
	const watcher = statWatchers.get(key);
	if (!watcher) return;
	if (typeof listener === "function") watcher.removeListener("change", listener);
	else watcher.removeAllListeners("change");
	if (watcher.listenerCount("change") === 0) {
		watcher.stop();
		statWatchers.delete(key);
	}
}
