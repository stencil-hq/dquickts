"""L0 golden: csv reader/writer, DictReader/DictWriter, dialects and quoting."""
import csv
import io

rows = [
    ["name", "qty", "note"],
    ["apple", 3, "plain"],
    ["banana, ripe", 12, 'say "hi"'],
    ["multi\nline", 0, ""],
    ["none", None, 1.5],
]
out = io.StringIO()
w = csv.writer(out)
for r in rows:
    print("writerow returns", w.writerow(r))
print("written", repr(out.getvalue()))

back = list(csv.reader(io.StringIO(out.getvalue())))
print("read back", back)
print("all strings", all(isinstance(c, str) for r in back for c in r))

for name, q in [("ALL", csv.QUOTE_ALL), ("MINIMAL", csv.QUOTE_MINIMAL), ("NONNUMERIC", csv.QUOTE_NONNUMERIC),
                ("STRINGS", csv.QUOTE_STRINGS), ("NOTNULL", csv.QUOTE_NOTNULL)]:
    o = io.StringIO()
    csv.writer(o, quoting=q, lineterminator="\n").writerow(["a", 1, 2.5, None, ""])
    print("quoting", name, repr(o.getvalue()))
o = io.StringIO()
csv.writer(o, quoting=csv.QUOTE_NONE, escapechar="\\", lineterminator="\n").writerow(["a,b", 'q"'])
print("QUOTE_NONE escape", repr(o.getvalue()))
try:
    csv.writer(io.StringIO(), quoting=csv.QUOTE_NONE).writerow(["a,b"])
except csv.Error as e:
    print("QUOTE_NONE no escape", type(e).__name__, e)

print("nonnumeric read", list(csv.reader(['"a",1,2.5'], quoting=csv.QUOTE_NONNUMERIC)))
print("delimiter", list(csv.reader(["a;b;c", "1;;3"], delimiter=";")))
print("tab dialect", list(csv.reader(["x\ty"], dialect="excel-tab")))
print("skipinitialspace", list(csv.reader(['a, "b, c", d'], skipinitialspace=True)))
print("no skip", list(csv.reader(['a, "b, c"'])))
print("doublequote", list(csv.reader(['"he said ""yo"""'])))
print("empty lines", list(csv.reader(["", "a", ""])))
print("quotechar", list(csv.reader(["'a,b',c"], quotechar="'")))
try:
    list(csv.reader(['a,"b'], strict=True))
except csv.Error as e:
    print("strict", type(e).__name__, e)
try:
    list(csv.reader(['"a"b,c'], strict=True))
except csv.Error as e:
    print("strict bad quote", type(e).__name__, e)
print("lenient", list(csv.reader(['"a"b,c'])))

r = csv.reader(io.StringIO("h1,h2\n1,2\n3,4\n"))
print("line_num", next(r), r.line_num, next(r), r.line_num)

# DictWriter / DictReader
o = io.StringIO()
dw = csv.DictWriter(o, fieldnames=["id", "name", "score"], lineterminator="\n")
dw.writeheader()
dw.writerow({"id": 1, "name": "ann", "score": 9.5})
dw.writerows([{"id": 2, "name": "bob"}, {"id": 3, "name": "c, d", "score": 7}])
print("DictWriter", repr(o.getvalue()))
try:
    dw.writerow({"id": 4, "extra": True})
except ValueError as e:
    print("extra key", type(e).__name__, e)
csv.DictWriter(o, ["a"], extrasaction="ignore", lineterminator="\n").writerow({"a": 1, "b": 2})
print("extrasaction ignore", repr(o.getvalue().splitlines()[-1]))

dr = csv.DictReader(io.StringIO(o.getvalue()))
print("fieldnames", dr.fieldnames)
for rec in dr:
    print("DictReader", rec)
short = csv.DictReader(io.StringIO("a,b,c\n1,2\n1,2,3,4\n"), restval="?", restkey="more")
print("restval/restkey", list(short))
named = csv.DictReader(io.StringIO("1,2\n3,4\n"), fieldnames=["x", "y"])
print("explicit fieldnames", list(named))

csv.register_dialect("pipes", delimiter="|", quoting=csv.QUOTE_ALL, lineterminator="\n")
o = io.StringIO()
csv.writer(o, dialect="pipes").writerow(["p", "q"])
print("custom dialect", repr(o.getvalue()), "pipes" in csv.list_dialects())
d = csv.get_dialect("pipes")
print("dialect attrs", repr(d.delimiter), d.quoting == csv.QUOTE_ALL, repr(d.quotechar))
csv.unregister_dialect("pipes")
try:
    csv.get_dialect("pipes")
except csv.Error as e:
    print("unregistered", type(e).__name__, e)
try:
    csv.writer(io.StringIO(), delimiter="ab")
except TypeError as e:
    print("bad delimiter", type(e).__name__, e)
sniffed = csv.Sniffer().sniff("a;b;c\n1;2;3\n4;5;6\n")
print("sniffer", repr(sniffed.delimiter), csv.Sniffer().has_header("name,age\nann,30\nbob,40\n"))
print("field_size_limit", csv.field_size_limit())
