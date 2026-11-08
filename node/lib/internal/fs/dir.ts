// internal:fs/dir — fs.Dir (opendir) over the native uv_dir_t handle.

import * as pathMod from "node:path";
import { bind } from "internal:async-context";
import { codedError, validateFunction } from "internal:errors";
import { type BufferEncodingOpt, Dirent, encodePath, UV_DIRENT_UNKNOWN, direntTypeFromMode } from "internal:fs/utils";
import { call, sync } from "internal:fs/ops";

interface DirHandle {
	read(n: number, cb?: (err: Error | null, res: [string[], number[]] | null) => void): [string[], number[]] | null;
	close(cb?: (err: Error | null) => void): void;
}

export interface OpendirOptions {
	encoding?: BufferEncodingOpt;
	bufferSize?: number;
	recursive?: boolean;
}

export class Dir {
	#handle: DirHandle;
	#path: string;
	#encoding: BufferEncodingOpt;
	#bufferSize: number;
	#recursive: boolean;
	#buffered: Dirent[] = [];
	#closed = false;
	#pending: Promise<unknown> = Promise.resolve();
	#subdirs: { path: string; handle: DirHandle }[] = [];

	constructor(handle: DirHandle, path: string, options: OpendirOptions) {
		this.#handle = handle;
		this.#path = path;
		this.#encoding = options.encoding ?? "utf8";
		this.#bufferSize = options.bufferSize ?? 32;
		this.#recursive = !!options.recursive;
	}

	get path(): string {
		return this.#path;
	}

	#checkClosed(): void {
		if (this.#closed) throw codedError("ERR_DIR_CLOSED", "Directory handle was closed");
	}

	#current(): { path: string; handle: DirHandle } {
		return this.#subdirs.length ? this.#subdirs[this.#subdirs.length - 1] : { path: this.#path, handle: this.#handle };
	}

	#process(dir: string, res: [string[], number[]]): void {
		const [names, types] = res;
		for (let i = 0; i < names.length; i++) {
			let type = types[i];
			if (type === UV_DIRENT_UNKNOWN) {
				try {
					type = direntTypeFromMode(sync<number[]>("lstat", pathMod.join(dir, names[i]), false)[1]);
				} catch {
					// keep unknown
				}
			}
			const dirent = new Dirent(encodePath(names[i], this.#encoding), type, dir);
			this.#buffered.push(dirent);
			if (this.#recursive && dirent.isDirectory()) {
				const sub = pathMod.join(dir, names[i]);
				this.#subdirs.unshift({ path: sub, handle: sync<DirHandle>("opendir", sub) });
			}
		}
	}

	readSync(): Dirent | null {
		this.#checkClosed();
		for (;;) {
			const next = this.#buffered.shift();
			if (next) return next;
			const cur = this.#current();
			const res = cur.handle.read(this.#bufferSize);
			if (res) {
				this.#process(cur.path, res);
				continue;
			}
			if (!this.#subdirs.length) return null;
			this.#subdirs.pop();
			cur.handle.close();
		}
	}

	async #readAsync(): Promise<Dirent | null> {
		for (;;) {
			const next = this.#buffered.shift();
			if (next) return next;
			const cur = this.#current();
			const { promise, resolve, reject } = Promise.withResolvers<[string[], number[]] | null>();
			cur.handle.read(this.#bufferSize, (err, r) => (err ? reject(err) : resolve(r)));
			const res = await promise;
			if (res) {
				this.#process(cur.path, res);
				continue;
			}
			if (!this.#subdirs.length) return null;
			this.#subdirs.pop();
			cur.handle.close();
		}
	}

	read(): Promise<Dirent | null>;
	read(callback: (err: Error | null, dirent?: Dirent | null) => void): void;
	read(callback?: (err: Error | null, dirent?: Dirent | null) => void): Promise<Dirent | null> | void {
		if (callback !== undefined) validateFunction(callback, "callback");
		this.#checkClosed();
		const p = this.#pending.then(() => this.#readAsync());
		this.#pending = p.catch(() => undefined);
		if (!callback) return p;
		const cb = bind(callback);
		p.then(
			(d) => cb(null, d),
			(e: Error) => cb(e),
		);
	}

	closeSync(): void {
		this.#checkClosed();
		this.#closed = true;
		for (const s of this.#subdirs) s.handle.close();
		this.#subdirs = [];
		this.#handle.close();
	}

	close(): Promise<void>;
	close(callback: (err: Error | null) => void): void;
	close(callback?: (err: Error | null) => void): Promise<void> | void {
		if (callback !== undefined) validateFunction(callback, "callback");
		let p: Promise<void>;
		if (this.#closed) {
			p = Promise.reject(codedError("ERR_DIR_CLOSED", "Directory handle was closed"));
		} else {
			this.#closed = true;
			p = this.#pending.then(() => {
				for (const s of this.#subdirs) s.handle.close();
				this.#subdirs = [];
				const { promise, resolve, reject } = Promise.withResolvers<void>();
				this.#handle.close((err) => (err ? reject(err) : resolve()));
				return promise;
			});
		}
		if (!callback) return p;
		const cb = bind(callback);
		p.then(
			() => cb(null),
			(e: Error) => cb(e),
		);
	}

	async *entries(): AsyncGenerator<Dirent> {
		try {
			for (;;) {
				const d = await this.read();
				if (d === null) break;
				yield d;
			}
		} finally {
			if (!this.#closed) await this.close();
		}
	}

	[Symbol.asyncIterator](): AsyncGenerator<Dirent> {
		return this.entries();
	}

	*[Symbol.iterator](): Generator<Dirent> {
		try {
			for (;;) {
				const d = this.readSync();
				if (d === null) break;
				yield d;
			}
		} finally {
			if (!this.#closed) this.closeSync();
		}
	}

	[Symbol.dispose](): void {
		if (!this.#closed) this.closeSync();
	}

	async [Symbol.asyncDispose](): Promise<void> {
		if (!this.#closed) await this.close();
	}
}

export function opendirSyncImpl(path: string | Uint8Array, display: string, options: OpendirOptions): Dir {
	return new Dir(sync<DirHandle>("opendir", path), display, options);
}

export async function opendirImpl(path: string | Uint8Array, display: string, options: OpendirOptions): Promise<Dir> {
	return new Dir(await call<DirHandle>("opendir", path), display, options);
}
