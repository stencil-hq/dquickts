import struct
from safetensors import safe_open
from safetensors import _safetensors_rust as st
data = struct.pack("<4f", 1.0, 2.0, 3.0, 4.0)
blob = st.serialize({"w": {"dtype": "float32", "shape": [2, 2], "data": data}}, metadata={"k": "v"})
print(type(blob).__name__, len(blob))
out = st.deserialize(blob)
for name, info in out:
    print(name, info["dtype"], info["shape"], struct.unpack("<4f", bytes(info["data"])))
import tempfile, os
with tempfile.TemporaryDirectory() as d:
    fn = os.path.join(d, "t.safetensors")
    with open(fn, "wb") as f:
        f.write(blob)
    with safe_open(fn, framework="numpy" if False else "pt") as f:
        print(sorted(f.keys()), f.metadata())
