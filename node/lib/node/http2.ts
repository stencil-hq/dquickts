import { notSupported } from "internal:errors";
export const sensitiveHeaders = Symbol("nodejs.http2.sensitiveHeaders");
export function connect(): never {
	throw notSupported("HTTP/2");
}
export const createServer = connect,
	createSecureServer = connect;
export const constants = {
	NGHTTP2_NO_ERROR: 0,
	NGHTTP2_PROTOCOL_ERROR: 1,
	NGHTTP2_INTERNAL_ERROR: 2,
	NGHTTP2_FLOW_CONTROL_ERROR: 3,
	NGHTTP2_SETTINGS_TIMEOUT: 4,
	NGHTTP2_STREAM_CLOSED: 5,
	NGHTTP2_FRAME_SIZE_ERROR: 6,
	NGHTTP2_REFUSED_STREAM: 7,
	NGHTTP2_CANCEL: 8,
	NGHTTP2_COMPRESSION_ERROR: 9,
	NGHTTP2_CONNECT_ERROR: 10,
	NGHTTP2_ENHANCE_YOUR_CALM: 11,
	NGHTTP2_INADEQUATE_SECURITY: 12,
	NGHTTP2_HTTP_1_1_REQUIRED: 13,
	HTTP2_HEADER_STATUS: ":status",
	HTTP2_HEADER_METHOD: ":method",
	HTTP2_HEADER_PATH: ":path",
	HTTP2_HEADER_SCHEME: ":scheme",
	HTTP2_HEADER_AUTHORITY: ":authority",
	HTTP2_METHOD_GET: "GET",
	HTTP2_METHOD_POST: "POST",
	HTTP_STATUS_OK: 200,
	DEFAULT_SETTINGS_HEADER_TABLE_SIZE: 4096,
	DEFAULT_SETTINGS_ENABLE_PUSH: 1,
	DEFAULT_SETTINGS_INITIAL_WINDOW_SIZE: 65535,
	DEFAULT_SETTINGS_MAX_FRAME_SIZE: 16384,
};
export function getDefaultSettings(): object {
	return {
		headerTableSize: 4096,
		enablePush: true,
		initialWindowSize: 65535,
		maxFrameSize: 16384,
		maxConcurrentStreams: 4294967295,
		maxHeaderListSize: 65535,
	};
}
const settingIds: Record<string, number> = {
	headerTableSize: 1,
	enablePush: 2,
	maxConcurrentStreams: 3,
	initialWindowSize: 4,
	maxFrameSize: 5,
	maxHeaderListSize: 6,
	enableConnectProtocol: 8,
};
export function getPackedSettings(settings: Record<string, number | boolean>): Uint8Array {
	const pairs: [number, number][] = [];
	for (const key in settings) if (settingIds[key]) pairs.push([settingIds[key], Number(settings[key])]);
	const out = new Uint8Array(pairs.length * 6),
		view = new DataView(out.buffer);
	for (let i = 0; i < pairs.length; i++) {
		view.setUint16(i * 6, pairs[i][0]);
		view.setUint32(i * 6 + 2, pairs[i][1]);
	}
	return out;
}
export function getUnpackedSettings(buffer: Uint8Array): Record<string, number | boolean> {
	if (buffer.byteLength % 6) throw new RangeError("Invalid packed settings length");
	const out: Record<string, number | boolean> = {},
		view = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);
	for (let i = 0; i < buffer.byteLength; i += 6) {
		const id = view.getUint16(i),
			value = view.getUint32(i + 2);
		for (const key in settingIds)
			if (settingIds[key] === id) out[key] = key === "enablePush" || key === "enableConnectProtocol" ? !!value : value;
	}
	return out;
}
export const performServerHandshake = connect;
Object.assign(constants, {
	NGHTTP2_SESSION_SERVER: 0,
	NGHTTP2_SESSION_CLIENT: 1,
	NGHTTP2_STREAM_STATE_IDLE: 1,
	NGHTTP2_STREAM_STATE_OPEN: 2,
	NGHTTP2_STREAM_STATE_RESERVED_LOCAL: 3,
	NGHTTP2_STREAM_STATE_RESERVED_REMOTE: 4,
	NGHTTP2_STREAM_STATE_HALF_CLOSED_LOCAL: 5,
	NGHTTP2_STREAM_STATE_HALF_CLOSED_REMOTE: 6,
	NGHTTP2_STREAM_STATE_CLOSED: 7,
	NGHTTP2_FLAG_NONE: 0,
	NGHTTP2_FLAG_END_STREAM: 1,
	NGHTTP2_FLAG_END_HEADERS: 4,
	NGHTTP2_FLAG_ACK: 1,
	NGHTTP2_FLAG_PADDED: 8,
	NGHTTP2_FLAG_PRIORITY: 32,
	NGHTTP2_SETTINGS_HEADER_TABLE_SIZE: 1,
	NGHTTP2_SETTINGS_ENABLE_PUSH: 2,
	NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS: 3,
	NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE: 4,
	NGHTTP2_SETTINGS_MAX_FRAME_SIZE: 5,
	NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE: 6,
	NGHTTP2_DEFAULT_WEIGHT: 16,
});
const headers =
	"accept accept-charset accept-encoding accept-language accept-ranges access-control-allow-origin age allow authorization cache-control connection content-disposition content-encoding content-language content-length content-location content-range content-type cookie date etag expect expires from host if-match if-modified-since if-none-match if-range if-unmodified-since last-modified link location max-forwards proxy-authenticate proxy-authorization range referer refresh retry-after server set-cookie strict-transport-security te trailer transfer-encoding upgrade user-agent vary via warning www-authenticate";
for (const header of headers.split(" "))
	Object.assign(constants, { ["HTTP2_HEADER_" + header.replace(/-/g, "_").toUpperCase()]: header });
for (const method of ["DELETE", "GET", "HEAD", "POST", "PUT", "CONNECT", "OPTIONS", "TRACE", "PATCH"])
	Object.assign(constants, { ["HTTP2_METHOD_" + method]: method });
export default {
	connect,
	createServer,
	createSecureServer,
	constants,
	sensitiveHeaders,
	getDefaultSettings,
	getPackedSettings,
	getUnpackedSettings,
	performServerHandshake,
};
