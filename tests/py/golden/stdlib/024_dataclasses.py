"""L0 golden: dataclasses basics: repr/eq/order, default_factory, frozen, replace, __post_init__."""
import dataclasses
from dataclasses import FrozenInstanceError, InitVar, asdict, astuple, dataclass, field, fields, replace


@dataclass
class Point:
    x: int
    y: int = 0


p = Point(1, 2)
print("repr", p, Point(5))
print("eq", p == Point(1, 2), p == Point(2, 1), p == (1, 2))
try:
    p < Point(3, 4)
except TypeError as e:
    print("no order", type(e).__name__, e)
print("hash none", Point.__hash__ is None)
print("fields", [(f.name, f.type.__name__, None if f.default is dataclasses.MISSING else f.default) for f in fields(p)])
print("asdict", asdict(p), "astuple", astuple(p))
print("replace", replace(p, y=9), p)
print("is_dataclass", dataclasses.is_dataclass(p), dataclasses.is_dataclass(Point), dataclasses.is_dataclass(1))
try:
    Point()
except TypeError as e:
    print("missing arg", type(e).__name__, e)
try:
    replace(p, z=1)
except TypeError as e:
    print("replace unknown", type(e).__name__, e)


@dataclass(order=True)
class Version:
    major: int
    minor: int = 0
    label: str = field(default="", compare=False)


print("order", Version(1, 2) < Version(1, 10), Version(2) > Version(1, 99), Version(1, label="a") == Version(1, label="b"))
print("sorted", sorted([Version(1, 5), Version(0, 9), Version(1, 0)]))


@dataclass
class Inventory:
    items: list = field(default_factory=list)
    counts: dict = field(default_factory=dict, repr=False)
    secret: str = field(default="hidden", repr=False)


a, b = Inventory(), Inventory()
a.items.append("apple")
print("default_factory", a, b, a.items is not b.items)
try:
    @dataclass
    class Bad:
        items: list = []
except ValueError as e:
    print("mutable default", type(e).__name__, e)


@dataclass(frozen=True)
class Frozen:
    name: str
    tags: tuple = ()


fz = Frozen("f", ("t",))
try:
    fz.name = "g"
except FrozenInstanceError as e:
    print("frozen", type(e).__name__, e)
try:
    del fz.name
except FrozenInstanceError as e:
    print("frozen del", type(e).__name__, e)
print("frozen hashable", hash(Frozen("x", (1,))) == hash(Frozen("x", (1,))))
print("frozen replace", replace(fz, name="g"), fz)
print("FrozenInstanceError base", FrozenInstanceError.__mro__[1].__name__)


@dataclass
class Rect:
    w: float
    h: float
    area: float = field(init=False)
    scale: InitVar[float] = 1.0

    def __post_init__(self, scale):
        self.w *= scale
        self.h *= scale
        self.area = self.w * self.h


print("post_init", Rect(2, 3), Rect(2, 3, scale=2))
print("InitVar not field", [f.name for f in fields(Rect)])
