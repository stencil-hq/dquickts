// stream.addAbortSignal() (Node's lib/internal/streams/add-abort-signal.js).

import { addAbortListener } from "node:events";
import { AbortError, ERR_INVALID_ARG_TYPE } from "internal:streams/shared";
import { isNodeStream, isWebStream, kControllerErrorFunction, type StreamLike } from "internal:streams/utils";
import { eos } from "internal:streams/end-of-stream";

// Inlined for readable-stream; also does not allow the signal to be absent.
function validateAbortSignal(signal: unknown, name: string): void {
	if (typeof signal !== "object" || signal === null || !("aborted" in signal)) {
		throw new ERR_INVALID_ARG_TYPE(name, "AbortSignal", signal);
	}
}

export function addAbortSignal<T>(signal: AbortSignal, stream: T): T {
	validateAbortSignal(signal, "signal");
	if (!isNodeStream(stream as StreamLike) && !isWebStream(stream as StreamLike)) {
		throw new ERR_INVALID_ARG_TYPE("stream", ["ReadableStream", "WritableStream", "Stream"], stream);
	}
	return addAbortSignalNoValidate(signal, stream);
}

export function addAbortSignalNoValidate<T>(signal: AbortSignal | undefined | null, stream: T): T {
	if (typeof signal !== "object" || signal === null || !("aborted" in signal)) return stream;
	const s = stream as unknown as { destroy(err: unknown): void; [kControllerErrorFunction]: (err: unknown) => void };
	const onAbort = isNodeStream(stream as StreamLike)
		? () => {
				s.destroy(new AbortError(undefined, { cause: signal.reason }));
			}
		: () => {
				s[kControllerErrorFunction](new AbortError(undefined, { cause: signal.reason }));
			};
	if (signal.aborted) {
		onAbort();
	} else {
		const disposable = addAbortListener(signal, onAbort);
		eos(stream, () => disposable[Symbol.dispose]());
	}
	return stream;
}
