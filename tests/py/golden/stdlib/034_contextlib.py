"""L0 golden: contextlib contextmanager, suppress, redirect_stdout/stderr."""
import contextlib
import io
import sys

log = []


@contextlib.contextmanager
def tag(name):
    log.append(f"enter {name}")
    try:
        yield name.upper()
    except KeyError as e:
        log.append(f"handled {e}")
    finally:
        log.append(f"exit {name}")


with tag("a") as t:
    log.append(f"body {t}")
print("contextmanager", log)
log.clear()
with tag("b"):
    raise KeyError("k")
print("swallowed", log)
log.clear()
try:
    with tag("c"):
        raise ValueError("v")
except ValueError as e:
    print("propagated", repr(e), log)


@contextlib.contextmanager
def no_yield():
    if False:
        yield


try:
    with no_yield():
        pass
except RuntimeError as e:
    print("no yield", type(e).__name__, e)


@contextlib.contextmanager
def twice():
    yield 1
    yield 2


try:
    with twice():
        pass
except RuntimeError as e:
    print("two yields", type(e).__name__, e)


@tag("deco")
def decorated():
    log.append("in function")


log.clear()
decorated()
print("as decorator", log)

with contextlib.suppress(ZeroDivisionError, KeyError):
    1 / 0
print("suppress ok")
with contextlib.suppress(KeyError):
    {}["x"]
try:
    with contextlib.suppress(KeyError):
        [][1]
except IndexError as e:
    print("suppress other", type(e).__name__)
try:
    with contextlib.suppress(ValueError):
        raise ExceptionGroup("eg", [ValueError(1), TypeError(2)])
except ExceptionGroup as eg:
    print("suppress group remainder", [type(x).__name__ for x in eg.exceptions])

buf = io.StringIO()
with contextlib.redirect_stdout(buf):
    print("captured line")
    print("second", 2)
print("redirect_stdout", repr(buf.getvalue()))
err = io.StringIO()
with contextlib.redirect_stderr(err):
    print("to stderr", file=sys.stderr)
print("redirect_stderr", repr(err.getvalue()))
print("stdout restored", sys.stdout is sys.__stdout__)
with contextlib.chdir("."):
    print("chdir is a context manager", True)
