"""L0 golden: enum Enum/IntEnum/StrEnum/Flag/IntFlag, auto, unique."""
import enum
from enum import Enum, Flag, IntEnum, IntFlag, StrEnum, auto, unique


class Color(Enum):
    RED = 1
    GREEN = 2
    BLUE = 3
    CRIMSON = 1  # alias


print("member", Color.RED, repr(Color.RED))
print("name/value", Color.GREEN.name, Color.GREEN.value)
print("iteration skips alias", list(Color))
print("members incl alias", list(Color.__members__))
print("alias identity", Color.CRIMSON is Color.RED, Color.CRIMSON.name)
print("by value", Color(2), "by name", Color["BLUE"])
print("len/contains", len(Color), Color.RED in Color, 1 in Color, 9 in Color)
print("type", type(Color.RED).__name__, isinstance(Color.RED, Color))
print("eq int", Color.RED == 1, Color.RED is Color(1))
print("hashable", {Color.RED: "r"}[Color.CRIMSON])
try:
    Color(9)
except ValueError as e:
    print("bad value", type(e).__name__, e)
try:
    Color["PINK"]
except KeyError as e:
    print("bad name", type(e).__name__, e)
try:
    Color.RED = 5
except AttributeError as e:
    print("reassign", type(e).__name__, e)
try:
    Color.RED < Color.BLUE
except TypeError as e:
    print("no order", type(e).__name__, e)


class Shape(Enum):
    CIRCLE = auto()
    SQUARE = auto()
    TRIANGLE = auto()

    def describe(self):
        return f"{self.name.lower()}#{self.value}"

    @property
    def sides(self):
        return {"CIRCLE": 0, "SQUARE": 4, "TRIANGLE": 3}[self.name]


print("auto", [s.value for s in Shape])
print("methods", Shape.SQUARE.describe(), Shape.TRIANGLE.sides)


class Priority(IntEnum):
    LOW = 1
    MID = 5
    HIGH = 10


print("IntEnum", Priority.MID, repr(Priority.MID), Priority.MID + 1, Priority.LOW < Priority.HIGH)
print("IntEnum eq int", Priority.HIGH == 10, sorted([Priority.HIGH, 3, Priority.LOW]))
print("IntEnum format", f"{Priority.MID}", f"{Priority.MID:>4}", str(Priority.MID))


class Mode(StrEnum):
    READ = auto()
    WRITE = "w"


print("StrEnum", Mode.READ, repr(Mode.READ), Mode.WRITE == "w", Mode.READ.upper(), f"{Mode.WRITE}")
print("StrEnum value", Mode("read"), Mode.READ.value)


class Perm(Flag):
    R = 4
    W = 2
    X = 1
    RWX = 7


rw = Perm.R | Perm.W
print("Flag", rw, repr(rw), rw.value)
print("Flag contains", Perm.R in rw, Perm.X in rw)
print("Flag iter", list(rw), list(Perm.RWX))
print("Flag ops", Perm.RWX & ~Perm.W, Perm.RWX ^ Perm.R, ~Perm.R)
print("Flag empty", Perm(0), bool(Perm(0)), repr(Perm(0)))
print("Flag members", list(Perm), len(Perm))
print("Flag from value", Perm(6), Perm(6) == rw)
try:
    Perm(8)
except ValueError as e:
    print("Flag bad", type(e).__name__, e)


class Bits(IntFlag):
    A = auto()
    B = auto()
    C = auto()


print("IntFlag", [b.value for b in Bits], Bits.A | Bits.C, int(Bits.A | Bits.C), Bits.B | 8)
print("IntFlag repr", repr(Bits.A | Bits.B))

try:
    @unique
    class Dup(Enum):
        A = 1
        B = 1
except ValueError as e:
    print("unique", type(e).__name__, e)

Animal = Enum("Animal", "ANT BEE CAT")
print("functional", list(Animal), Animal.BEE.value)
Http = IntEnum("Http", [("OK", 200), ("NOT_FOUND", 404)])
print("functional pairs", Http(404), Http.OK.value)


class Lenient(Enum):
    ONE = 1

    @classmethod
    def _missing_(cls, value):
        if value == "one":
            return cls.ONE
        return None


print("_missing_", Lenient("one"))
try:
    Lenient("two")
except ValueError as e:
    print("_missing_ none", type(e).__name__, e)
try:
    class Extend(Color):
        PINK = 4
except TypeError as e:
    print("extend", type(e).__name__, e)
print("member type", type(Color.__members__).__name__, enum.EnumType.__name__)
print("verify", enum.verify(enum.CONTINUOUS)(Enum("C", {"A": 1, "B": 2})).B)
