// node:os — operating-system information backed by libuv.
import * as native from "qbun:os";
import * as core from "qbun:core";
import process from "node:process";
import { Buffer } from "node:buffer";
import { invalidArgType, outOfRange } from "internal:errors";

export const EOL = "\n";
export const devNull = "/dev/null";
export const constants = Object.assign({}, native.constants, { UV_UDP_REUSEADDR: 4 });
Object.freeze(constants.signals);
const info = core.info() as { arch: string; platform: string };
export function arch(): string {
	return info.arch;
}
export function platform(): string {
	return info.platform;
}
export function type(): string {
	return native.uname().sysname;
}
export function release(): string {
	return native.uname().release;
}
export function version(): string {
	return native.uname().version;
}
export function machine(): string {
	return native.uname().machine;
}
export function hostname(): string {
	return native.hostname();
}
export function homedir(): string {
	return process.env.HOME || native.homedir();
}
export function tmpdir(): string {
	const path = process.env.TMPDIR || process.env.TMP || process.env.TEMP || native.tmpdir();
	return path.length > 1 && path.endsWith("/") ? path.slice(0, -1) : path;
}
export function uptime(): number {
	return native.uptime();
}
export function loadavg(): [number, number, number] {
	return native.loadavg();
}
export function totalmem(): number {
	return native.totalmem();
}
export function freemem(): number {
	return native.freemem();
}
export function availableParallelism(): number {
	return native.availableParallelism();
}
export interface CpuInfo {
	model: string;
	speed: number;
	times: { user: number; nice: number; sys: number; idle: number; irq: number };
}
export function cpus(): CpuInfo[] {
	return native.cpus();
}
export interface NetworkInterfaceInfo {
	address: string;
	netmask: string;
	family: "IPv4" | "IPv6";
	mac: string;
	internal: boolean;
	cidr: string | null;
	scopeid?: number;
}
export function networkInterfaces(): Record<string, NetworkInterfaceInfo[]> {
	return native.networkInterfaces();
}
export function endianness(): "LE" | "BE" {
	return new Uint8Array(new Uint16Array([1]).buffer)[0] === 1 ? "LE" : "BE";
}
export function userInfo(options?: { encoding?: string }): {
	uid: number;
	gid: number;
	username: string | Buffer;
	homedir: string | Buffer;
	shell: string | Buffer | null;
} {
	if (options !== undefined && (options === null || typeof options !== "object"))
		throw invalidArgType("options", "of type object", options);
	const result = native.userInfo();
	if (options?.encoding === "buffer")
		return {
			...result,
			username: Buffer.from(result.username),
			homedir: Buffer.from(result.homedir),
			shell: result.shell === null ? null : Buffer.from(result.shell),
		};
	if (options?.encoding !== undefined) {
		if (!Buffer.isEncoding(options.encoding)) throw invalidArgType("encoding", "a valid encoding", options.encoding);
		return {
			...result,
			username: Buffer.from(result.username).toString(options.encoding),
			homedir: Buffer.from(result.homedir).toString(options.encoding),
			shell: result.shell === null ? null : Buffer.from(result.shell).toString(options.encoding),
		};
	}
	return result;
}
function validatePid(pid: number): void {
	if (typeof pid !== "number") throw invalidArgType("pid", "of type number", pid);
	if (!Number.isInteger(pid) || pid < -2147483648 || pid > 2147483647)
		throw outOfRange("pid", "a signed 32-bit integer", pid);
}
export function getPriority(pid = 0): number {
	validatePid(pid);
	return native.getPriority(pid);
}
export function setPriority(pid: number, priority?: number): void {
	if (priority === undefined) {
		priority = pid;
		pid = 0;
	}
	validatePid(pid);
	if (typeof priority !== "number") throw invalidArgType("priority", "of type number", priority);
	if (!Number.isInteger(priority) || priority < -20 || priority > 19)
		throw outOfRange("priority", ">= -20 && <= 19", priority);
	native.setPriority(pid, priority);
}
for (const fn of [
	arch,
	platform,
	type,
	release,
	version,
	machine,
	hostname,
	homedir,
	tmpdir,
	uptime,
	totalmem,
	freemem,
	availableParallelism,
	endianness,
]) {
	Object.defineProperty(fn, Symbol.toPrimitive, { value: fn });
}
export default {
	EOL,
	devNull,
	constants,
	arch,
	platform,
	type,
	release,
	version,
	machine,
	hostname,
	homedir,
	tmpdir,
	uptime,
	loadavg,
	totalmem,
	freemem,
	availableParallelism,
	cpus,
	networkInterfaces,
	endianness,
	userInfo,
	getPriority,
	setPriority,
};
