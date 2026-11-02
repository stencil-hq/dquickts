# Builtin exception hierarchy: base classes, MROs, BaseException vs Exception.


def chain(cls):
    return " > ".join(c.__name__ for c in cls.__mro__)


for cls in [
    KeyError,
    IndexError,
    ZeroDivisionError,
    OverflowError,
    UnboundLocalError,
    ModuleNotFoundError,
    FileNotFoundError,
    RecursionError,
    NotImplementedError,
    UnicodeDecodeError,
    StopIteration,
    StopAsyncIteration,
    GeneratorExit,
    KeyboardInterrupt,
    SystemExit,
    TimeoutError,
    IndentationError,
    TabError,
    DeprecationWarning,
    ExceptionGroup,
]:
    print(chain(cls))

print("KeyError LookupError", issubclass(KeyError, LookupError))
print("IndexError LookupError", issubclass(IndexError, LookupError))
print("ZeroDivision Arithmetic", issubclass(ZeroDivisionError, ArithmeticError))
print("FloatingPointError Arithmetic", issubclass(FloatingPointError, ArithmeticError))
print("UnicodeError ValueError", issubclass(UnicodeError, ValueError))
print("IOError is OSError", IOError is OSError, EnvironmentError is OSError)
print("Warning Exception", issubclass(Warning, Exception), issubclass(UserWarning, Warning))
print("GeneratorExit not Exception", issubclass(GeneratorExit, Exception))
print("KeyboardInterrupt not Exception", issubclass(KeyboardInterrupt, Exception))
print("SystemExit not Exception", issubclass(SystemExit, Exception))
print("StopIteration Exception", issubclass(StopIteration, Exception))
print("ExceptionGroup both", issubclass(ExceptionGroup, Exception), issubclass(ExceptionGroup, BaseExceptionGroup))
print("BaseException bases", BaseException.__bases__ == (object,))
print("OSError family", issubclass(PermissionError, OSError), issubclass(ConnectionResetError, ConnectionError))


def catch_with_exception(exc):
    try:
        try:
            raise exc
        except Exception as e:
            return "Exception caught " + type(e).__name__
    except BaseException as e:
        return "BaseException caught " + type(e).__name__


for exc in [ValueError("v"), KeyboardInterrupt(), SystemExit(3), GeneratorExit(), StopIteration(1)]:
    print("catch", catch_with_exception(exc))

try:
    raise SystemExit(5)
except SystemExit as e:
    print("SystemExit code", e.code, e.args)
try:
    raise SystemExit
except SystemExit as e:
    print("SystemExit default code", e.code)
try:
    raise SystemExit("message", 2)
except SystemExit as e:
    print("SystemExit multi args", e.code)

try:
    raise KeyboardInterrupt
except Exception:
    print("must not happen")
except BaseException as e:
    print("KeyboardInterrupt via BaseException", repr(e))

try:
    raise LookupError("generic lookup")
except (KeyError, IndexError):
    print("must not happen")
except LookupError as e:
    print("parent caught", type(e).__name__, e)

try:
    {}["key"]
except LookupError as e:
    print("subclass caught by parent", type(e).__name__, e)

e = OSError(2, "No such file or directory")
print("OSError mapping", type(e).__name__, e.errno, e.strerror, str(e))
print("OSError single arg", type(OSError("x")).__name__, OSError("x").errno)
print("OSError errno 13", type(OSError(13, "denied")).__name__)
ue = UnicodeDecodeError("utf-8", b"\xff", 0, 1, "invalid start byte")
print("UnicodeDecodeError", ue.encoding, ue.object, ue.start, ue.end, ue.reason)
print("str", str(ue))
se = SyntaxError("bad", ("file.py", 3, 5, "x = (", 3, 6))
print("SyntaxError attrs", se.msg, se.filename, se.lineno, se.offset, se.text, se.end_lineno, se.end_offset)
print("SyntaxError str", str(se))
ke = KeyError("k")
print("KeyError str repr", str(ke), repr(ke), str(KeyError()), str(KeyError("a", "b")))
si = StopIteration(42)
print("StopIteration value", si.value, StopIteration().value)
ni = NameError("msg", name="nm")
print("NameError name", ni.name, str(ni))
ae = AttributeError("m", name="n", obj=5)
print("AttributeError name obj", ae.name, ae.obj)
ie = ImportError("m", name="mod", path="p")
print("ImportError attrs", ie.name, ie.path, ie.msg)
try:
    BaseException.__new__(int)
except TypeError as e:
    print(type(e).__name__, e)
