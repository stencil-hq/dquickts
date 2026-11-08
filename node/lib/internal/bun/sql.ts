import { notSupported } from "internal:errors";
class UnsupportedClient {
	constructor(..._args: unknown[]) {}
	connect(): never {
		throw notSupported("Database connections");
	}
	query(..._args: unknown[]): never {
		throw notSupported("Database queries");
	}
	unsafe(..._args: unknown[]): never {
		throw notSupported("Database queries");
	}
	close(): Promise<void> {
		return Promise.resolve();
	}
	end(): Promise<void> {
		return this.close();
	}
}
export class SQL extends UnsupportedClient {}
export const sql = Object.assign(
	(..._args: unknown[]): never => {
		throw notSupported("SQL");
	},
	{
		connect: (): never => {
			throw notSupported("SQL");
		},
		close: () => Promise.resolve(),
		unsafe: (..._args: unknown[]): never => {
			throw notSupported("SQL");
		},
	},
);
export class RedisClient extends UnsupportedClient {
	send(..._args: unknown[]): never {
		throw notSupported("Redis");
	}
	get(..._args: unknown[]): never {
		throw notSupported("Redis");
	}
	set(..._args: unknown[]): never {
		throw notSupported("Redis");
	}
}
export const redis = new RedisClient();
export class S3Client extends UnsupportedClient {
	file(..._args: unknown[]): never {
		throw notSupported("S3");
	}
	write(..._args: unknown[]): never {
		throw notSupported("S3");
	}
	delete(..._args: unknown[]): never {
		throw notSupported("S3");
	}
	exists(..._args: unknown[]): never {
		throw notSupported("S3");
	}
	presign(..._args: unknown[]): never {
		throw notSupported("S3");
	}
}
export const s3 = new S3Client();
// Every command is deliberately rejected rather than silently performing local work.
for (const name of "append auth blpop brpop bzpopmax bzpopmin decr decrby del echo eval evalsha exists expire expireat flushall flushdb get getdel getex getrange getset hdel hexists hget hgetall hincrby hincrbyfloat hkeys hlen hmget hmset hset hsetnx hvals incr incrby incrbyfloat keys lindex llen lpop lpush lrange lrem lset ltrim mget mset msetnx persist pexpire pexpireat ping psetex pttl publish quit rename renamenx rpop rpush sadd scard sdiff set setex setnx sismember smembers smismember spop srandmember srem strlen subscribe ttl type unlink unsubscribe watch unwatch zadd zcard zcount zincrby zrange zrank zrem zrevrange zrevrank zscore".split(
	" ",
)) {
	if (name in RedisClient.prototype) continue;
	Object.defineProperty(RedisClient.prototype, name, {
		value: function (..._args: unknown[]): never {
			throw notSupported(`Redis.${name}`);
		},
		writable: true,
		configurable: true,
	});
}
for (const name of ["file", "write", "delete", "exists", "presign", "size", "stat", "list"]) {
	if (!(name in S3Client.prototype))
		Object.defineProperty(S3Client.prototype, name, {
			value: function (..._args: unknown[]): never {
				throw notSupported(`S3.${name}`);
			},
			writable: true,
			configurable: true,
		});
	Object.defineProperty(S3Client, name, {
		value: function (..._args: unknown[]): never {
			throw notSupported(`S3.${name}`);
		},
		writable: true,
		configurable: true,
	});
}
for (const name of ["begin", "transaction", "reserve", "savepoint", "distributed", "file"])
	Object.defineProperty(SQL.prototype, name, {
		value: function (..._args: unknown[]): never {
			throw notSupported(`SQL.${name}`);
		},
		writable: true,
		configurable: true,
	});
