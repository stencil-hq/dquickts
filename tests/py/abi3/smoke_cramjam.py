import cramjam
data = b"hello abi3 " * 100
for name in ("snappy", "lz4", "zstd", "brotli", "gzip", "deflate", "bzip2"):
    mod = getattr(cramjam, name)
    c = mod.compress(data)
    d = mod.decompress(c)
    print(name, bytes(d) == data, len(bytes(d)))
buf = cramjam.Buffer(b"abc")
print(bytes(buf.read()), len(buf))
