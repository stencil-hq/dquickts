import hf_xet, os, tempfile
d = tempfile.mkdtemp()
paths = []
for i, size in enumerate([0, 10, 100000, 3 * 1024 * 1024]):
    p = os.path.join(d, f"f{i}.bin")
    with open(p, "wb") as f:
        f.write(bytes((j * 31 + i) % 251 for j in range(size)))
    paths.append(p)
res = hf_xet.hash_files(paths)
for r in res:
    print(type(r).__name__, r.hash if hasattr(r, "hash") else r, getattr(r, "filesize", None))
