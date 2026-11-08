import { Session as CallbackSession, open, close, url, waitForDebugger, console } from "node:inspector";
export { open, close, url, waitForDebugger, console };
export class Session extends CallbackSession {
	post(method: string, params?: object): Promise<object> {
		const p = Promise.withResolvers<object>();
		try {
			super.post(method, params, (error, result) => (error ? p.reject(error) : p.resolve(result ?? {})));
		} catch (error) {
			p.reject(error);
		}
		return p.promise;
	}
}
export default { Session, open, close, url, waitForDebugger, console };
