"""L0 golden: dataclasses kw_only, slots, nested asdict/astuple, hashing, make_dataclass, inheritance."""
import dataclasses
from dataclasses import KW_ONLY, asdict, astuple, dataclass, field, fields


@dataclass
class Point:
    x: int
    y: int = 0


p = Point(1, 2)


@dataclass(kw_only=True)
class Opts:
    verbose: bool = False
    level: int


print("kw_only", Opts(level=3))
try:
    Opts(True, 3)
except TypeError as e:
    print("kw_only positional", type(e).__name__, e)


@dataclass
class Mixed:
    a: int
    _: KW_ONLY
    b: int = 1
    c: int


print("KW_ONLY sentinel", Mixed(0, c=5), [f.kw_only for f in fields(Mixed)])


@dataclass
class PerField:
    a: int
    b: int = field(default=2, kw_only=True)


print("field kw_only", PerField(1, b=3))


@dataclass(slots=True)
class Slotted:
    x: int
    y: int = 2


s = Slotted(1)
print("slots", s, Slotted.__slots__, hasattr(s, "__dict__"))
try:
    s.z = 3
except AttributeError as e:
    print("slots no new attr", type(e).__name__)


@dataclass
class Nested:
    p: Point
    ps: list
    m: dict


n = Nested(Point(1, 2), [Point(3, 4)], {"k": Point(5, 6)})
print("asdict nested", asdict(n))
print("astuple nested", astuple(n))
print("asdict copies", asdict(n)["ps"] is not n.ps)
print("asdict factory", asdict(p, dict_factory=lambda kv: sorted(kv, reverse=True)))
try:
    asdict(Point)
except TypeError as e:
    print("asdict class", type(e).__name__, e)


@dataclass(eq=False)
class Identity:
    v: int


i1 = Identity(1)
print("eq=False", i1 == Identity(1), i1 == i1)


@dataclass(unsafe_hash=True)
class Hashed:
    v: int


print("unsafe_hash", hash(Hashed(3)) == hash((3,)), Hashed(3) in {Hashed(3)})
Dyn = dataclasses.make_dataclass("Dyn", [("a", int), ("b", str, field(default="z"))])
print("make_dataclass", Dyn(1), Dyn.__name__)


@dataclass
class Child(Point):
    z: int = 0


print("inheritance", Child(1, 2, 3), [f.name for f in fields(Child)])
print("MISSING", fields(Point)[0].default is dataclasses.MISSING, fields(Point)[1].default)
try:
    @dataclass
    class BadOrder:
        a: int = 1
        b: int
except TypeError as e:
    print("default before non-default", type(e).__name__, e)
