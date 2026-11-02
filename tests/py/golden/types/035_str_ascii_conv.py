"""L0 golden: ascii(), str() and repr() of builtin objects and user classes.

str() falls back to repr() unless __str__ is defined; ascii() escapes every
non-ASCII character in the repr; containers always use repr of items.

Usage: python3.15 -I -X utf8 tests/py/golden/types/035_str_ascii_conv.py
"""


class OnlyRepr:
    """Defines __repr__ only, so str() uses it too."""

    def __repr__(self) -> str:
        return "OnlyRepr(é)"


class Both:
    """Defines distinct __str__ and __repr__."""

    def __str__(self) -> str:
        return "both-str"

    def __repr__(self) -> str:
        return "Both()"


class BadStr:
    """__str__ returning a non-str."""

    def __str__(self):
        return 42


class StrSub(str):
    """Trivial str subclass."""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


OBJECTS = [None, True, False, 0, -5, 2 ** 70, 0.5, -0.0, 1e16, float("inf"), float("nan"), 1j, -1.5 + 2j,
           "", "é", "😀", "a\nb", b"", b"\xff", bytearray(b"x"), [], [1, "a"], (), (1,), (1, 2),
           {}, {"k": [1]}, set(), frozenset(), {1}, frozenset({2}), range(3), range(1, 10, 2),
           slice(1, 2), slice(None), Ellipsis, NotImplemented, OnlyRepr(), Both(), [Both()],
           int, str, type, object, len, StrSub("s")]

for obj in OBJECTS:
    print("str", str(obj), "| repr", repr(obj), "| ascii", ascii(obj))

show("str(BadStr())", lambda: str(BadStr()))
show("ascii('é😀\\x00')", lambda: ascii("é😀\x00"))
show("ascii(['ü'])", lambda: ascii(["ü"]))
show("ascii(OnlyRepr())", lambda: ascii(OnlyRepr()))
show("ascii type", lambda: type(ascii(1)).__name__)
show("str(StrSub) type", lambda: type(str(StrSub("s"))).__name__)
show("format(Both())", lambda: format(Both()))
show("f'{Both()}'", lambda: f"{Both()}")
show("f'{Both()!r}'", lambda: f"{Both()!r}")
show("f'{Both()!s}'", lambda: f"{Both()!s}")
show("'%s %r %a' % (...)", lambda: "%s %r %a" % (Both(), Both(), "é"))
show("str(b'x', 'ascii')", lambda: str(b"x", "ascii"))
show("str(1, 'ascii')", lambda: str(1, "ascii"))
show("str(b'x', errors='strict')", lambda: str(b"x", errors="strict"))
show("str.__str__('x')", lambda: str.__str__("x"))
show("object.__str__(Both())", lambda: object.__str__(Both()))
show("repr(str)", lambda: repr(str))
show("repr(len)", lambda: repr(len))
show("repr(int.bit_length)", lambda: repr(int.bit_length))
show("repr(str.join)", lambda: repr(str.join))
show("repr(None.__class__)", lambda: repr(None.__class__))
