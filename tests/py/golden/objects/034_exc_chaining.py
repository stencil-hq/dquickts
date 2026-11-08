# Exception chaining: explicit `raise ... from`, implicit __context__, `from None`,
# __suppress_context__, and chains through multiple levels.


def describe(e):
    return (
        type(e).__name__
        + " cause="
        + (type(e.__cause__).__name__ if e.__cause__ else "None")
        + " context="
        + (type(e.__context__).__name__ if e.__context__ else "None")
        + " suppress="
        + str(e.__suppress_context__)
    )


try:
    try:
        {}["k"]
    except KeyError as exc:
        raise ValueError("explicit") from exc
except ValueError as e:
    print("from exc", describe(e))
    print("cause is context", e.__cause__ is e.__context__)

try:
    try:
        1 / 0
    except ZeroDivisionError:
        raise RuntimeError("implicit")
except RuntimeError as e:
    print("implicit", describe(e))
    print("context message", e.__context__)

try:
    try:
        [][1]
    except IndexError:
        raise LookupError("hidden") from None
except LookupError as e:
    print("from None", describe(e))

try:
    raise TypeError("t") from ValueError("v")
except TypeError as e:
    print("from new instance", describe(e), repr(e.__cause__))

try:
    raise TypeError("t") from ValueError
except TypeError as e:
    print("from class", describe(e), repr(e.__cause__))

try:
    raise TypeError("t") from 5
except TypeError as e:
    print("from invalid", type(e).__name__, e)


def three_levels():
    try:
        try:
            try:
                raise KeyError("a")
            except KeyError as e1:
                raise ValueError("b") from e1
        except ValueError:
            raise RuntimeError("c")
    except RuntimeError as e:
        return e


e = three_levels()
links = []
cur = e
while cur is not None:
    links.append(type(cur).__name__ + ":" + str(cur))
    cur = cur.__cause__ or cur.__context__
print("chain walk", links)

err = ValueError("manual")
print("fresh exception", err.__cause__, err.__context__, err.__suppress_context__)
err.__cause__ = KeyError("set")
print("setting cause sets suppress", err.__suppress_context__)
err.__suppress_context__ = False
print("suppress writable", err.__suppress_context__)
try:
    err.__cause__ = "bad"
except TypeError as ex:
    print(type(ex).__name__, ex)
try:
    err.__context__ = 1
except TypeError as ex:
    print(type(ex).__name__, ex)


def reraise_same():
    try:
        raise ValueError("same")
    except ValueError as e:
        raise e


try:
    reraise_same()
except ValueError as e:
    print("re-raise same object no self context", e.__context__)

try:
    try:
        raise KeyError("ctx")
    except KeyError:
        try:
            raise ValueError("inner handled")
        except ValueError:
            pass
        raise TypeError("after inner")
except TypeError as e:
    print("context after nested handled", repr(e.__context__))


def finally_context():
    try:
        raise ValueError("in try")
    finally:
        raise_in_finally()


def raise_in_finally():
    raise KeyError("in finally")


try:
    finally_context()
except KeyError as e:
    print("finally context", repr(e.__context__))

try:
    try:
        raise ValueError("a")
    except ValueError as a:
        raise a from a
except ValueError as e:
    print("self cause", e.__cause__ is e, e.__context__)
