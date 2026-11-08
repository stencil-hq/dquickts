import { inspect } from "node:util";
import { compare } from "internal:util/comparisons";
import { codedError, invalidArgType } from "internal:errors";
interface AssertionOptions {
	actual?: unknown;
	expected?: unknown;
	message?: string;
	operator?: string;
	stackStartFn?: Function;
}
export class AssertionError extends Error {
	code = "ERR_ASSERTION";
	actual: unknown;
	expected: unknown;
	operator: string;
	generatedMessage: boolean;
	constructor(options: AssertionOptions) {
		const op = options.operator ?? "fail";
		super(options.message ?? `${inspect(options.actual)} ${op} ${inspect(options.expected)}`);
		this.name = "AssertionError";
		this.actual = options.actual;
		this.expected = options.expected;
		this.operator = op;
		this.generatedMessage = options.message === undefined;
		const capture = Error as typeof Error & { captureStackTrace?: (target: Error, start?: Function) => void };
		capture.captureStackTrace?.(this, options.stackStartFn ?? AssertionError);
	}
	toString(): string {
		return `${this.name} [${this.code}]: ${this.message}`;
	}
}
function error(actual: unknown, expected: unknown, message: unknown, operator: string, stackStartFn: Function): never {
	if (message instanceof Error) throw message;
	throw new AssertionError({
		actual,
		expected,
		message: message === undefined ? undefined : String(message),
		operator,
		stackStartFn,
	});
}
export function ok(value: unknown, message?: string | Error): asserts value {
	if (!value) error(value, true, message, "==", ok);
}
export function fail(
	actual?: unknown,
	expected?: unknown,
	message?: string | Error,
	operator?: string,
	stackStartFn?: Function,
): never {
	if (arguments.length <= 1) error(undefined, undefined, actual ?? "Failed", "fail", fail);
	error(actual, expected, message, operator ?? "!=", stackStartFn ?? fail);
}
export function equal(actual: unknown, expected: unknown, message?: string | Error): void {
	if (
		actual != expected &&
		!(typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected))
	)
		error(actual, expected, message, "==", equal);
}
export function notEqual(actual: unknown, expected: unknown, message?: string | Error): void {
	if (
		actual == expected ||
		(typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected))
	)
		error(actual, expected, message, "!=", notEqual);
}
export function strictEqual(actual: unknown, expected: unknown, message?: string | Error): void {
	if (!Object.is(actual, expected)) error(actual, expected, message, "strictEqual", strictEqual);
}
export function notStrictEqual(actual: unknown, expected: unknown, message?: string | Error): void {
	if (Object.is(actual, expected)) error(actual, expected, message, "notStrictEqual", notStrictEqual);
}
export function deepEqual(actual: unknown, expected: unknown, message?: string | Error): void {
	if (!compare(actual, expected, false)) error(actual, expected, message, "deepEqual", deepEqual);
}
export function notDeepEqual(actual: unknown, expected: unknown, message?: string | Error): void {
	if (compare(actual, expected, false)) error(actual, expected, message, "notDeepEqual", notDeepEqual);
}
export function deepStrictEqual(actual: unknown, expected: unknown, message?: string | Error): void {
	if (!compare(actual, expected, true)) error(actual, expected, message, "deepStrictEqual", deepStrictEqual);
}
export function notDeepStrictEqual(actual: unknown, expected: unknown, message?: string | Error): void {
	if (compare(actual, expected, true)) error(actual, expected, message, "notDeepStrictEqual", notDeepStrictEqual);
}
export function partialDeepStrictEqual(actual: unknown, expected: unknown, message?: string | Error): void {
	if (!compare(actual, expected, true, true))
		error(actual, expected, message, "partialDeepStrictEqual", partialDeepStrictEqual);
}
type Matcher = RegExp | Function | Record<string, unknown>;
function matches(thrown: unknown, expected: Matcher): boolean {
	if (expected instanceof RegExp) return expected.test(String(thrown));
	if (typeof expected === "function") {
		if (expected.prototype && thrown instanceof expected) return true;
		if (expected === Error || Error.prototype.isPrototypeOf(expected.prototype)) return false;
		return expected.call({}, thrown) === true;
	}
	if (expected === null || typeof expected !== "object")
		throw invalidArgType("error", "a function, RegExp, or object", expected);
	let count = 0;
	const keys = Reflect.ownKeys(expected);
	if (expected instanceof Error) keys.push("name", "message");
	for (const k of keys) {
		count++;
		if (thrown === null || typeof thrown !== "object" || !(k in thrown)) return false;
		const a = (thrown as Record<PropertyKey, unknown>)[k],
			b = (expected as Record<PropertyKey, unknown>)[k];
		if (b instanceof RegExp && typeof a === "string" ? !b.test(a) : !compare(a, b, true)) return false;
	}
	if (!count) throw codedError("ERR_INVALID_ARG_VALUE", 'The argument "error" may not be an empty object', TypeError);
	return true;
}
const noException = Symbol("no exception");
function check(
	thrown: unknown,
	expected: Matcher | string | undefined,
	message: string | undefined,
	negative: boolean,
	fn: Function,
): void {
	if (typeof expected === "string") {
		message = expected;
		expected = undefined;
	}
	if (
		expected !== undefined &&
		!(expected instanceof RegExp) &&
		typeof expected !== "function" &&
		(typeof expected !== "object" || expected === null)
	)
		throw invalidArgType("error", "a function, RegExp, or object", expected);
	if (thrown === noException) {
		if (!negative) error(undefined, expected, message ?? "Missing expected exception.", fn.name, fn);
		return;
	}
	if (negative) {
		if (expected === undefined || matches(thrown, expected))
			error(
				thrown,
				expected,
				message ?? `Got unwanted exception.\nActual message: "${(thrown as Error)?.message ?? thrown}"`,
				fn.name,
				fn,
			);
		throw thrown;
	}
	if (expected !== undefined && !matches(thrown, expected)) {
		if (typeof expected === "object" && !(expected instanceof RegExp)) error(thrown, expected, message, fn.name, fn);
		throw thrown;
	}
}
export function throws(block: () => unknown, expected?: Matcher | string, message?: string): void {
	if (typeof block !== "function") throw invalidArgType("fn", "of type function", block);
	let thrown: unknown = noException;
	try {
		block();
	} catch (e) {
		thrown = e;
	}
	check(thrown, expected, message, false, throws);
}
export function doesNotThrow(block: () => unknown, expected?: Matcher | string, message?: string): void {
	if (typeof block !== "function") throw invalidArgType("fn", "of type function", block);
	let thrown: unknown = noException;
	try {
		block();
	} catch (e) {
		thrown = e;
	}
	check(thrown, expected, message, true, doesNotThrow);
}
async function rejection(block: PromiseLike<unknown> | (() => PromiseLike<unknown>)): Promise<unknown> {
	let p: PromiseLike<unknown>;
	if (typeof block === "function") {
		p = block();
		if (!p || typeof p.then !== "function")
			throw codedError(
				"ERR_INVALID_RETURN_VALUE",
				'Expected instance of Promise to be returned from the "promiseFn" function.',
				TypeError,
			);
	} else {
		p = block;
		if (!p || typeof p.then !== "function") throw invalidArgType("promiseFn", "a function or Promise", block);
	}
	try {
		await p;
		return noException;
	} catch (e) {
		return e;
	}
}
export async function rejects(
	block: PromiseLike<unknown> | (() => PromiseLike<unknown>),
	expected?: Matcher | string,
	message?: string,
): Promise<void> {
	check(await rejection(block), expected, message, false, rejects);
}
export async function doesNotReject(
	block: PromiseLike<unknown> | (() => PromiseLike<unknown>),
	expected?: Matcher | string,
	message?: string,
): Promise<void> {
	check(await rejection(block), expected, message, true, doesNotReject);
}
export function match(value: string, regexp: RegExp, message?: string | Error): void {
	if (typeof value !== "string") throw invalidArgType("string", "of type string", value);
	if (!(regexp instanceof RegExp)) throw invalidArgType("regexp", "a RegExp", regexp);
	if (!regexp.test(value)) error(value, regexp, message, "match", match);
}
export function doesNotMatch(value: string, regexp: RegExp, message?: string | Error): void {
	if (typeof value !== "string") throw invalidArgType("string", "of type string", value);
	if (!(regexp instanceof RegExp)) throw invalidArgType("regexp", "a RegExp", regexp);
	if (regexp.test(value)) error(value, regexp, message, "doesNotMatch", doesNotMatch);
}
export function ifError(value: unknown): void {
	if (value !== null && value !== undefined)
		error(
			value,
			null,
			`ifError got unwanted exception: ${value instanceof Error ? value.message : inspect(value)}`,
			"ifError",
			ifError,
		);
}
export class CallTracker {
	private records: {
		fn: Function;
		wrapper: Function;
		expected: number;
		actual: number;
		stack: string;
		calls: { thisArg: unknown; arguments: unknown[] }[];
	}[] = [];
	calls(fn: Function = () => {}, exact = 1): Function {
		if (typeof fn === "number") {
			exact = fn;
			fn = () => {};
		}
		if (typeof fn !== "function") throw invalidArgType("fn", "of type function", fn);
		if (!Number.isInteger(exact) || exact < 0)
			throw codedError("ERR_OUT_OF_RANGE", "exact must be a non-negative integer", RangeError);
		const record = {
			fn,
			wrapper: fn,
			expected: exact,
			actual: 0,
			stack: new Error().stack ?? "",
			calls: [] as { thisArg: unknown; arguments: unknown[] }[],
		};
		const wrapper = function (this: unknown, ...args: unknown[]) {
			record.actual++;
			record.calls.push({ thisArg: this, arguments: args });
			return fn.apply(this, args);
		};
		record.wrapper = wrapper;
		this.records.push(record);
		return wrapper;
	}
	report(): { message: string; actual: number; expected: number; operator: string; stack: string }[] {
		return this.records
			.filter((r) => r.actual !== r.expected)
			.map((r) => ({
				message: `Expected the ${r.fn.name || "calls"} function to be executed ${r.expected} time(s) but was executed ${r.actual} time(s).`,
				actual: r.actual,
				expected: r.expected,
				operator: r.fn.name,
				stack: r.stack,
			}));
	}
	verify(): void {
		const report = this.report();
		if (report.length)
			throw new AssertionError({
				message: report[0].message,
				actual: report[0].actual,
				expected: report[0].expected,
				operator: "verify",
			});
	}
	reset(fn?: Function): void {
		for (const r of this.records)
			if (fn === undefined || r.wrapper === fn) {
				r.actual = 0;
				r.calls = [];
			}
	}
	getCalls(fn: Function): { thisArg: unknown; arguments: unknown[] }[] {
		const record = this.records.find((r) => r.wrapper === fn);
		if (!record)
			throw codedError("ERR_INVALID_ARG_VALUE", "The function is not tracked by this CallTracker", TypeError);
		return record.calls.map((call) => ({ thisArg: call.thisArg, arguments: call.arguments.slice() }));
	}
}
const assert = Object.assign(ok, {
	AssertionError,
	ok,
	fail,
	equal,
	notEqual,
	strictEqual,
	notStrictEqual,
	deepEqual,
	notDeepEqual,
	deepStrictEqual,
	notDeepStrictEqual,
	partialDeepStrictEqual,
	throws,
	doesNotThrow,
	rejects,
	doesNotReject,
	match,
	doesNotMatch,
	ifError,
	CallTracker,
});
export const strict = Object.assign(
	function strict(value: unknown, message?: string | Error) {
		ok(value, message);
	},
	assert,
	{ equal: strictEqual, notEqual: notStrictEqual, deepEqual: deepStrictEqual, notDeepEqual: notDeepStrictEqual },
);
Object.assign(strict, { strict });
Object.assign(assert, { strict });
export default assert;
