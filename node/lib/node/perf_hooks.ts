import * as core from "qbun:core";
import { setInterval, clearInterval, Timeout } from "node:timers";
import { codedError, invalidArgType, outOfRange } from "internal:errors";
export class PerformanceEntry {
	constructor(
		public name: string,
		public entryType: string,
		public startTime: number,
		public duration: number,
	) {}
	toJSON(): object {
		return { name: this.name, entryType: this.entryType, startTime: this.startTime, duration: this.duration };
	}
	get [Symbol.toStringTag](): string {
		return "PerformanceEntry";
	}
}
export class PerformanceMark extends PerformanceEntry {
	detail: unknown;
	constructor(name: string, options: { startTime?: number; detail?: unknown } = {}) {
		const time = options.startTime ?? now();
		if (typeof time !== "number" || time < 0 || !Number.isFinite(time))
			throw outOfRange("startTime", "a non-negative finite number", time);
		super(String(name), "mark", time, 0);
		this.detail = options.detail ?? null;
	}
	toJSON(): object {
		return { ...super.toJSON(), detail: this.detail };
	}
	get [Symbol.toStringTag](): string {
		return "PerformanceMark";
	}
}
export class PerformanceMeasure extends PerformanceEntry {
	detail: unknown = null;
	get [Symbol.toStringTag](): string {
		return "PerformanceMeasure";
	}
	toJSON(): object {
		return { ...super.toJSON(), detail: this.detail };
	}
}
const engine = globalThis.performance;
const clockStart = core.hrtime();
const now = engine?.now ? engine.now.bind(engine) : () => Number(core.hrtime() - clockStart) / 1e6;
const timeOrigin = engine?.timeOrigin ?? Date.now() - now();
let entries: PerformanceEntry[] = [];
let resourceBufferSize = 250;
const observers = new Set<PerformanceObserver>();
function record(entry: PerformanceEntry): void {
	entries.push(entry);
	for (const observer of observers) observer.enqueue(entry);
}
export class PerformanceObserverEntryList {
	constructor(private entries: PerformanceEntry[]) {}
	getEntries(): PerformanceEntry[] {
		return this.entries.slice().sort((a, b) => a.startTime - b.startTime);
	}
	getEntriesByType(type: string): PerformanceEntry[] {
		return this.getEntries().filter((e) => e.entryType === type);
	}
	getEntriesByName(name: string, type?: string): PerformanceEntry[] {
		return this.getEntries().filter((e) => e.name === name && (type === undefined || e.entryType === type));
	}
}
export class PerformanceObserver {
	static supportedEntryTypes = Object.freeze([
		"function",
		"mark",
		"measure",
		"gc",
		"http",
		"http2",
		"dns",
		"net",
		"resource",
	]);
	private types = new Set<string>();
	private pending: PerformanceEntry[] = [];
	private scheduled = false;
	constructor(private callback: (list: PerformanceObserverEntryList, observer: PerformanceObserver) => void) {
		if (typeof callback !== "function") throw invalidArgType("callback", "a function", callback);
	}
	observe(options: { entryTypes?: string[]; type?: string; buffered?: boolean }): void {
		if (!options || typeof options !== "object") throw invalidArgType("options", "an object", options);
		if (options.entryTypes && options.type)
			throw codedError("ERR_INVALID_ARG_VALUE", "Cannot specify both entryTypes and type", TypeError);
		if (options.entryTypes) this.types = new Set(options.entryTypes);
		else if (options.type) this.types.add(options.type);
		else throw codedError("ERR_INVALID_ARG_VALUE", "An entryTypes or type option is required", TypeError);
		observers.add(this);
		if (options.buffered) for (const entry of entries) this.enqueue(entry);
	}
	enqueue(entry: PerformanceEntry): void {
		if (!this.types.has(entry.entryType)) return;
		this.pending.push(entry);
		if (!this.scheduled) {
			this.scheduled = true;
			queueMicrotask(() => {
				this.scheduled = false;
				const list = this.takeRecords();
				if (list.length) this.callback(new PerformanceObserverEntryList(list), this);
			});
		}
	}
	disconnect(): void {
		observers.delete(this);
		this.pending = [];
		this.types.clear();
	}
	takeRecords(): PerformanceEntry[] {
		const result = this.pending;
		this.pending = [];
		return result;
	}
}
function timestamp(value: string | number | undefined, fallback: number): number {
	if (value === undefined) return fallback;
	if (typeof value === "number") {
		if (value < 0 || !Number.isFinite(value)) throw outOfRange("timestamp", "a non-negative finite number", value);
		return value;
	}
	for (let i = entries.length - 1; i >= 0; i--)
		if (entries[i].entryType === "mark" && entries[i].name === value) return entries[i].startTime;
	if (value === "nodeStart") return 0;
	throw codedError("ERR_INVALID_PERFORMANCE_MARK", `The "${value}" performance mark has not been set`);
}
export class Histogram {
	protected values: number[] = [];
	protected timer: Timeout | undefined;
	get count(): number {
		return this.values.length;
	}
	get countBigInt(): bigint {
		return BigInt(this.count);
	}
	get min(): number {
		return this.values.length ? this.values.reduce((a, b) => Math.min(a, b), Infinity) : 9223372036854776000;
	}
	get max(): number {
		return this.values.length ? this.values.reduce((a, b) => Math.max(a, b), 0) : 0;
	}
	get minBigInt(): bigint {
		return this.count ? BigInt(this.min) : 9223372036854775807n;
	}
	get maxBigInt(): bigint {
		return BigInt(this.max);
	}
	get mean(): number {
		return this.values.reduce((a, b) => a + b, 0) / this.count;
	}
	get stddev(): number {
		const mean = this.mean;
		return Math.sqrt(this.values.reduce((a, b) => a + (b - mean) ** 2, 0) / this.count);
	}
	get exceeds(): number {
		return 0;
	}
	get exceedsBigInt(): bigint {
		return 0n;
	}
	record(value: number | bigint): void {
		if (typeof value !== "number" && typeof value !== "bigint")
			throw invalidArgType("val", "a number or bigint", value);
		const n = Number(value);
		if (!Number.isInteger(n) || n < 1) throw outOfRange("val", "a positive integer", value);
		this.values.push(n);
	}
	private previous = core.hrtime();
	recordDelta(): void {
		const current = core.hrtime();
		this.record(current - this.previous || 1n);
		this.previous = current;
	}
	add(other: Histogram): void {
		if (!(other instanceof Histogram)) throw invalidArgType("other", "a Histogram", other);
		this.values.push(...other.values);
	}
	percentile(percent: number): number {
		if (typeof percent !== "number" || !(percent > 0 && percent <= 100))
			throw outOfRange("percentile", "> 0 && <= 100", percent);
		if (!this.count) return 0;
		const sorted = this.values.slice().sort((a, b) => a - b);
		return sorted[Math.max(0, Math.ceil((percent / 100) * sorted.length) - 1)];
	}
	percentileBigInt(percent: number): bigint {
		return BigInt(this.percentile(percent));
	}
	get percentiles(): Map<number, number> {
		const result = new Map<number, number>();
		result.set(0, this.count ? this.min : 0);
		for (const p of [50, 75, 87.5, 93.75, 96.875, 100]) result.set(p, this.percentile(p));
		return result;
	}
	get percentilesBigInt(): Map<number, bigint> {
		const result = new Map<number, bigint>();
		for (const [p, n] of this.percentiles) result.set(p, BigInt(n));
		return result;
	}
	reset(): void {
		this.values = [];
	}
	enable(): boolean {
		return false;
	}
	disable(): boolean {
		return false;
	}
}
export class RecordableHistogram extends Histogram {}
export class IntervalHistogram extends Histogram {
	constructor(private resolution: number) {
		super();
	}
	enable(): boolean {
		if (this.timer !== undefined) return false;
		let previous = core.hrtime();
		this.timer = setInterval(() => {
			const current = core.hrtime();
			this.record(current - previous || 1n);
			previous = current;
		}, this.resolution);
		this.timer.unref();
		return true;
	}
	disable(): boolean {
		if (this.timer === undefined) return false;
		clearInterval(this.timer);
		this.timer = undefined;
		return true;
	}
}
export function createHistogram(
	options: { lowest?: number | bigint; highest?: number | bigint; figures?: number } = {},
): RecordableHistogram {
	if (options.lowest !== undefined && Number(options.lowest) < 1) throw outOfRange("lowest", ">= 1", options.lowest);
	if (options.highest !== undefined && Number(options.highest) < 2 * Number(options.lowest ?? 1))
		throw outOfRange("highest", ">= 2 * lowest", options.highest);
	if (
		options.figures !== undefined &&
		(!Number.isInteger(options.figures) || options.figures < 1 || options.figures > 5)
	)
		throw outOfRange("figures", ">= 1 && <= 5", options.figures);
	return new RecordableHistogram();
}
export function monitorEventLoopDelay(options: { resolution?: number } = {}): IntervalHistogram {
	const resolution = options.resolution ?? 10;
	if (!Number.isInteger(resolution) || resolution < 1) throw outOfRange("resolution", "a positive integer", resolution);
	return new IntervalHistogram(resolution);
}
export const constants = {
	NODE_PERFORMANCE_GC_MAJOR: 4,
	NODE_PERFORMANCE_GC_MINOR: 1,
	NODE_PERFORMANCE_GC_INCREMENTAL: 8,
	NODE_PERFORMANCE_GC_WEAKCB: 16,
	NODE_PERFORMANCE_GC_FLAGS_NO: 0,
	NODE_PERFORMANCE_GC_FLAGS_CONSTRUCT_RETAINED: 2,
	NODE_PERFORMANCE_GC_FLAGS_FORCED: 4,
	NODE_PERFORMANCE_GC_FLAGS_SYNCHRONOUS_PHANTOM_PROCESSING: 8,
	NODE_PERFORMANCE_GC_FLAGS_ALL_AVAILABLE_GARBAGE: 16,
	NODE_PERFORMANCE_GC_FLAGS_ALL_EXTERNAL_MEMORY: 32,
	NODE_PERFORMANCE_GC_FLAGS_SCHEDULE_IDLE: 64,
};
const nodeTiming = Object.assign(new PerformanceEntry("node", "node", 0, 0), {
	nodeStart: 0,
	v8Start: 0,
	bootstrapComplete: 0,
	environment: 0,
	loopStart: 0,
	loopExit: -1,
	idleTime: 0,
});
const methods = {
	now,
	timeOrigin,
	nodeTiming,
	mark(name: string, options?: { startTime?: number; detail?: unknown }): PerformanceMark {
		const mark = new PerformanceMark(name, options);
		record(mark);
		return mark;
	},
	measure(
		name: string,
		start?: string | { start?: string | number; end?: string | number; duration?: number; detail?: unknown },
		end?: string,
	): PerformanceMeasure {
		let a = 0,
			b = now(),
			detail: unknown = null;
		if (typeof start === "object" && start !== null) {
			if (start.duration !== undefined && start.start !== undefined && start.end !== undefined)
				throw codedError(
					"ERR_PERFORMANCE_MEASURE_INVALID_OPTIONS",
					"Cannot specify start, end, and duration",
					TypeError,
				);
			if (start.duration !== undefined && start.start === undefined && start.end === undefined)
				throw codedError(
					"ERR_PERFORMANCE_MEASURE_INVALID_OPTIONS",
					"Must specify start or end with duration",
					TypeError,
				);
			a = timestamp(start.start, 0);
			b = timestamp(start.end, b);
			if (start.duration !== undefined) {
				if (start.end !== undefined) a = b - start.duration;
				else b = a + start.duration;
			}
			detail = start.detail ?? null;
		} else {
			a = timestamp(start, 0);
			b = timestamp(end, b);
		}
		const entry = new PerformanceMeasure(String(name), "measure", a, b - a);
		entry.detail = detail;
		record(entry);
		return entry;
	},
	getEntries(): PerformanceEntry[] {
		return new PerformanceObserverEntryList(entries).getEntries();
	},
	getEntriesByType(type: string): PerformanceEntry[] {
		return new PerformanceObserverEntryList(entries).getEntriesByType(type);
	},
	getEntriesByName(name: string, type?: string): PerformanceEntry[] {
		return new PerformanceObserverEntryList(entries).getEntriesByName(name, type);
	},
	clearMarks(name?: string): void {
		entries = entries.filter((e) => e.entryType !== "mark" || (name !== undefined && e.name !== String(name)));
	},
	clearMeasures(name?: string): void {
		entries = entries.filter((e) => e.entryType !== "measure" || (name !== undefined && e.name !== String(name)));
	},
	clearResourceTimings(): void {
		entries = entries.filter((e) => e.entryType !== "resource");
	},
	setResourceTimingBufferSize(size: number): void {
		if (typeof size !== "number" || !Number.isFinite(size) || size < 0)
			throw outOfRange("maxSize", "a non-negative finite number", size);
		resourceBufferSize = Math.floor(size);
	},
	markResourceTiming(
		timingInfo: { startTime: number; endTime?: number },
		requestedUrl: string,
		initiatorType: string,
		_global?: unknown,
		cacheMode = "",
		bodyInfo: object = {},
		responseStatus = 0,
		deliveryType = "",
	): PerformanceEntry {
		if (cacheMode !== "" && cacheMode !== "local")
			throw codedError("ERR_INVALID_ARG_VALUE", "cacheMode must be empty or local", TypeError);
		const entry = Object.assign(
			new PerformanceEntry(
				requestedUrl,
				"resource",
				timingInfo.startTime,
				(timingInfo.endTime ?? now()) - timingInfo.startTime,
			),
			{ initiatorType, ...timingInfo, ...bodyInfo, responseStatus, deliveryType },
		);
		if (entries.filter((e) => e.entryType === "resource").length < resourceBufferSize) record(entry);
		else for (const observer of observers) observer.enqueue(entry);
		return entry;
	},
	eventLoopUtilization(
		first?: { idle: number; active: number },
		second?: { idle: number; active: number },
	): { idle: number; active: number; utilization: number } {
		const active = first ? (second ? first.active - second.active : now() - first.active) : now(),
			idle = first && second ? first.idle - second.idle : 0;
		return { idle, active, utilization: active / (active + idle) || 0 };
	},
	timerify<T extends (...args: unknown[]) => unknown>(fn: T, options: { histogram?: Histogram } = {}): T {
		if (typeof fn !== "function") throw invalidArgType("fn", "a function", fn);
		const wrapped = function (this: unknown, ...args: unknown[]) {
			const start = now();
			const finish = () => {
				const entry = new PerformanceEntry(fn.name, "function", start, now() - start);
				Object.assign(entry, args);
				record(entry);
				options.histogram?.record(Math.max(1, Math.round(entry.duration * 1e6)));
			};
			try {
				const result = fn.apply(this, args);
				if (result && typeof (result as Promise<unknown>).then === "function")
					return Promise.resolve(result).finally(finish);
				finish();
				return result;
			} catch (error) {
				finish();
				throw error;
			}
		};
		Object.defineProperty(wrapped, "name", { value: `timerified ${fn.name}` });
		return wrapped as T;
	},
	toJSON(): object {
		return { nodeTiming, timeOrigin, eventLoopUtilization: methods.eventLoopUtilization() };
	},
};
export const performance = engine ?? {};
for (const key in methods)
	Object.defineProperty(performance, key, {
		value: methods[key as keyof typeof methods],
		writable: true,
		configurable: true,
		enumerable: true,
	});
export default {
	performance,
	PerformanceEntry,
	PerformanceMark,
	PerformanceMeasure,
	PerformanceObserver,
	PerformanceObserverEntryList,
	Histogram,
	RecordableHistogram,
	IntervalHistogram,
	createHistogram,
	monitorEventLoopDelay,
	constants,
};
