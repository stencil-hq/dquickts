# raise statement forms: bare re-raise, raising classes vs instances, invalid raise targets,
# raising inside handlers, and re-raising a stored exception later.


def reraise():
    try:
        int("x")
    except ValueError:
        raise


try:
    reraise()
except ValueError as e:
    print("bare raise", type(e).__name__, e)

try:
    raise
except RuntimeError as e:
    print("bare raise no active", type(e).__name__, e)


def reraise_outside_handler():
    raise


try:
    reraise_outside_handler()
except RuntimeError as e:
    print("bare raise in function", type(e).__name__, e)


def handler_raises():
    try:
        raise ValueError("first")
    except ValueError:
        raise TypeError("second")


try:
    handler_raises()
except TypeError as e:
    print("handler raises", e, "context", repr(e.__context__))

try:
    try:
        raise ValueError(1)
    except 5:
        pass
except TypeError as e:
    print("bad except target", type(e).__name__, e)

try:
    try:
        raise ValueError(1)
    except (KeyError, "str"):
        pass
except TypeError as e:
    print("bad tuple except target", type(e).__name__, e)

for target in [5, "message", None, object(), int]:
    try:
        raise target
    except TypeError as e:
        print("raise", type(target).__name__, "->", type(e).__name__, e)

try:
    raise ValueError
except ValueError as e:
    print("raise class instantiates", repr(e), e.args)


class NeedsArgs(Exception):
    def __init__(self, a, b):
        super().__init__(a + b)


try:
    raise NeedsArgs
except TypeError as e:
    print("class needing args", type(e).__name__, e)


class FactoryResult(Exception):
    def __new__(cls):
        return "not an exception"


try:
    raise FactoryResult
except TypeError as e:
    print("class constructing non-exception", type(e).__name__, e)

saved = None
try:
    {}["missing"]
except KeyError as e:
    saved = e
try:
    raise saved
except KeyError as e:
    print("raise stored later", e is saved, repr(e))


def raise_in_nested_handlers():
    try:
        raise ValueError("outer")
    except ValueError:
        try:
            raise KeyError("inner")
        except KeyError:
            pass
        raise


try:
    raise_in_nested_handlers()
except ValueError as e:
    print("bare raise after nested handler re-raises outer", type(e).__name__, e)


def raise_in_finally_after_return():
    try:
        return "value"
    finally:
        pass


print("normal return through finally", raise_in_finally_after_return())
try:
    raise ValueError("with traceback") from None
except ValueError as e:
    print("traceback object present", e.__traceback__ is not None, type(e.__traceback__).__name__)
