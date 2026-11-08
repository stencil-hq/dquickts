// RFC 3492 Bootstring (Punycode).
export const version = "2.3.1";
const maxInt = 2147483647;
function overflow(): never {
	throw new RangeError("Overflow: input needs wider integers to process");
}
function adapt(delta: number, points: number, first: boolean): number {
	delta = first ? Math.floor(delta / 700) : delta >> 1;
	delta += Math.floor(delta / points);
	let k = 0;
	while (delta > 455) {
		delta = Math.floor(delta / 35);
		k += 36;
	}
	return k + Math.floor((36 * delta) / (delta + 38));
}
export const ucs2 = {
	decode(input: string): number[] {
		const result: number[] = [];
		for (let i = 0; i < input.length; i++) {
			const c = input.charCodeAt(i);
			if (c >= 0xd800 && c <= 0xdbff && i + 1 < input.length) {
				const d = input.charCodeAt(i + 1);
				if ((d & 0xfc00) === 0xdc00) {
					result.push(((c & 1023) << 10) + (d & 1023) + 65536);
					i++;
					continue;
				}
			}
			result.push(c);
		}
		return result;
	},
	encode(points: number[]): string {
		let out = "";
		for (const c of points) out += String.fromCodePoint(c);
		return out;
	},
};
export function decode(input: string): string {
	const output: number[] = [];
	let n = 128,
		i = 0,
		bias = 72,
		basic = input.lastIndexOf("-"),
		index = 0;
	if (basic >= 0) {
		for (let j = 0; j < basic; j++) {
			const c = input.charCodeAt(j);
			if (c >= 128) throw new RangeError("Illegal input >= 0x80 (not a basic code point)");
			output.push(c);
		}
		index = basic + 1;
	}
	while (index < input.length) {
		const old = i;
		let w = 1;
		for (let k = 36; ; k += 36) {
			if (index >= input.length) throw new RangeError("Invalid input");
			const c = input.charCodeAt(index++),
				digit = c >= 48 && c <= 57 ? c - 22 : c >= 65 && c <= 90 ? c - 65 : c >= 97 && c <= 122 ? c - 97 : 36;
			if (digit >= 36 || digit > Math.floor((maxInt - i) / w)) overflow();
			i += digit * w;
			const t = k <= bias ? 1 : k >= bias + 26 ? 26 : k - bias;
			if (digit < t) break;
			if (w > Math.floor(maxInt / (36 - t))) overflow();
			w *= 36 - t;
		}
		const size = output.length + 1;
		bias = adapt(i - old, size, old === 0);
		if (Math.floor(i / size) > maxInt - n) overflow();
		n += Math.floor(i / size);
		i %= size;
		output.splice(i++, 0, n);
	}
	return ucs2.encode(output);
}
export function encode(input: string): string {
	const points = ucs2.decode(input),
		output: string[] = [];
	let n = 128,
		delta = 0,
		bias = 72;
	for (const c of points) if (c < 128) output.push(String.fromCharCode(c));
	let handled = output.length;
	const basic = handled;
	if (basic) output.push("-");
	while (handled < points.length) {
		let m = maxInt;
		for (const c of points) if (c >= n && c < m) m = c;
		if (m - n > Math.floor((maxInt - delta) / (handled + 1))) overflow();
		delta += (m - n) * (handled + 1);
		n = m;
		for (const c of points) {
			if (c < n && ++delta > maxInt) overflow();
			if (c === n) {
				let q = delta;
				for (let k = 36; ; k += 36) {
					const t = k <= bias ? 1 : k >= bias + 26 ? 26 : k - bias;
					if (q < t) break;
					const d = t + ((q - t) % (36 - t));
					output.push(String.fromCharCode(d + 22 + 75 * Number(d < 26)));
					q = Math.floor((q - t) / (36 - t));
				}
				output.push(String.fromCharCode(q + 22 + 75 * Number(q < 26)));
				bias = adapt(delta, handled + 1, handled === basic);
				delta = 0;
				handled++;
			}
		}
		delta++;
		n++;
	}
	return output.join("");
}
function mapDomain(input: string, convert: (label: string) => string): string {
	const at = input.indexOf("@"),
		prefix = at < 0 ? "" : input.slice(0, at + 1),
		domain = at < 0 ? input : input.slice(at + 1);
	return (
		prefix +
		domain
			.replace(/[\u3002\uff0e\uff61]/g, ".")
			.split(".")
			.map(convert)
			.join(".")
	);
}
export function toASCII(input: string): string {
	return mapDomain(input, (label) => (/[^\x00-\x7f]/.test(label) ? "xn--" + encode(label) : label));
}
export function toUnicode(input: string): string {
	return mapDomain(input, (label) => (/^xn--/.test(label) ? decode(label.slice(4).toLowerCase()) : label));
}
export default { version, ucs2, decode, encode, toASCII, toUnicode };
