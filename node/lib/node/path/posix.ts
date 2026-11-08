// node:path/posix
import { posix } from "node:path";
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
} = posix;
export default posix;
