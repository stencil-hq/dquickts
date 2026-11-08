"""L0 golden: json.dumps / json.dump serialization options."""
import io
import json

data = {"b": [1, 2.5, None], "a": True, "c": {"z": "x", "y": False}}

print("default", json.dumps(data))
print("sort_keys", json.dumps(data, sort_keys=True))
print("compact", json.dumps(data, separators=(",", ":")))
print("custom seps", json.dumps([1, {"k": 2}], separators=(" ; ", " = ")))
print("indent 2")
print(json.dumps(data, indent=2, sort_keys=True))
print("indent tab")
print(json.dumps({"x": [1, {"y": []}], "e": {}}, indent="\t"))
print("indent 0")
print(json.dumps([1, [2]], indent=0))
print("indent seps", repr(json.dumps({"a": [1, 2]}, indent=1)))

# scalars
print("none", json.dumps(None))
print("bools", json.dumps([True, False]))
print("int", json.dumps(12345678901234567890123))
print("neg int", json.dumps(-7))
print("float", json.dumps([0.1, 1.0, -0.0, 1e100, 1.5e-7, 3.14]))
print("tuple", json.dumps((1, (2, 3))))
print("str esc", json.dumps('quote" back\\ nl\n tab\t ctl\x01'))
print("empty", json.dumps([]), json.dumps({}), json.dumps(""))

# non-ASCII
s = {"name": "café ☕ 日本 \U0001f600"}
print("ascii", json.dumps(s))
print("unicode", json.dumps(s, ensure_ascii=False))
print("lone surrogate", json.dumps("\ud800"))

# dict keys conversion
print("keys", json.dumps({1: "i", 2.5: "f", True: "t", None: "n"}))
try:
    json.dumps({(1, 2): "tuple key"})
except TypeError as e:
    print("tuple key", type(e).__name__, e)
print("skipkeys", json.dumps({(1, 2): "x", "ok": 1}, skipkeys=True))

# NaN / Infinity
special = [float("nan"), float("inf"), float("-inf")]
print("special", json.dumps(special))
try:
    json.dumps(special, allow_nan=False)
except ValueError as e:
    print("allow_nan", type(e).__name__, e)

# default=
class Point:
    def __init__(self, x, y):
        self.x, self.y = x, y


def encode(obj):
    if isinstance(obj, Point):
        return {"x": obj.x, "y": obj.y}
    if isinstance(obj, set):
        return sorted(obj)
    raise TypeError(f"cannot encode {type(obj).__name__}")


print("default fn", json.dumps({"p": Point(1, 2), "s": {3, 1, 2}}, default=encode))
try:
    json.dumps({"bad": object()}, default=encode)
except TypeError as e:
    print("default raise", type(e).__name__, e)
try:
    json.dumps(Point(0, 0))
except TypeError as e:
    print("no default", type(e).__name__, e)
try:
    json.dumps(b"bytes")
except TypeError as e:
    print("bytes", type(e).__name__, e)


class Enc(json.JSONEncoder):
    def default(self, o):
        if isinstance(o, complex):
            return [o.real, o.imag]
        return super().default(o)


print("encoder cls", json.dumps({"c": 1 + 2j}, cls=Enc))

# circular reference
lst = [1]
lst.append(lst)
try:
    json.dumps(lst)
except ValueError as e:
    print("circular", type(e).__name__, e)

# nested
nested = {"l1": {"l2": {"l3": [1, [2, [3, {"deep": "yes"}]]]}}}
print("nested", json.dumps(nested))
print("nested indent")
print(json.dumps(nested, indent=1))

# int / str subclasses
class MyInt(int):
    pass


print("int subclass", json.dumps([MyInt(5), True]))

# json.dump to StringIO
buf = io.StringIO()
json.dump({"k": [1, 2]}, buf, indent=2)
print("dump", repr(buf.getvalue()))
buf = io.StringIO()
json.dump("é", buf, ensure_ascii=False)
print("dump unicode", repr(buf.getvalue()))
