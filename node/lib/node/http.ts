// node:http — HTTP/1.1 client and server (implementation in internal:http/*).

import { Agent, globalAgent } from "internal:http/agent";
import { ClientRequest, type ClientRequestArgs } from "internal:http/client";
import { IncomingMessage } from "internal:http/incoming";
import { OutgoingMessage } from "internal:http/outgoing";
import { Server, ServerResponse, createServer, type ServerOptions } from "internal:http/server";
import { METHODS, STATUS_CODES, maxHeaderSize, validateHeaderName, validateHeaderValue } from "internal:http/common";
import { validateNumber } from "internal:errors";

export {
	Agent,
	globalAgent,
	ClientRequest,
	IncomingMessage,
	OutgoingMessage,
	Server,
	ServerResponse,
	createServer,
	METHODS,
	STATUS_CODES,
	maxHeaderSize,
	validateHeaderName,
	validateHeaderValue,
};
export type { ServerOptions, ClientRequestArgs };

type ResponseListener = (res: IncomingMessage) => void;

export function request(
	url: string | URL | ClientRequestArgs,
	options?: ClientRequestArgs | ResponseListener,
	cb?: ResponseListener,
): ClientRequest {
	return new ClientRequest(url, options, cb);
}

export function get(
	url: string | URL | ClientRequestArgs,
	options?: ClientRequestArgs | ResponseListener,
	cb?: ResponseListener,
): ClientRequest {
	const req = new ClientRequest(url, options, cb);
	req.end();
	return req;
}

/** parsers are not pooled by qbun; kept for API compatibility */
export function setMaxIdleHTTPParsers(max: number): void {
	validateNumber(max, "max", 1);
}

const http = {
	Agent,
	ClientRequest,
	IncomingMessage,
	OutgoingMessage,
	Server,
	ServerResponse,
	METHODS,
	STATUS_CODES,
	createServer,
	get,
	request,
	maxHeaderSize,
	validateHeaderName,
	validateHeaderValue,
	setMaxIdleHTTPParsers,
};

Object.defineProperty(http, "globalAgent", {
	configurable: true,
	enumerable: true,
	get: () => Agent.globalAgent,
	set: (agent: Agent) => {
		Agent.globalAgent = agent;
	},
});

// Node exposes the WHATWG WebSocket, CloseEvent and MessageEvent on node:http
for (const name of ["WebSocket", "CloseEvent", "MessageEvent"]) {
	Object.defineProperty(http, name, {
		configurable: true,
		enumerable: true,
		get: () => (globalThis as unknown as Record<string, unknown>)[name],
	});
}

export default http;
