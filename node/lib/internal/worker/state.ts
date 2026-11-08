// Per-thread worker state, filled by internal:worker/thread in a worker.

import * as core from "qbun:core";
import * as native from "qbun:worker";
import type { MessagePort } from "internal:worker/messaging";

export const SHARE_ENV: unique symbol = Symbol.for("nodejs.worker_threads.SHARE_ENV") as never;

const info = core.info() as { isMainThread: boolean };

export interface WorkerState {
	isMainThread: boolean;
	threadId: number;
	threadName: string;
	parentPort: MessagePort | null;
	workerData: unknown;
	environmentData: Map<unknown, unknown>;
	resourceLimits: Record<string, number>;
	/** a Web Worker (new Worker() global), not node:worker_threads */
	web: boolean;
}

export const state: WorkerState = {
	isMainThread: info.isMainThread,
	threadId: (native as unknown as { threadId(): number }).threadId(),
	threadName: info.isMainThread ? "main" : "",
	parentPort: null,
	workerData: null,
	environmentData: new Map(),
	resourceLimits: {},
	web: false,
};
