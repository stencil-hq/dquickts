# Function introspection: __name__, __qualname__, __doc__, __module__, custom attributes.


def top():
    """Top-level docstring."""

    def inner():
        def innermost():
            pass

        return innermost

    return inner


print("name", top.__name__)
print("qualname", top.__qualname__)
print("doc", repr(top.__doc__))
print("module", top.__module__)
inner = top()
print("nested qualname", inner.__qualname__)
print("nested name", inner.__name__)
print("deep qualname", inner().__qualname__)
print("no doc", repr(inner.__doc__))

lam = lambda x: x
print("lambda name", lam.__name__)
print("lambda qualname", lam.__qualname__)


def make_lambda():
    return lambda: 0


print("nested lambda qualname", make_lambda().__qualname__)


class Outer:
    """Outer doc."""

    def method(self):
        """Method doc."""

        def helper():
            pass

        return helper

    class Inner:
        def m(self):
            pass

    @staticmethod
    def s():
        pass

    @classmethod
    def c(cls):
        pass

    lam = lambda self: 1


print("method qualname", Outer.method.__qualname__)
print("method doc", Outer.method.__doc__)
print("helper in method", Outer().method().__qualname__)
print("inner class qualname", Outer.Inner.__qualname__)
print("inner method qualname", Outer.Inner.m.__qualname__)
print("static qualname", Outer.s.__qualname__)
print("classmethod qualname", Outer.c.__qualname__)
print("class lambda qualname", Outer.lam.__qualname__)
print("class doc", Outer.__doc__)
print("class name", Outer.__name__, Outer.Inner.__name__)
print("bound method name", Outer().method.__name__)
print("bound __func__ is", Outer().method.__func__ is Outer.method)


def make_class():
    class Local:
        def m(self):
            pass

    return Local


Local = make_class()
print("local class qualname", Local.__qualname__)
print("local method qualname", Local.m.__qualname__)

top.__name__ = "renamed"
top.__qualname__ = "Some.renamed"
print("renamed", top.__name__, top.__qualname__)
try:
    top.__name__ = 5
except TypeError as e:
    print(type(e).__name__, e)
top.__doc__ = "new doc"
print("new doc", top.__doc__)


def attrs():
    pass


attrs.counter = 0
attrs.counter += 1
attrs.tags = ["a"]
print("func attrs", attrs.counter, attrs.tags)
print("func dict", attrs.__dict__)
print("hasattr", hasattr(attrs, "counter"), hasattr(attrs, "missing"))
del attrs.counter
print("after del", attrs.__dict__)
try:
    attrs.missing
except AttributeError as e:
    print(type(e).__name__, e)


def annotated(a: int, b: "str" = "x") -> bool:
    return True


print("annotations", annotated.__annotations__)
print("code name", annotated.__code__.co_name)
print("argcount", annotated.__code__.co_argcount)
print("varnames", annotated.__code__.co_varnames)


def kinds(a, b, /, c, *args, d, **kw):
    x = 1
    return x


code = kinds.__code__
print("posonly count", code.co_posonlyargcount)
print("kwonly count", code.co_kwonlyargcount)
print("all varnames", code.co_varnames)
print("callable", callable(kinds), callable(Outer), callable(Outer()), callable(1))
print("type name", type(kinds).__name__, type(len).__name__, type(Outer().method).__name__)
