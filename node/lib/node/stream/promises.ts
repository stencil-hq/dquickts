// Port of Node's lib/stream/promises.js. Must not import node:stream statically (node:stream imports this module).
import { isIterable, isNodeStream, isWebStream } from "internal:streams/utils";
import { pipelineImpl as pl } from "internal:streams/pipeline";
import { finished } from "internal:streams/end-of-stream";

export function pipeline(...streams: unknown[]): Promise<unknown> {
	const { promise, resolve, reject } = Promise.withResolvers<unknown>();
	let signal: AbortSignal | undefined;
	let end: boolean | undefined;
	const lastArg = streams[streams.length - 1];
	if (
		lastArg &&
		typeof lastArg === "object" &&
		!isNodeStream(lastArg) &&
		!isIterable(lastArg) &&
		!isWebStream(lastArg)
	) {
		const options = streams.pop() as { signal?: AbortSignal; end?: boolean };
		signal = options.signal;
		end = options.end;
	}

	let lastStream = streams[streams.length - 1];
	if (streams.length === 1 && Array.isArray(streams[0])) {
		lastStream = streams[0][streams[0].length - 1];
	}

	try {
		const stream = pl(
			streams,
			(err: unknown, value?: unknown) => {
				if (err) {
					reject(err);
				} else {
					resolve(value);
				}
			},
			{ signal, end },
		);

		if (typeof lastStream === "function" && stream.readable) {
			stream.resume();
		}
	} catch (err) {
		// Node's executor-based Promise turns synchronous throws into rejections.
		reject(err);
	}
	return promise;
}

export { finished };

export default {
	finished,
	pipeline,
};
