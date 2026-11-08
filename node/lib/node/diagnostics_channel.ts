import { invalidArgType } from "internal:errors";
type Subscriber = (message: unknown, name: string | symbol) => void;
type Context = Record<string, unknown>;
type Store = { run<T>(context: unknown, fn: (...args: unknown[]) => T, ...args: unknown[]): T };
const channels = new Map<string | symbol, Channel>();
export class Channel {
	readonly name: string | symbol;
	private subscribers = new Set<Subscriber>();
	private stores = new Map<Store, ((message: unknown) => unknown) | undefined>();
	constructor(name: string | symbol) {
		if (typeof name !== "string" && typeof name !== "symbol")
			throw invalidArgType("channel", "a string or symbol", name);
		this.name = name;
	}
	get hasSubscribers(): boolean {
		return this.subscribers.size > 0;
	}
	subscribe(subscription: Subscriber): void {
		if (typeof subscription !== "function") throw invalidArgType("subscription", "a function", subscription);
		this.subscribers.add(subscription);
	}
	unsubscribe(subscription: Subscriber): boolean {
		return this.subscribers.delete(subscription);
	}
	publish(message: unknown): void {
		for (const fn of Array.from(this.subscribers)) {
			try {
				fn(message, this.name);
			} catch (error) {
				queueMicrotask(() => {
					throw error;
				});
			}
		}
	}
	bindStore(store: Store, transform?: (message: unknown) => unknown): void {
		if (!store || typeof store.run !== "function") throw invalidArgType("store", "an AsyncLocalStorage", store);
		if (transform !== undefined && typeof transform !== "function")
			throw invalidArgType("transform", "a function", transform);
		this.stores.set(store, transform);
	}
	unbindStore(store: Store): boolean {
		return this.stores.delete(store);
	}
	runStores<T>(context: unknown, fn: (...args: unknown[]) => T, thisArg?: unknown, ...args: unknown[]): T {
		const stores = Array.from(this.stores);
		const run = (i: number): T => {
			if (i === stores.length) {
				this.publish(context);
				return fn.apply(thisArg, args);
			}
			const [store, transform] = stores[i];
			return store.run(transform ? transform(context) : context, () => run(i + 1));
		};
		return run(0);
	}
}
export function channel(name: string | symbol): Channel {
	let c = channels.get(name);
	if (!c) {
		c = new Channel(name);
		channels.set(name, c);
	}
	return c;
}
export function hasSubscribers(name: string | symbol): boolean {
	return channel(name).hasSubscribers;
}
export function subscribe(name: string | symbol, subscription: Subscriber): void {
	channel(name).subscribe(subscription);
}
export function unsubscribe(name: string | symbol, subscription: Subscriber): boolean {
	return channel(name).unsubscribe(subscription);
}
type TraceChannels = { start: Channel; end: Channel; asyncStart: Channel; asyncEnd: Channel; error: Channel };
export class TracingChannel {
	start: Channel;
	end: Channel;
	asyncStart: Channel;
	asyncEnd: Channel;
	error: Channel;
	constructor(nameOrChannels: string | TraceChannels) {
		if (typeof nameOrChannels === "string") {
			this.start = channel(`tracing:${nameOrChannels}:start`);
			this.end = channel(`tracing:${nameOrChannels}:end`);
			this.asyncStart = channel(`tracing:${nameOrChannels}:asyncStart`);
			this.asyncEnd = channel(`tracing:${nameOrChannels}:asyncEnd`);
			this.error = channel(`tracing:${nameOrChannels}:error`);
		} else {
			this.start = nameOrChannels.start;
			this.end = nameOrChannels.end;
			this.asyncStart = nameOrChannels.asyncStart;
			this.asyncEnd = nameOrChannels.asyncEnd;
			this.error = nameOrChannels.error;
		}
	}
	get hasSubscribers(): boolean {
		return (
			this.start.hasSubscribers ||
			this.end.hasSubscribers ||
			this.asyncStart.hasSubscribers ||
			this.asyncEnd.hasSubscribers ||
			this.error.hasSubscribers
		);
	}
	subscribe(handlers: Partial<Record<keyof TraceChannels, Subscriber>>): void {
		for (const name in handlers) {
			const fn = handlers[name as keyof TraceChannels];
			if (fn) this[name as keyof TraceChannels].subscribe(fn);
		}
	}
	unsubscribe(handlers: Partial<Record<keyof TraceChannels, Subscriber>>): void {
		for (const name in handlers) {
			const fn = handlers[name as keyof TraceChannels];
			if (fn) this[name as keyof TraceChannels].unsubscribe(fn);
		}
	}
	traceSync<T>(fn: (...args: unknown[]) => T, context: Context = {}, thisArg?: unknown, ...args: unknown[]): T {
		return this.start.runStores(context, () => {
			try {
				const result = fn.apply(thisArg, args);
				context.result = result;
				return result;
			} catch (error) {
				context.error = error;
				this.error.publish(context);
				throw error;
			} finally {
				this.end.publish(context);
			}
		});
	}
	tracePromise<T>(
		fn: (...args: unknown[]) => Promise<T>,
		context: Context = {},
		thisArg?: unknown,
		...args: unknown[]
	): Promise<T> {
		return this.start.runStores(context, () => {
			try {
				return fn.apply(thisArg, args).then(
					(result) => {
						context.result = result;
						return this.asyncStart.runStores(context, () => {
							try {
								return result;
							} finally {
								this.asyncEnd.publish(context);
							}
						});
					},
					(error) => {
						context.error = error;
						this.error.publish(context);
						return this.asyncStart.runStores(context, () => {
							try {
								throw error;
							} finally {
								this.asyncEnd.publish(context);
							}
						});
					},
				);
			} catch (error) {
				context.error = error;
				this.error.publish(context);
				throw error;
			} finally {
				this.end.publish(context);
			}
		});
	}
	traceCallback<T>(
		fn: (...args: unknown[]) => T,
		position = -1,
		context: Context = {},
		thisArg?: unknown,
		...args: unknown[]
	): T {
		const index = position < 0 ? args.length + position : position,
			callback = args[index];
		if (typeof callback !== "function") throw invalidArgType("callback", "a function", callback);
		const self = this;
		args[index] = function (this: unknown, ...values: unknown[]) {
			if (values[0]) {
				context.error = values[0];
				self.error.publish(context);
			} else context.result = values[1];
			return self.asyncStart.runStores(context, () => {
				try {
					return callback.apply(this, values);
				} finally {
					self.asyncEnd.publish(context);
				}
			});
		};
		return this.traceSync(fn, context, thisArg, ...args);
	}
}
export function tracingChannel(name: string | TraceChannels): TracingChannel {
	return new TracingChannel(name);
}
export default { channel, hasSubscribers, subscribe, unsubscribe, Channel, tracingChannel, TracingChannel };
