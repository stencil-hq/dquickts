"""L0 golden: json.loads / json.load parsing, hooks and JSONDecodeError."""
import collections
import decimal
import io
import json

print("int", repr(json.loads("42")))
print("float 1.0", repr(json.loads("1.0")))
print("float exp", repr(json.loads("1e3")), repr(json.loads("-2.5E-3")))
print("neg zero", repr(json.loads("-0")), repr(json.loads("-0.0")))
print("big int", repr(json.loads("123456789012345678901234567890")))
print("big float", repr(json.loads("1e400")), repr(json.loads("-1e400")))
print("nan inf", json.loads("[NaN, Infinity, -Infinity]"))
print("literals", json.loads("[true, false, null]"))
print("string", repr(json.loads('"a\\nb\\t\\"q\\" \\u00e9 \\ud83d\\ude00"')))
print("lone surrogate", repr(json.loads('"\\ud800"')))
print("raw unicode", repr(json.loads('"日本"')))
print("whitespace", json.loads(' \n\t{ "a" : [ 1 , 2 ] }\r\n'))
print("dup keys", json.loads('{"a": 1, "a": 2}'))
print("nested", json.loads('{"x": {"y": [1, {"z": []}]}}'))
print("types", [type(v).__name__ for v in json.loads('[1, 1.5, "s", null, true, [], {}]')])

# hooks
print("pairs hook", json.loads('{"b": 1, "a": 2, "b": 3}', object_pairs_hook=list))
od = json.loads('{"z": 1, "y": 2}', object_pairs_hook=collections.OrderedDict)
print("ordered", type(od).__name__, list(od.items()))
print("object hook", json.loads('{"x": 1, "y": {"x": 2}}', object_hook=lambda d: sorted(d.items())))
print("parse_float", json.loads("[1.10, 2]", parse_float=decimal.Decimal))
print("parse_int", json.loads("[1, 2.0]", parse_int=lambda s: s + "!"))
print("parse_constant", json.loads("[NaN, Infinity]", parse_constant=lambda s: s.lower()))

# bytes input
print("bytes utf8", json.loads(b'{"k": "\xc3\xa9"}'))
print("bytes utf16", json.loads('["x"]'.encode("utf-16")))

# errors
bad_inputs = [
    "",
    "[1, 2",
    '{"a" 1}',
    "{'a': 1}",
    "[1,]",
    '{"a": 1,}',
    "tru",
    '"unterminated',
    '"bad \\x escape"',
    "[1] extra",
    '{\n  "a": 1,\n  "b": ?\n}',
    "01",
    '"ctl \x01 char"',
    "nan",
]
for text in bad_inputs:
    try:
        json.loads(text)
    except json.JSONDecodeError as e:
        print("err", repr(text))
        print("   ", e.msg, "| pos", e.pos, "| line", e.lineno, "| col", e.colno)
        print("    str:", str(e))

print("is ValueError", issubclass(json.JSONDecodeError, ValueError))
print("strict false", repr(json.loads('"tab\there"', strict=False)))
try:
    json.loads(123)
except TypeError as e:
    print("non-str", type(e).__name__, e)

# raw_decode
dec = json.JSONDecoder()
print("raw_decode", dec.raw_decode('{"a": 1} trailing'))
print("raw_decode idx", dec.raw_decode('xx[1, 2]yy', 2))

# load from StringIO
src = io.StringIO('{"items": [1, 2, 3], "ok": true}')
obj = json.load(src)
print("load", obj, sum(obj["items"]))

# round trip
original = {"s": "ü\n", "l": [1, 2.5, None, True], "d": {"e": {}}, "n": -12}
again = json.loads(json.dumps(original))
print("roundtrip eq", again == original)
print("roundtrip float", json.loads(json.dumps(0.1 + 0.2)) == 0.1 + 0.2)
print("tuple becomes list", json.loads(json.dumps((1, 2))))
print("int keys become str", json.loads(json.dumps({1: "a"})))
