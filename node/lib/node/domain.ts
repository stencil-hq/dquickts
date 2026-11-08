import { EventEmitter } from "node:events";
export let active: Domain | null = null;
const stack: (Domain | null)[] = [];
export class Domain extends EventEmitter {
	members: EventEmitter[] = [];
	enter(): void {
		stack.push(active);
		active = this;
	}
	exit(): void {
		active = stack.pop() ?? null;
	}
	run<T>(fn: (...args: unknown[]) => T, ...args: unknown[]): T | undefined {
		this.enter();
		try {
			return fn(...args);
		} catch (error) {
			this.emit("error", error);
		} finally {
			this.exit();
		}
	}
	add(emitter: EventEmitter): void {
		if (!this.members.includes(emitter)) {
			this.members.push(emitter);
			(emitter as EventEmitter & { domain?: Domain }).domain = this;
		}
	}
	remove(emitter: EventEmitter): void {
		const i = this.members.indexOf(emitter);
		if (i >= 0) this.members.splice(i, 1);
		delete (emitter as EventEmitter & { domain?: Domain }).domain;
	}
	bind<T>(fn: (...args: unknown[]) => T): (...args: unknown[]) => T | undefined {
		const domain = this;
		return function (this: unknown, ...args: unknown[]) {
			return domain.run(() => fn.apply(this, args));
		};
	}
	intercept<T>(fn: (...args: unknown[]) => T): (...args: unknown[]) => T | undefined {
		return this.bind((error, ...args) => {
			if (error) {
				this.emit("error", error);
				return undefined as T;
			}
			return fn(...args);
		});
	}
	dispose(): void {
		this.exit();
		for (const member of [...this.members]) this.remove(member);
		this.removeAllListeners();
	}
}
export function create(): Domain {
	return new Domain();
}
export const createDomain = create;
export default {
	Domain,
	create,
	createDomain,
	get active() {
		return active;
	},
};
