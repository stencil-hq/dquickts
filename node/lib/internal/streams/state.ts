// highWaterMark defaults (Node's lib/internal/streams/state.js).

import { ERR_INVALID_ARG_VALUE, validateInteger } from "internal:streams/shared";

let defaultHighWaterMarkBytes = 64 * 1024;
let defaultHighWaterMarkObjectMode = 16;

type HwmOptions = { highWaterMark?: number | null; [key: string]: unknown };

function highWaterMarkFrom(options: HwmOptions, isDuplex: boolean | undefined, duplexKey: string): unknown {
	return options.highWaterMark != null ? options.highWaterMark : isDuplex ? options[duplexKey] : null;
}

export function getDefaultHighWaterMark(objectMode?: boolean): number {
	return objectMode ? defaultHighWaterMarkObjectMode : defaultHighWaterMarkBytes;
}

export function setDefaultHighWaterMark(objectMode: boolean, value: number): void {
	validateInteger(value, "value", 0);
	if (objectMode) defaultHighWaterMarkObjectMode = value;
	else defaultHighWaterMarkBytes = value;
}

export function getHighWaterMark(
	state: { objectMode?: boolean },
	options: HwmOptions,
	duplexKey: string,
	isDuplex?: boolean,
): number {
	const hwm = highWaterMarkFrom(options, isDuplex, duplexKey);
	if (hwm != null) {
		if (!Number.isInteger(hwm) || (hwm as number) < 0) {
			const name = isDuplex ? `options.${duplexKey}` : "options.highWaterMark";
			throw new ERR_INVALID_ARG_VALUE(name, hwm);
		}
		return Math.floor(hwm as number);
	}
	return getDefaultHighWaterMark(state.objectMode);
}
