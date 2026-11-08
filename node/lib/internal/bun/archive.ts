// Bun.Archive: in-memory tar/tar.gz creation, File enumeration, and safe extraction.
// Tar metadata is kept for extraction; files() intentionally includes only regular files.
import { gzipSync, gunzipSync } from "node:zlib";
import { promises as fs } from "node:fs";
import { extractTar } from "internal:archive/extract";
import { codedError, invalidArgType, outOfRange } from "internal:errors";
import { readTar, writeTar, type TarEntry } from "internal:archive/tar";
import { archiveFilter } from "internal:archive/filter";

type ByteInput = ArrayBufferLike | ArrayBufferView;
type ArchiveContent = string | Blob | ByteInput;
type ArchiveInput = Record<string, ArchiveContent> | Blob | ByteInput;
interface ArchiveOptions {
	compress?: "gzip";
	level?: number;
}
interface ArchiveExtractOptions {
	glob?: string | readonly string[];
}
interface SourceEntry {
	path: string;
	content: string | Blob | Uint8Array;
	mtime: number;
}

function bytesView(value: unknown): Uint8Array | undefined {
	if (value instanceof ArrayBuffer || (typeof SharedArrayBuffer !== "undefined" && value instanceof SharedArrayBuffer))
		return new Uint8Array(value);
	if (ArrayBuffer.isView(value)) return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
	return undefined;
}

function blobValue(value: unknown): value is Blob {
	if (value === null || typeof value !== "object") return false;
	return value instanceof Blob;
}

function compressionOptions(options: ArchiveOptions | undefined): { compress: boolean; level: number } {
	if (options !== undefined && (options === null || typeof options !== "object" || Array.isArray(options)))
		throw invalidArgType("options", "an object", options);
	if (options?.compress !== undefined && options.compress !== "gzip")
		throw codedError("ERR_INVALID_ARG_VALUE", 'Archive compression must be "gzip"', TypeError);
	const level = options?.level === undefined ? 6 : options.level;
	if (typeof level !== "number") throw invalidArgType("options.level", "a number", level);
	if (!Number.isInteger(level) || level < 1 || level > 12)
		throw outOfRange("options.level", "an integer between 1 and 12", level);
	return { compress: options?.compress === "gzip", level };
}

export class Archive {
	#source: Uint8Array | Blob | SourceEntry[];
	#compress: boolean;
	#level: number;
	#tarPromise: Promise<Uint8Array> | undefined;
	#entriesPromise: Promise<TarEntry[]> | undefined;
	#outputPromise: Promise<Uint8Array> | undefined;

	constructor(data: ArchiveInput, options?: ArchiveOptions) {
		const compression = compressionOptions(options);
		this.#compress = compression.compress;
		this.#level = compression.level;
		const view = bytesView(data);
		if (view) {
			this.#source = view.slice();
			return;
		}
		if (blobValue(data)) {
			this.#source = data;
			return;
		}
		if (data === null || typeof data !== "object" || Array.isArray(data)) {
			throw invalidArgType("data", "an entries object, Blob, ArrayBuffer, or ArrayBufferView", data);
		}
		const entries: SourceEntry[] = [];
		const now = Date.now() / 1000;
		for (const path in data) {
			if (!Object.prototype.hasOwnProperty.call(data, path)) continue;
			if (!path || path.includes("\0"))
				throw codedError(
					"ERR_INVALID_ARG_VALUE",
					"Archive paths must be nonempty and contain no null bytes",
					TypeError,
				);
			const value = (data as Record<string, ArchiveContent>)[path];
			if (typeof value === "string") {
				entries.push({ path: path.replace(/\\/g, "/"), content: value, mtime: now });
				continue;
			}
			const bytes = bytesView(value);
			if (bytes) {
				entries.push({ path: path.replace(/\\/g, "/"), content: bytes.slice(), mtime: now });
				continue;
			}
			if (!blobValue(value))
				throw invalidArgType(`data[${JSON.stringify(path)}]`, "a string, Blob, ArrayBuffer, or ArrayBufferView", value);
			const modified = "lastModified" in value ? value.lastModified : undefined;
			entries.push({
				path: path.replace(/\\/g, "/"),
				content: value,
				mtime: typeof modified === "number" && Number.isFinite(modified) ? modified / 1000 : now,
			});
		}
		this.#source = entries;
	}

	async #loadTar(): Promise<Uint8Array> {
		const source = this.#source;
		if (Array.isArray(source)) {
			const entries: TarEntry[] = [];
			for (const entry of source) {
				const data =
					typeof entry.content === "string"
						? new TextEncoder().encode(entry.content)
						: entry.content instanceof Uint8Array
							? entry.content
							: new Uint8Array(await entry.content.arrayBuffer());
				const directory = entry.path.endsWith("/");
				if (directory && data.length !== 0)
					throw codedError("ERR_INVALID_ARG_VALUE", `Archive directory has nonempty contents: ${entry.path}`);
				entries.push({
					path: entry.path,
					type: directory ? "directory" : "file",
					data,
					linkpath: "",
					mode: directory ? 0o755 : 0o644,
					mtime: entry.mtime,
				});
			}
			return writeTar(entries);
		}
		const data = source instanceof Uint8Array ? source : new Uint8Array(await source.arrayBuffer());
		if (data.length >= 2 && data[0] === 0x1f && data[1] === 0x8b) {
			return gunzipSync(data);
		}
		return data;
	}

	async #entries(): Promise<TarEntry[]> {
		this.#tarPromise ??= this.#loadTar();
		this.#entriesPromise ??= this.#tarPromise.then(readTar);
		return this.#entriesPromise;
	}

	async bytes(): Promise<Uint8Array> {
		this.#tarPromise ??= this.#loadTar();
		this.#outputPromise ??= this.#tarPromise.then((data) => {
			if (!this.#compress) return data;
			// Bun uses libdeflate's 1-12 scale; zlib's strongest encoding is 9.
			return gzipSync(data, { level: Math.min(this.#level, 9) });
		});
		return (await this.#outputPromise).slice();
	}

	async blob(): Promise<Blob> {
		const bytes = await this.bytes();
		return new Blob([bytes], { type: this.#compress ? "application/gzip" : "application/x-tar" });
	}

	async files(glob?: string | readonly string[]): Promise<Map<string, File>> {
		const matches = archiveFilter(glob);
		const entries = await this.#entries();
		const result = new Map<string, File>();
		for (const entry of entries) {
			if (entry.type !== "file" || !matches(entry.path)) continue;
			result.set(entry.path, new File([entry.data], entry.path, { lastModified: entry.mtime * 1000 }));
		}
		return result;
	}

	async extract(path: string, options?: ArchiveExtractOptions): Promise<number> {
		if (typeof path !== "string") throw invalidArgType("path", "a string", path);
		if (options !== undefined && (options === null || typeof options !== "object" || Array.isArray(options)))
			throw invalidArgType("options", "an object", options);
		const matches = archiveFilter(options?.glob);
		const entries = await this.#entries();
		return extractTar(entries, path, matches, options?.glob !== undefined);
	}

	static async write(path: string, data: ArchiveInput | Archive, options?: ArchiveOptions): Promise<void> {
		if (typeof path !== "string") throw invalidArgType("path", "a string", path);
		let archive: Archive;
		if (data instanceof Archive) {
			archive = options === undefined ? data : new Archive(await data.bytes(), options);
		} else archive = new Archive(data, options);
		const bytes = await archive.bytes();
		await fs.writeFile(path, bytes);
	}

	get [Symbol.toStringTag](): string {
		return "Archive";
	}
}
