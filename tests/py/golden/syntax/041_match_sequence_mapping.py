# match statement: sequence patterns (fixed, starred, nested) and mapping patterns.

def seq(x):
    match x:
        case []:
            return "empty"
        case [a]:
            return "one " + repr(a)
        case [a, b]:
            return "two " + repr((a, b))
        case [a, b, c]:
            return "three " + repr((a, b, c))
        case [first, *rest]:
            return "many first=" + repr(first) + " rest=" + repr(rest)
    return "not a sequence"

for v in [[], (), [1], (1,), [1, 2], (1, 2, 3), list(range(6)), "ab", b"ab", {1, 2},
          {"a": 1}, range(2), 5, None]:
    print("seq", repr(v), seq(v))

def star(x):
    match x:
        case [*_, "end"]:
            return "ends with end"
        case ["start", *middle, last]:
            return "start middle=" + repr(middle) + " last=" + repr(last)
        case [*init, 0]:
            return "init before 0 " + repr(init)
        case [_, _, *_]:
            return "at least two"
    return "other"

for v in [["end"], ["x", "end"], ["start", "end"], ["start", 1, 2, 3], ["start", 9],
          [5, 4, 0], [0], [1, 2], [1]]:
    print("star", repr(v), star(v))

# starred binds a list, even when subject is a tuple
match (1, 2, 3):
    case (a, *b):
        print("starred type", type(b).__name__, b)

# nested sequence patterns
def nested(x):
    match x:
        case [(a, b), [c, *d]]:
            return "nested " + repr((a, b, c, d))
        case [[[deep]]]:
            return "deep " + repr(deep)
        case [1, [2, [3, _]]]:
            return "literal nest"
    return "no"

print("nested", nested([(1, 2), [3, 4, 5]]), nested([[[7]]]), nested([1, [2, [3, 4]]]), nested([1]))

# sequence patterns with literals and alternatives
def cmd(c):
    match c.split():
        case ["go", ("north" | "south" | "east" | "west") as direction]:
            return "go " + direction
        case ["pick", "up", item] | ["take", item]:
            return "take " + item
        case ["drop", *items] if items:
            return "drop " + ",".join(items)
        case ["quit" | "exit"]:
            return "bye"
        case _:
            return "unknown: " + c

for c in ["go north", "go up", "pick up lamp", "take key", "drop a b c", "drop", "exit", ""]:
    print("cmd", repr(c), cmd(c))

# mapping patterns
def mapping(x):
    match x:
        case {"type": "point", "x": px, "y": py}:
            return "point " + repr((px, py))
        case {"type": "circle", "r": r, **rest}:
            return "circle r=" + repr(r) + " rest=" + repr(rest)
        case {"type": t}:
            return "typed " + repr(t)
        case {}:
            return "any mapping"
    return "not a mapping"

for v in [{"type": "point", "x": 1, "y": 2, "z": 3}, {"type": "circle", "r": 5, "color": "red"},
          {"type": "circle", "r": 1}, {"type": "square"}, {"x": 1}, {}, [], "type"]:
    print("mapping", repr(v), mapping(v))

# mapping keys can be literals or value patterns
class K:
    NAME = "name"

match {1: "one", "name": "N", None: "nil"}:
    case {1: v1, K.NAME: v2, None: v3}:
        print("literal/value keys", v1, v2, v3)

# mapping patterns use get-like lookup and do not call __missing__ via []
class D(dict):
    def __missing__(self, key):
        return "missing!"

match D(a=1):
    case {"b": _}:
        print("never: __missing__ not used")
    case {"a": a}:
        print("dict subclass", a)

# duplicate keys / invalid patterns are syntax errors
for src in ["match x:\n case {'a': 1, 'a': 2}: pass",
            "match x:\n case [*a, *b]: pass",
            "match x:\n case a | b: pass",
            "match x:\n case {**_}: pass",
            "match x:\n case x:\n  pass\n case 1: pass",
            "match x:\n case [a, a]: pass",
            "match x:\n case 1 + 1: pass"]:
    try:
        compile(src, "<m>", "exec")
        print("compiles", repr(src))
    except SyntaxError as e:
        print("syntax", repr(src), e.msg)
