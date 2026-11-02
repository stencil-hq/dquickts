# Positional-only and keyword-only parameters, and the TypeError messages for bad calls.


def report(label, func, *args, **kwargs):
    try:
        print(label, repr(func(*args, **kwargs)))
    except TypeError as e:
        print(label, type(e).__name__, e)


def f(x):
    return x


report("missing pos", f)
report("too many", f, 1, 2)
report("unexpected kw", f, 1, y=2)
report("multiple values", f, 1, x=2)
report("by keyword", f, x=3)


def g(a, b):
    return a, b


report("g 3 given", g, 1, 2, 3)
report("g missing 2", g)
report("g missing b", g, 1)
report("g kw both", g, b=1, a=2)


def g_defaults(a, b, c=0):
    return a, b, c


report("from to", g_defaults, 1, 2, 3, 4)
report("missing with default", g_defaults)


def pos_only(a, b, /, c):
    return a, b, c


report("posonly ok", pos_only, 1, 2, 3)
report("posonly c kw", pos_only, 1, 2, c=3)
report("posonly as kw", pos_only, 1, b=2, c=3)
report("posonly missing", pos_only, 1)


def pos_only_kwargs(a, /, **kw):
    return a, kw


report("posonly name in kwargs", pos_only_kwargs, 1, a=2)


def kw_only(a, *, b, c=3):
    return a, b, c


report("kwonly ok", kw_only, 1, b=2)
report("kwonly positional", kw_only, 1, 2)
report("kwonly missing", kw_only, 1)


def kw_only2(*, x, y):
    return x, y


report("kwonly missing 2", kw_only2)
report("kwonly positional 2", kw_only2, 1)


def full(a, b=2, /, c=3, *, d, e=5, **kw):
    return a, b, c, d, e, kw


report("full 1", full, 1, d=4)
report("full 2", full, 1, 2, 3, d=4, e=6, f=7)
report("full too many", full, 1, 2, 3, 4, d=1)


def many(a, b, c, d):
    return a


report("missing 3", many, 1)
report("missing 4", many)


class K:
    def method(self, x):
        return x

    @staticmethod
    def smeth(x):
        return x

    @classmethod
    def cmeth(cls, x):
        return x


report("method missing", K().method)
report("method too many", K().method, 1, 2)
report("unbound missing self", K.method)
report("static missing", K.smeth)
report("classmethod missing", K.cmeth)
report("builtin len args", len)
report("builtin len kw", len, obj=[1])
report("builtin int kw", int, "5", base=10)
report("builtin divmod 1", divmod, 1)
report("lambda missing", lambda p, q: p)
report("lambda kw", lambda p: p, q=1)
report("not callable", 5)
report("none not callable", None)
report("str not callable", "abc")
