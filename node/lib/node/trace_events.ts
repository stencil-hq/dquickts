export class Tracing {
	enabled = false;
	readonly categories: string;
	constructor(options: { categories?: string[] } = {}) {
		this.categories = (options.categories ?? []).join(",");
	}
	enable(): void {
		this.enabled = true;
	}
	disable(): void {
		this.enabled = false;
	}
}
export function createTracing(options: { categories: string[] }): Tracing {
	return new Tracing(options);
}
export function getEnabledCategories(): undefined {
	return undefined;
}
export default { createTracing, getEnabledCategories };
