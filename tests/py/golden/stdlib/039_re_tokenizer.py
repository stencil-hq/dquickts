"""L0 golden: realistic re workloads: tokenizer via lastgroup, log parsing, templating."""
import re
from typing import NamedTuple


class Token(NamedTuple):
    kind: str
    value: str
    line: int
    column: int


TOKEN_SPEC = [
    ("NUMBER", r"\d+(?:\.\d*)?"),
    ("ASSIGN", r":="),
    ("END", r";"),
    ("ID", r"[A-Za-z_]\w*"),
    ("OP", r"[+\-*/()]"),
    ("NEWLINE", r"\n"),
    ("SKIP", r"[ \t]+"),
    ("MISMATCH", r"."),
]
TOKEN_RE = re.compile("|".join(f"(?P<{name}>{pat})" for name, pat in TOKEN_SPEC))
KEYWORDS = {"IF", "THEN", "ENDIF", "FOR", "NEXT", "GOSUB", "RETURN"}


def tokenize(code):
    """Yield Token tuples for *code*, raising RuntimeError on bad characters."""
    line_num = 1
    line_start = 0
    for mo in TOKEN_RE.finditer(code):
        kind = mo.lastgroup
        value = mo.group()
        column = mo.start() - line_start
        if kind == "NUMBER":
            value = float(value) if "." in value else int(value)
        elif kind == "ID" and value in KEYWORDS:
            kind = value
        elif kind == "NEWLINE":
            line_start = mo.end()
            line_num += 1
            continue
        elif kind == "SKIP":
            continue
        elif kind == "MISMATCH":
            raise RuntimeError(f"{value!r} unexpected on line {line_num}")
        yield Token(kind, value, line_num, column)


program = """IF quantity THEN
    total := total + price * quantity;
    tax := price * 0.05;
ENDIF;"""
for tok in tokenize(program):
    print("token", tok)
try:
    list(tokenize("x := 1 $ 2"))
except RuntimeError as e:
    print("tokenize error", e)
print("pattern groups", TOKEN_RE.groups, list(TOKEN_RE.groupindex)[:3])

LOG_RE = re.compile(
    r"^(?P<ip>\d{1,3}(?:\.\d{1,3}){3}) - - \[(?P<ts>[^\]]+)\] "
    r'"(?P<method>[A-Z]+) (?P<path>\S+) HTTP/(?P<ver>[\d.]+)" (?P<status>\d{3}) (?P<size>\d+|-)$'
)
logs = """\
127.0.0.1 - - [10/Oct/2000:13:55:36 -0700] "GET /apache_pb.gif HTTP/1.0" 200 2326
10.0.0.2 - - [10/Oct/2000:13:56:01 -0700] "POST /api/v1/items?id=3 HTTP/1.1" 201 -
garbage line here
192.168.1.10 - - [11/Oct/2000:00:00:00 +0000] "DELETE /x HTTP/2" 404 0"""
status_counts = {}
for line in logs.splitlines():
    m = LOG_RE.match(line)
    if not m:
        print("log skip", repr(line))
        continue
    info = m.groupdict()
    status_counts[info["status"]] = status_counts.get(info["status"], 0) + 1
    print("log", info["method"], info["path"], info["status"], info["size"], m.span("path"))
print("status counts", sorted(status_counts.items()))

template = "Hello {{ name }}, you owe ${{amount}} by {{ due|upper }}. {{ missing }}"
ctx = {"name": "Ann", "amount": "12.50", "due": "friday"}


def render(m):
    key, _, filt = m.group(1).partition("|")
    value = ctx.get(key, f"<{key}?>")
    return value.upper() if filt == "upper" else value


print("render", re.sub(r"\{\{\s*([\w|]+)\s*\}\}", render, template))

camel = ["camelCaseString", "HTTPServerError", "getHTTPResponseCode", "simple", "already_snake"]
for name in camel:
    snake = re.sub(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", "_", name).lower()
    print("snake", name, "->", snake)

emails = "contact: a.b@example.com, bad@, x_y+z@sub.domain.org; @nope.com"
print("emails", re.findall(r"[\w.+-]+@[\w-]+(?:\.[\w-]+)+", emails))
print("ints and floats", [float(x) if "." in x else int(x) for x in re.findall(r"-?\d+(?:\.\d+)?", "a -3 b 4.25 c 10.")])
kv = "a=1; b = two ;c=  3 ;; d="
print("key values", dict(re.findall(r"\s*(\w+)\s*=\s*([^;]*?)\s*(?:;|$)", kv)))
print("collapse ws", re.sub(r"\s+", " ", "  many \n\t spaces  ").strip())
print("strip tags", re.sub(r"<[^>]+>", "", "<p>Hello <b>world</b></p>"))
print("dedupe words", re.sub(r"\b(\w+)(\s+\1\b)+", r"\1", "the the cat sat sat sat down", flags=re.I))
date_re = re.compile(r"(?P<y>\d{4})-(?P<m>\d\d)-(?P<d>\d\d)")
print("reformat dates", date_re.sub(r"\g<d>/\g<m>/\g<y>", "from 2024-01-05 to 2024-12-31"))
print("split sentences", re.split(r"(?<=[.!?])\s+", "One. Two! Three? Four"))
