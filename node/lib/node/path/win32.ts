// node:path/win32
import { win32 } from "node:path";
export const {
	sep,
	delimiter,
	resolve,
	normalize,
	isAbsolute,
	join,
	relative,
	toNamespacedPath,
	dirname,
	basename,
	extname,
	format,
	parse,
	matchesGlob,
} = win32;
export default win32;
