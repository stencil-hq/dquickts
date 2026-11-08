// IP address parsing and formatting shared by node:net, node:dns and node:http.
// Regexes from Node.js lib/internal/net.js (MIT).

const v4Seg = "(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9][0-9]|[0-9])";
const v4Str = `(?:${v4Seg}\\.){3}${v4Seg}`;
const IPv4Reg = new RegExp(`^${v4Str}$`);
const v6Seg = "(?:[0-9a-fA-F]{1,4})";
const IPv6Reg = new RegExp(
	"^(?:" +
		`(?:${v6Seg}:){7}(?:${v6Seg}|:)|` +
		`(?:${v6Seg}:){6}(?:${v4Str}|:${v6Seg}|:)|` +
		`(?:${v6Seg}:){5}(?::${v4Str}|(?::${v6Seg}){1,2}|:)|` +
		`(?:${v6Seg}:){4}(?:(?::${v6Seg}){0,1}:${v4Str}|(?::${v6Seg}){1,3}|:)|` +
		`(?:${v6Seg}:){3}(?:(?::${v6Seg}){0,2}:${v4Str}|(?::${v6Seg}){1,4}|:)|` +
		`(?:${v6Seg}:){2}(?:(?::${v6Seg}){0,3}:${v4Str}|(?::${v6Seg}){1,5}|:)|` +
		`(?:${v6Seg}:){1}(?:(?::${v6Seg}){0,4}:${v4Str}|(?::${v6Seg}){1,6}|:)|` +
		`(?::(?:(?::${v6Seg}){0,5}:${v4Str}|(?::${v6Seg}){1,7}|:))` +
		")(?:%[0-9a-zA-Z-.:]{1,})?$",
);

export function isIPv4(input: unknown): boolean {
	return IPv4Reg.test(String(input));
}

export function isIPv6(input: unknown): boolean {
	const s = String(input);
	if (s.length < 2 || !s.includes(":")) return false;
	return IPv6Reg.test(s);
}

export function isIP(input: unknown): 0 | 4 | 6 {
	if (isIPv4(input)) return 4;
	if (isIPv6(input)) return 6;
	return 0;
}

export function parseIPv4(s: string): number {
	const p = s.split(".");
	return ((+p[0] << 24) >>> 0) + (+p[1] << 16) + (+p[2] << 8) + +p[3];
}

export function formatIPv4(n: number): string {
	return `${(n >>> 24) & 255}.${(n >>> 16) & 255}.${(n >>> 8) & 255}.${n & 255}`;
}

/** 128-bit value of an IPv6 address (zone id ignored); null when invalid */
export function parseIPv6(input: string): bigint | null {
	let s = input;
	const pct = s.indexOf("%");
	if (pct >= 0) s = s.slice(0, pct);
	if (!isIPv6(s)) return null;
	let tail: number[] = [];
	const lastColon = s.lastIndexOf(":");
	const lastPart = s.slice(lastColon + 1);
	if (lastPart.includes(".")) {
		const v4 = parseIPv4(lastPart);
		tail = [v4 >>> 16, v4 & 0xffff];
		s = `${s.slice(0, lastColon + 1)}${tail.length ? "0:0" : ""}`;
	}
	const halves = s.split("::");
	const head = halves[0] ? halves[0].split(":") : [];
	const rest = halves.length > 1 && halves[1] ? halves[1].split(":") : [];
	const groups: number[] = [];
	for (const g of head) groups.push(parseInt(g, 16));
	if (halves.length > 1) {
		const fill = 8 - head.length - rest.length;
		for (let i = 0; i < fill; i++) groups.push(0);
	}
	for (const g of rest) groups.push(parseInt(g, 16));
	if (tail.length) {
		groups[6] = tail[0];
		groups[7] = tail[1];
	}
	if (groups.length !== 8) return null;
	let v = 0n;
	for (const g of groups) v = (v << 16n) | BigInt(g & 0xffff);
	return v;
}

/** RFC 5952 text of a 128-bit IPv6 value */
export function formatIPv6(v: bigint): string {
	const groups: number[] = [];
	for (let i = 7; i >= 0; i--) groups.push(Number((v >> BigInt(i * 16)) & 0xffffn));
	// IPv4-mapped
	if (
		groups[0] === 0 &&
		groups[1] === 0 &&
		groups[2] === 0 &&
		groups[3] === 0 &&
		groups[4] === 0 &&
		groups[5] === 0xffff
	) {
		return `::ffff:${formatIPv4(((groups[6] << 16) | groups[7]) >>> 0)}`;
	}
	let bestStart = -1;
	let bestLen = 0;
	for (let i = 0; i < 8; ) {
		if (groups[i] !== 0) {
			i++;
			continue;
		}
		let j = i;
		while (j < 8 && groups[j] === 0) j++;
		if (j - i > bestLen && j - i >= 2) {
			bestStart = i;
			bestLen = j - i;
		}
		i = j;
	}
	const hex = groups.map((g) => g.toString(16));
	if (bestStart < 0) return hex.join(":");
	return `${hex.slice(0, bestStart).join(":")}::${hex.slice(bestStart + bestLen).join(":")}`;
}
