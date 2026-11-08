"""L0 golden: pprint formatting, keyword lists and unicodedata lookups."""
import keyword
import pprint
import re
import unicodedata

data = {
    "zeta": [1, 2, 3],
    "alpha": {"nested": {"deep": list(range(12)), "flag": True}, "name": "x" * 30},
    "mid": ("tuple", "of", "strings", None),
    "beta": {3, 1, 2},
    "long": "a fairly long string value that will need wrapping when the width is small enough",
}
print("pformat default:")
print(pprint.pformat(data))
print("pformat width=40:")
print(pprint.pformat(data, width=40))
print("pformat unsorted:")
print(pprint.pformat(data, sort_dicts=False, width=60))
print("pformat depth=1:", pprint.pformat(data, depth=1))
print("pformat compact:")
print(pprint.pformat(list(range(40)), width=30, compact=True))
print("pformat indent:")
print(pprint.pformat({"a": [1, {"b": 2}], "c": "d" * 50}, indent=4, width=30))
print("underscore_numbers", pprint.pformat(12345678, underscore_numbers=True))
rec = [1]
rec.append(rec)
print("recursive", re.sub(r"id=\d+", "id=N", pprint.pformat(rec)), pprint.isrecursive(rec), pprint.isreadable(rec))
print("saferepr", pprint.saferepr({"b": 1, "a": [2]}))
print("isreadable", pprint.isreadable([1, "a"]), pprint.isreadable(pprint))
pp = pprint.PrettyPrinter(width=20)
print("PrettyPrinter:")
pp.pprint({"key1": "value1", "key2": "value2"})
print("pp function:")
pprint.pp({"b": 1, "a": 2})
print("bytes wrap:")
print(pprint.pformat(b"x" * 60, width=30))

# keyword
print("kwlist", keyword.kwlist)
print("softkwlist", keyword.softkwlist)
print("iskeyword", keyword.iskeyword("if"), keyword.iskeyword("match"), keyword.iskeyword("print"), keyword.iskeyword("None"))
print("issoftkeyword", keyword.issoftkeyword("match"), keyword.issoftkeyword("_"), keyword.issoftkeyword("if"))

# unicodedata
for ch in ["A", "é", "€", "١", "\u00a0", "\U0001f600", "ß", "\u0301", "Ω"]:
    print(
        "char", ascii(ch),
        unicodedata.name(ch, "?"),
        unicodedata.category(ch),
        unicodedata.bidirectional(ch),
        unicodedata.combining(ch),
        unicodedata.east_asian_width(ch),
        unicodedata.mirrored(ch),
    )
print("name default", unicodedata.name("\x00", "no name"))
try:
    unicodedata.name("\x00")
except ValueError as e:
    print("name missing", type(e).__name__, e)
print("lookup", unicodedata.lookup("GREEK SMALL LETTER ALPHA"), unicodedata.lookup("SNOWMAN"))
try:
    unicodedata.lookup("NOT A REAL NAME")
except KeyError as e:
    print("lookup missing", type(e).__name__, e)
print("numeric", unicodedata.decimal("٣"), unicodedata.digit("²"), unicodedata.numeric("½"), unicodedata.numeric("x", -1))
try:
    unicodedata.decimal("x")
except ValueError as e:
    print("decimal missing", type(e).__name__, e)
composed = "é"
decomposed = "e\u0301"
for form in ["NFC", "NFD", "NFKC", "NFKD"]:
    print("normalize", form, ascii(unicodedata.normalize(form, decomposed)), ascii(unicodedata.normalize(form, "ﬁ²")))
print("is_normalized", unicodedata.is_normalized("NFC", composed), unicodedata.is_normalized("NFC", decomposed))
print("equal after NFC", composed == decomposed, unicodedata.normalize("NFC", decomposed) == composed)
print("decomposition", unicodedata.decomposition("é"), repr(unicodedata.decomposition("a")), unicodedata.decomposition("ﬁ"))
try:
    unicodedata.normalize("NFX", "a")
except ValueError as e:
    print("bad form", type(e).__name__, e)
print("casefold", "Straße".casefold(), "ΣΑΣ".lower(), "ǆ".title(), "ﬁ".upper())
