# yield from part 2: delegation of throw() and close(), exceptions crossing the delegation
# boundary, and errors for non-iterables / plain iterators without send().


def thrower_inner():
    while True:
        try:
            yield "ready"
        except KeyError as e:
            yield "inner handled " + str(e)


def thrower_outer():
    try:
        yield from thrower_inner()
    except ValueError as e:
        yield "outer handled " + str(e)


t = thrower_outer()
next(t)
print("throw delegated to inner", t.throw(KeyError("k")))
next(t)
print("throw not handled by inner", t.throw(ValueError("v")))
print("outer exhausted after", next(t, "exhausted"))


def closing_inner():
    try:
        yield 1
        yield 2
    finally:
        print("  inner finally on close")


def closing_outer():
    try:
        yield from closing_inner()
    finally:
        print("  outer finally on close")


co = closing_outer()
next(co)
co.close()
print("closed both")


def inner_returns_on_close():
    try:
        yield 1
    except GeneratorExit:
        print("  inner saw GeneratorExit")
        raise


def outer_sees_exit():
    try:
        yield from inner_returns_on_close()
    except GeneratorExit:
        print("  outer saw GeneratorExit")
        raise


oe = outer_sees_exit()
next(oe)
oe.close()
print("GeneratorExit passes through both")


def bad_source():
    yield from 5


try:
    next(bad_source())
except TypeError as e:
    print(type(e).__name__, e)


def send_to_plain_iterator():
    yield from iter([1, 2])


s = send_to_plain_iterator()
next(s)
try:
    s.send("value")
except AttributeError as e:
    print("send to list iterator", type(e).__name__, e)
print("delegating generator finished", next(s, "exhausted"))


def throw_to_plain_iterator():
    yield from iter([1, 2])


tp = throw_to_plain_iterator()
next(tp)
try:
    tp.throw(KeyError("into plain"))
except KeyError as e:
    print("throw into plain iterator raises at yield from", type(e).__name__, e)


def inner_raises():
    yield 1
    raise LookupError("from inner")


def outer_catches():
    try:
        yield from inner_raises()
    except LookupError as e:
        yield "outer caught " + str(e)


print("inner exception propagates to outer", list(outer_catches()))


def echo_inner():
    log = []
    while True:
        value = yield log
        if value == "stop":
            return log
        log.append(value)


def echo_outer():
    result = yield from echo_inner()
    yield "inner returned " + repr(result)


eo = echo_outer()
next(eo)
eo.send("a")
eo.send("b")
print("send delegated", eo.send("stop"))
