// Port of Node's lib/stream/web.js.

import { ERR_INVALID_ARG_TYPE, validateBoolean } from "internal:streams/shared";
import { TransformStream, TransformStreamDefaultController } from "internal:webstreams/transformstream";
import {
	WritableStream,
	WritableStreamDefaultController,
	WritableStreamDefaultWriter,
} from "internal:webstreams/writablestream";
import {
	ReadableByteStreamController,
	ReadableStream,
	ReadableStreamBYOBReader,
	ReadableStreamBYOBRequest,
	ReadableStreamDefaultController,
	ReadableStreamDefaultReader,
	isReadableStream,
	readableStreamTee,
} from "internal:webstreams/readablestream";
import { ByteLengthQueuingStrategy, CountQueuingStrategy } from "internal:webstreams/queuingstrategies";
import { TextDecoderStream, TextEncoderStream } from "internal:webstreams/encoding";
import { CompressionStream, DecompressionStream } from "internal:webstreams/compression";

export function ReadableStreamTee(stream: unknown, cloneForBranch2: unknown = false): unknown {
	if (!isReadableStream(stream)) {
		throw new ERR_INVALID_ARG_TYPE("stream", "ReadableStream", stream);
	}
	validateBoolean(cloneForBranch2, "cloneForBranch2");
	return readableStreamTee(stream as never, cloneForBranch2);
}

export {
	ByteLengthQueuingStrategy,
	CompressionStream,
	CountQueuingStrategy,
	DecompressionStream,
	ReadableByteStreamController,
	ReadableStream,
	ReadableStreamBYOBReader,
	ReadableStreamBYOBRequest,
	ReadableStreamDefaultController,
	ReadableStreamDefaultReader,
	TextDecoderStream,
	TextEncoderStream,
	TransformStream,
	TransformStreamDefaultController,
	WritableStream,
	WritableStreamDefaultController,
	WritableStreamDefaultWriter,
};

export default {
	ReadableStream,
	ReadableStreamTee,
	ReadableStreamDefaultReader,
	ReadableStreamBYOBReader,
	ReadableStreamBYOBRequest,
	ReadableByteStreamController,
	ReadableStreamDefaultController,
	TransformStream,
	TransformStreamDefaultController,
	WritableStream,
	WritableStreamDefaultWriter,
	WritableStreamDefaultController,
	ByteLengthQueuingStrategy,
	CountQueuingStrategy,
	TextEncoderStream,
	TextDecoderStream,
	CompressionStream,
	DecompressionStream,
};
