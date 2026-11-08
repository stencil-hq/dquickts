import { EventEmitter } from "node:events";
import { notSupported } from "internal:errors";
export const isPrimary = true,
	isMaster = true,
	isWorker = false,
	workers: Record<string, unknown> = {},
	settings: Record<string, unknown> = {};
export let schedulingPolicy = 2;
export const SCHED_NONE = 1,
	SCHED_RR = 2;
export function fork(): never {
	throw notSupported("cluster.fork");
}
const cluster = new EventEmitter();
export function setupPrimary(options: Record<string, unknown> = {}): void {
	Object.assign(settings, options);
	queueMicrotask(() => cluster.emit("setup", settings));
}
export const setupMaster = setupPrimary;
export function disconnect(callback?: () => void): void {
	if (callback) queueMicrotask(callback);
}
export class Worker extends EventEmitter {}
Object.assign(cluster, {
	isPrimary,
	isMaster,
	isWorker,
	workers,
	settings,
	fork,
	setupPrimary,
	setupMaster,
	disconnect,
	Worker,
	SCHED_NONE,
	SCHED_RR,
	schedulingPolicy,
});
export default cluster;
