// Port of Node's lib/stream/consumers.js.
import { Buffer } from "node:buffer";

type Source = AsyncIterable<unknown>;

export async function blob(stream: Source): Promise<Blob> {
	const chunks: BlobPart[] = [];
	for await (const chunk of stream) chunks.push(chunk as BlobPart);
	return new Blob(chunks);
}

export async function arrayBuffer(stream: Source): Promise<ArrayBuffer> {
	const ret = await blob(stream);
	return ret.arrayBuffer();
}

export async function buffer(stream: Source): Promise<Buffer> {
	return Buffer.from(await arrayBuffer(stream));
}

export async function bytes(stream: Source): Promise<Uint8Array> {
	return new Uint8Array(await arrayBuffer(stream));
}

export async function text(stream: Source): Promise<string> {
	const dec = new TextDecoder();
	let str = "";
	for await (const chunk of stream) {
		if (typeof chunk === "string") str += chunk;
		else str += dec.decode(chunk as BufferSource, { stream: true });
	}
	// Flush pending incomplete multibyte characters.
	str += dec.decode(undefined, { stream: false });
	return str;
}

export async function json(stream: Source): Promise<unknown> {
	const str = await text(stream);
	return JSON.parse(str);
}

export default {
	arrayBuffer,
	blob,
	buffer,
	bytes,
	text,
	json,
};
