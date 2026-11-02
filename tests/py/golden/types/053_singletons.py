"""L0 golden: None, NotImplemented, Ellipsis and bool singletons.

Covers reprs, identity of literals and constructor calls, truthiness,
comparisons (None ordering errors), NotImplemented in boolean context, and
binary-operator fallback when __eq__/__add__ return NotImplemented.

Usage: python3.15 -I -X utf8 tests/py/golden/types/053_singletons.py
"""


class Left:
    """__add__ and __eq__ always return NotImplemented."""

    def __add__(self, other):
        return NotImplemented

    def __eq__(self, other):
        return NotImplemented

    __hash__ = None


class Right:
    """Provides __radd__ so Left() + Right() succeeds via reflection."""

    def __radd__(self, other):
        return "Right.__radd__"


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("None", lambda: None)
show("str(None)", lambda: str(None))
show("NotImplemented", lambda: NotImplemented)
show("Ellipsis", lambda: Ellipsis)
show("...", lambda: ...)
show("... is Ellipsis", lambda: ... is Ellipsis)
show("True/False", lambda: (True, False))
show("type(None)()", lambda: type(None)() is None)
show("type(...)()", lambda: type(...)() is Ellipsis)
show("type(NotImplemented)()", lambda: type(NotImplemented)() is NotImplemented)
show("bool() is False", lambda: bool() is False)
show("bool(1) is True", lambda: bool(1) is True)
show("type(None)(1)", lambda: type(None)(1))
show("type names", lambda: [type(x).__name__ for x in (None, ..., NotImplemented, True)])
show("bool(None)", lambda: bool(None))
show("bool(...)", lambda: bool(...))
show("bool(NotImplemented)", lambda: bool(NotImplemented))
show("None == None", lambda: None == None)
show("None == 0", lambda: None == 0)
show("None == False", lambda: None == False)
show("None == ''", lambda: None == "")
show("None < 1", lambda: None < 1)
show("None < None", lambda: None < None)
show("None + 1", lambda: None + 1)
nothing = None
show("None()", lambda: nothing())
show("None.x", lambda: nothing.x)
show("None[0]", lambda: nothing[0])
show("len(None)", lambda: len(None))
show("iter(None)", lambda: iter(None))
show("Ellipsis == Ellipsis", lambda: Ellipsis == ...)
show("... < ...", lambda: ... < ...)
show("hash consistency", lambda: hash(None) == hash(None))
show("None in [None]", lambda: None in [None])
show("{None: 1}", lambda: {None: 1, ...: 2})
show("None.__bool__()", lambda: None.__bool__())
show("None.__repr__()", lambda: None.__repr__())
show("Ellipsis.__reduce__()", lambda: Ellipsis.__reduce__())
show("NotImplemented.__reduce__()", lambda: NotImplemented.__reduce__())
show("int.__add__(1, 'a')", lambda: int.__add__(1, "a"))
show("float.__eq__(1.0, 'a')", lambda: float.__eq__(1.0, "a"))
show("str.__lt__('a', 1)", lambda: "a".__lt__(1))
show("Left() + 1", lambda: Left() + 1)
show("Left() + Right()", lambda: Left() + Right())
show("Left() == Left()", lambda: Left() == Left())
left = Left()
show("left == left identity fallback", lambda: left == left)
show("left != left", lambda: left != left)
show("Left() == 1", lambda: Left() == 1)
show("hash(Left())", lambda: hash(Left()))
show("f-strings", lambda: f"{None} {...} {True} {NotImplemented}")
show("format(None, '')", lambda: format(None, ""))
show("format(None, '>6')", lambda: format(None, ">6"))
show("format(True, '>6')", lambda: format(True, ">6"))
show("format(True, '^6s')", lambda: format(True, "^6s"))
show("'%s %r' % (None, ...)", lambda: "%s %r" % (None, ...))
show("isinstance(None, object)", lambda: isinstance(None, object))
show("x is None idiom", lambda: [x is None for x in (None, 0, "", False)])
show("or chain", lambda: None or 0 or "" or "last")
show("and chain", lambda: 1 and "x" and None and "never")
show("not None", lambda: not None)
show("True + False", lambda: True + False)
show("True is not False", lambda: True is not False)
show("max(None)", lambda: max(None))
show("sorted([None, None])", lambda: sorted([None, None]))
show("sorted([None, 1])", lambda: sorted([None, 1]))
