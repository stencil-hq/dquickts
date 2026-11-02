"""L0 golden: hashlib digests and zlib round trips/checksums on fixed bytes.

Compressed byte streams are not printed (they depend on the zlib build); only
round-trip results and checksums are.
"""
import binascii
import hashlib
import hmac
import zlib

msg = b"The quick brown fox jumps over the lazy dog"

for name in ["md5", "sha1", "sha224", "sha256", "sha384", "sha512", "sha3_256", "blake2b", "blake2s"]:
    h = hashlib.new(name, msg)
    print(name, h.name, h.digest_size, h.block_size, h.hexdigest())

print("sha256 empty", hashlib.sha256().hexdigest())
print("sha256 b''", hashlib.sha256(b"").hexdigest())
print("md5 abc", hashlib.md5(b"abc").hexdigest())
print("sha1 abc", hashlib.sha1(b"abc").hexdigest())
print("blake2b 16", hashlib.blake2b(b"abc", digest_size=16).hexdigest())
print("blake2b key", hashlib.blake2b(b"abc", key=b"secret", digest_size=20).hexdigest())
print("blake2b person", hashlib.blake2b(b"abc", person=b"me", digest_size=8).hexdigest())
print("shake_128", hashlib.shake_128(b"abc").hexdigest(10))

h = hashlib.sha256()
for chunk in (b"The quick ", b"brown fox ", b"jumps over the lazy dog"):
    h.update(chunk)
print("incremental equal", h.hexdigest() == hashlib.sha256(msg).hexdigest())
c = h.copy()
c.update(b"!")
print("copy diverges", c.hexdigest() != h.hexdigest(), h.hexdigest() == hashlib.sha256(msg).hexdigest())
print("digest bytes", hashlib.md5(b"").digest())
print("usedforsecurity", hashlib.md5(b"x", usedforsecurity=False).hexdigest())
print("memoryview input", hashlib.sha1(memoryview(b"view")).hexdigest())
try:
    hashlib.sha256("text")
except TypeError as e:
    print("str input", type(e).__name__, e)
try:
    hashlib.new("nope")
except ValueError as e:
    print("unknown algo", type(e).__name__, e)
print("guaranteed has sha256", "sha256" in hashlib.algorithms_guaranteed)
print("pbkdf2", hashlib.pbkdf2_hmac("sha256", b"password", b"salt", 1000).hex())
print("hmac", hmac.new(b"key", msg, "sha256").hexdigest())
print("hmac digest", hmac.digest(b"key", b"m", "md5").hex())
print("compare_digest", hmac.compare_digest(b"a", b"a"), hmac.compare_digest("a", "b"))

# zlib
data = b"hello hello hello hello " * 20
comp = zlib.compress(data)
print("compress smaller", len(comp) < len(data))
print("roundtrip", zlib.decompress(comp) == data)
for level in (0, 1, 6, 9, -1):
    print("level", level, zlib.decompress(zlib.compress(data, level)) == data)
print("level 0 bigger", len(zlib.compress(data, 0)) > len(data))
print("empty", zlib.decompress(zlib.compress(b"")))
print("crc32", zlib.crc32(b"hello world"), zlib.crc32(b""), zlib.crc32(b"world", zlib.crc32(b"hello ")))
print("adler32", zlib.adler32(b"hello world"), zlib.adler32(b""), zlib.adler32(b"Wikipedia"))
print("crc32 == binascii", zlib.crc32(msg) == binascii.crc32(msg))

co = zlib.compressobj(9, zlib.DEFLATED, -15)
raw = co.compress(data) + co.flush()
print("raw deflate", zlib.decompress(raw, -15) == data)
co = zlib.compressobj(wbits=31)
gz = co.compress(data) + co.flush()
print("gzip header", gz[:2], zlib.decompress(gz, 47) == data)

do = zlib.decompressobj()
pieces = [do.decompress(comp[i:i + 7]) for i in range(0, len(comp), 7)]
print("streaming", b"".join(pieces) + do.flush() == data, do.eof, do.unused_data)
do = zlib.decompressobj()
out = do.decompress(comp + b"TRAILER")
print("unused_data", out == data, do.unused_data)
do = zlib.decompressobj()
first = do.decompress(comp, 10)
print("max_length", len(first), len(do.unconsumed_tail) > 0)

try:
    zlib.decompress(b"not compressed")
except zlib.error as e:
    print("bad data", type(e).__name__, e)
try:
    zlib.decompress(comp[:-5])
except zlib.error as e:
    print("truncated", type(e).__name__, e)
try:
    zlib.compress(data, 10)
except zlib.error as e:
    print("bad level", type(e).__name__, e)
print("constants", zlib.MAX_WBITS, zlib.DEFLATED, zlib.Z_BEST_COMPRESSION, zlib.Z_DEFAULT_COMPRESSION)
