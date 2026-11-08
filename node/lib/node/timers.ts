import * as timerPromises from "node:timers/promises";
// node:timers — Node's Timeout and Immediate objects on libuv timers and the
// check phase of the loop.

import * as core from "qbun:core";
import { validateFunction } from "internal:errors";
import { bind } from "internal:async-context";

interface NativeTimer {
	start(ms: number, repeat: number): void;
	stop(): void;
	ref(): void;
	unref(): void;
	hasRef(): boolean;
	close(): void;
	isActive(): boolean;
}

const TIMEOUT_MAX = 2 ** 31 - 1;
let nextId = 1;
const activeById = new Map<number, Timeout>();

function clampDelay(ms: unknown): number {
	let n = Number(ms);
	if (!(n >= 1 && n <= TIMEOUT_MAX)) n = 1;
	return Math.trunc(n);
}

export class Timeout {
	_idleTimeout: number;
	_onTimeout: ((...args: unknown[]) => void) | null;
	_timerArgs: unknown[];
	_repeat: number | null;
	_destroyed = false;
	#id: number;
	#native: NativeTimer;
	#refed = true;

	constructor(
		callback: (...args: unknown[]) => void,
		after: unknown,
		args: unknown[],
		isRepeat: boolean,
		isRefed = true,
	) {
		this._idleTimeout = clampDelay(after);
		this._onTimeout = bind(callback);
		this._timerArgs = args;
		this._repeat = isRepeat ? this._idleTimeout : null;
		this.#id = nextId++;
		this.#native = core.createTimer(() => this.#fire()) as NativeTimer;
		this.#refed = isRefed;
		if (!isRefed) this.#native.unref();
		activeById.set(this.#id, this);
		this.#native.start(this._idleTimeout, this._repeat ?? 0);
	}

	#fire(): void {
		if (this._repeat === null) {
			this._destroyed = true;
			activeById.delete(this.#id);
		}
		const cb = this._onTimeout;
		if (cb) cb.apply(this, this._timerArgs);
	}

	refresh(): this {
		if (!this._destroyed || this._repeat === null) {
			this._destroyed = false;
			activeById.set(this.#id, this);
			this.#native.start(this._idleTimeout, this._repeat ?? 0);
		}
		return this;
	}

	ref(): this {
		this.#refed = true;
		this.#native.ref();
		return this;
	}

	unref(): this {
		this.#refed = false;
		this.#native.unref();
		return this;
	}

	hasRef(): boolean {
		return this.#refed && !this._destroyed;
	}

	close(): this {
		clearTimeout(this);
		return this;
	}

	_clear(): void {
		if (this._destroyed && !this.#native.isActive()) {
			activeById.delete(this.#id);
			return;
		}
		this._destroyed = true;
		activeById.delete(this.#id);
		this.#native.stop();
	}

	[Symbol.toPrimitive](): number {
		return this.#id;
	}

	[Symbol.dispose](): void {
		clearTimeout(this);
	}
}

export class Immediate {
	_onImmediate: ((...args: unknown[]) => void) | null;
	_argv: unknown[];
	_destroyed = false;
	#refed = true;

	constructor(callback: (...args: unknown[]) => void, args: unknown[]) {
		this._onImmediate = bind(callback);
		this._argv = args;
		immediateQueue.push(this);
		refedImmediates++;
		core.scheduleImmediates(true);
	}

	ref(): this {
		if (!this.#refed && !this._destroyed) {
			this.#refed = true;
			refedImmediates++;
			core.scheduleImmediates(true);
		}
		return this;
	}

	unref(): this {
		if (this.#refed && !this._destroyed) {
			this.#refed = false;
			refedImmediates--;
			core.scheduleImmediates(refedImmediates > 0);
		}
		return this;
	}

	hasRef(): boolean {
		return this.#refed;
	}

	_clear(): void {
		if (this._destroyed) return;
		this._destroyed = true;
		if (this.#refed) refedImmediates--;
		this._onImmediate = null;
	}

	[Symbol.dispose](): void {
		clearImmediate(this);
	}
}

let immediateQueue: Immediate[] = [];
let refedImmediates = 0;

/** Called by the loop's check phase: run the immediates queued so far. */
export function runImmediates(): void {
	const queue = immediateQueue;
	immediateQueue = [];
	for (let i = 0; i < queue.length; i++) {
		const imm = queue[i];
		if (imm._destroyed) continue;
		const cb = imm._onImmediate!;
		imm._clear();
		try {
			cb.apply(imm, imm._argv);
		} catch (err) {
			// the remaining immediates run on the next turn, like Node
			immediateQueue = queue.slice(i + 1).concat(immediateQueue);
			if (immediateQueue.length) core.scheduleImmediates(refedImmediates > 0);
			throw err;
		}
	}
	if (immediateQueue.length) core.scheduleImmediates(refedImmediates > 0);
}

export function setTimeout<A extends unknown[]>(callback: (...args: A) => void, ms?: number, ...args: A): Timeout {
	validateFunction(callback, "callback");
	return new Timeout(callback as (...a: unknown[]) => void, ms, args, false);
}

export function setInterval<A extends unknown[]>(callback: (...args: A) => void, ms?: number, ...args: A): Timeout {
	validateFunction(callback, "callback");
	return new Timeout(callback as (...a: unknown[]) => void, ms, args, true);
}

export function setImmediate<A extends unknown[]>(callback: (...args: A) => void, ...args: A): Immediate {
	validateFunction(callback, "callback");
	return new Immediate(callback as (...a: unknown[]) => void, args);
}

function toTimeout(timer: unknown): Timeout | undefined {
	if (timer instanceof Timeout) return timer;
	if (typeof timer === "number" || typeof timer === "string") return activeById.get(Number(timer));
	return undefined;
}

export function clearTimeout(timer: unknown): void {
	if (timer instanceof Immediate) {
		timer._clear();
		return;
	}
	toTimeout(timer)?._clear();
}

export const clearInterval = clearTimeout;

export function clearImmediate(immediate: unknown): void {
	if (immediate instanceof Immediate) immediate._clear();
}

/** Legacy: unref'd timer helpers kept for compatibility. */
export function active(timer: Timeout): void {
	timer.refresh();
}
export function unenroll(timer: Timeout): void {
	clearTimeout(timer);
}

export const promises = {
	get setTimeout() {
		return timerPromises.setTimeout;
	},
	get setImmediate() {
		return timerPromises.setImmediate;
	},
	get setInterval() {
		return timerPromises.setInterval;
	},
	get scheduler() {
		return timerPromises.scheduler;
	},
};

export default {
	setTimeout,
	setInterval,
	setImmediate,
	clearTimeout,
	clearInterval,
	clearImmediate,
	Timeout,
	Immediate,
	active,
	unenroll,
	promises,
};
