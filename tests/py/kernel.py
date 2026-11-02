"""Replay kernel for the L1 corpus differential (runs under the IUT and under CPython).

``tests/py/replay.py`` starts one kernel under the implementation under test
and one under CPython and feeds both the same notebook cells.  The kernel
mimics the omp eval kernel closely enough for recorded sessions to replay:
a persistent ``__main__`` namespace preloaded with a prelude shim, IPython-style
magic lines turned into ``pass``, top-level ``await`` on one persistent event
loop, and display of the value of a trailing expression.

Protocol (one JSON object per line):

- Requests arrive on stdin: ``{"op": "exec", "code": str}``, ``{"op": "reset"}``
  or ``{"op": "quit"}``.
- Responses go to the *original* stdout, which the kernel duplicates at start
  (``proto = os.dup(1)``); fd 0 and fd 1 are then pointed at ``/dev/null`` so
  cell code can neither consume requests nor corrupt responses.
- ``exec`` answers ``{"stdout": str, "stderr": str (last 4000 chars),
  "exc": null | {"type": str, "msg": str}, "display": str | null, "ms": float}``.
  While a cell runs, fd 1 and fd 2 point at private temp files, so output of
  Python code and of subprocesses is captured alike.
- ``reset`` answers ``{"ok": true}`` after replacing the namespace (and the
  event loop) with fresh ones; ``quit`` answers ``{"ok": true}`` and exits.

The kernel machinery uses only portable stdlib modules (sys, os, io, json, ast,
asyncio, contextlib, traceback, time, types); the prelude additionally imports
what the omp prelude imports, and a failing prelude import is reported on the
original stderr without stopping the kernel.

Usage::

    printf '%s\\n' '{"op": "exec", "code": "x = 6\\nx * 7"}' '{"op": "quit"}' \\
        | python3.15 tests/py/kernel.py
    # {"stdout": "", "stderr": "", "exc": null, "display": "42", "ms": 0.05}
    # {"ok": true}
"""

import ast
import asyncio
import builtins
import contextlib
import io
import json
import os
import sys
import time
import traceback
import types

#: ``inspect.CO_COROUTINE``; set on code compiled from cells using top-level await.
CO_COROUTINE = 0x0080
COMPILE_FLAGS = ast.PyCF_ALLOW_TOP_LEVEL_AWAIT
CELL_FILENAME = "<cell>"
STDERR_TAIL = 4000
#: Cap on captured stdout per cell, so a runaway cell cannot produce an
#: unbounded response line.  Truncation is deterministic, so both kernels agree.
STDOUT_LIMIT = 1_000_000

#: (namespace name, module to import) pairs mirroring the omp prelude's imports.
PRELUDE_MODULES = (
    ("asyncio", "asyncio"),
    ("collections", "collections.abc"),
    ("contextvars", "contextvars"),
    ("inspect", "inspect"),
    ("os", "os"),
    ("json", "json"),
    ("math", "math"),
    ("re", "re"),
    ("time", "time"),
    ("types", "types"),
    ("typing", "typing"),
    ("urllib", "urllib.parse"),
)

#: Prelude helpers that need the omp host (tool bridge, models, subagents).
UNAVAILABLE_HELPERS = (
    "agent", "budget", "completion", "judge", "judge_batch", "output", "parallel",
    "tool", "wait", "workpool",
)


# --------------------------------------------------------------------------
# Prelude shim
# --------------------------------------------------------------------------

def _is_internal_url(path: object) -> bool:
    """True for omp internal URLs such as ``local://x.md`` or ``artifact://3``."""
    if not isinstance(path, str):
        return False
    scheme, sep, _ = path.partition("://")
    return bool(sep and scheme and scheme[0].isalpha()
                and all(c.isalnum() or c in "+.-" for c in scheme))


def env(key: str | None = None, value: str | None = None):
    """Get/set environment variables (omp prelude ``env``).

    ``env()`` returns all variables as a sorted dict, ``env(key)`` one value
    (or None), ``env(key, value)`` sets and returns ``value``.
    """
    if key is None:
        return dict(sorted(os.environ.items()))
    if value is not None:
        os.environ[key] = value
        return value
    return os.environ.get(key)


def read(path, offset: int = 1, limit: int | None = None) -> str:
    """Read a UTF-8 text file (omp prelude ``read``); offset/limit are 1-indexed lines.

    Internal URLs need the omp read tool and raise ``RuntimeError`` in replay.
    """
    if _is_internal_url(path):
        raise RuntimeError(f"read({path!r}) is not available in replay")
    with open(path, encoding="utf-8") as f:
        data = f.read()
    if offset > 1 or limit is not None:
        lines = data.splitlines(keepends=True)
        start = max(0, offset - 1)
        end = start + limit if limit else len(lines)
        data = "".join(lines[start:end])
    return data


def write(path, content: str):
    """Write a UTF-8 text file, creating parent directories (omp prelude ``write``).

    Returns the ``pathlib.Path`` written.  Internal URLs raise ``ValueError``
    like the omp prelude does when no local root is configured.
    """
    from pathlib import Path

    if _is_internal_url(path):
        raise ValueError(f"Protocol paths are not supported by this helper: {path}")
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(content, encoding="utf-8")
    return p


def display(value) -> None:
    """Show a value: strings are printed as-is, anything else as its ``repr``."""
    if value is not None:
        print(value if isinstance(value, str) else repr(value))


def log(message) -> None:
    """omp prelude ``log``: emits a UI status event, which replay discards."""
    return None


def phase(title) -> None:
    """omp prelude ``phase``: records the current phase in the cell namespace."""
    sys.modules["__main__"].__dict__["__omp_current_phase__"] = str(title)


class ShellResult:
    """Result of ``run``: ``stdout``, ``stderr`` and ``exit_code``.

    Fields are also reachable by item access (``r["stdout"]``) and
    ``returncode`` aliases ``exit_code``, matching how old sessions used it.
    """

    __slots__ = ("stdout", "stderr", "exit_code")

    def __init__(self, stdout: str, stderr: str, exit_code: int):
        self.stdout = stdout
        self.stderr = stderr
        self.exit_code = exit_code

    @property
    def returncode(self) -> int:
        return self.exit_code

    @property
    def ok(self) -> bool:
        return self.exit_code == 0

    def __getitem__(self, key: str):
        if key in ("stdout", "stderr", "exit_code", "returncode"):
            return getattr(self, key)
        raise KeyError(key)

    def __repr__(self) -> str:
        return f"ShellResult(exit_code={self.exit_code!r}, stdout={self.stdout!r}, stderr={self.stderr!r})"


def run(cmd, cwd=None, timeout: float | None = None, env: dict | None = None) -> ShellResult:
    """Run a command (a string goes through the shell) and capture its output.

    stdin is ``/dev/null``; output is decoded as UTF-8 with replacement.
    """
    import subprocess

    proc = subprocess.run(
        cmd, shell=isinstance(cmd, str), cwd=cwd, timeout=timeout, env=env,
        stdin=subprocess.DEVNULL, capture_output=True, text=True,
        encoding="utf-8", errors="replace",
    )
    return ShellResult(proc.stdout, proc.stderr, proc.returncode)


def sh(cmd, cwd=None, timeout: float | None = None) -> str:
    """Run a shell command and return its stdout followed by its stderr."""
    result = run(cmd, cwd=cwd, timeout=timeout)
    return result.stdout + result.stderr


class Unavailable:
    """Stand-in for an omp helper that needs the agent host.

    Calling it, or using any public attribute (``tool.bash(...)``), raises
    ``RuntimeError('<name> is not available in replay')``.
    """

    def __init__(self, name: str):
        self._name = name

    def _fail(self):
        raise RuntimeError(f"{self._name} is not available in replay")

    def __call__(self, *args, **kwargs):
        self._fail()

    def __getattr__(self, attr: str):
        if attr.startswith("_"):
            raise AttributeError(attr)
        self._fail()

    def __repr__(self) -> str:
        return f"<{self._name} (not available in replay)>"


PRELUDE_HELPERS = {
    "display": display,
    "env": env,
    "log": log,
    "phase": phase,
    "read": read,
    "run": run,
    "sh": sh,
    "write": write,
}


def new_main_module() -> tuple[types.ModuleType, list[str]]:
    """Create a fresh ``__main__`` module with the prelude loaded.

    Returns the module and a list of prelude import failures (normally empty).
    """
    module = types.ModuleType("__main__")
    ns = module.__dict__
    ns["__builtins__"] = builtins
    errors = []
    for name, module_name in PRELUDE_MODULES:
        try:
            ns[name] = __import__(module_name)
        except Exception as e:
            errors.append(f"import {module_name}: {type(e).__name__}: {e}")
    for name, module_name, attr in (("Path", "pathlib", "Path"), ("unquote", "urllib.parse", "unquote")):
        try:
            ns[name] = getattr(__import__(module_name, fromlist=[attr]), attr)
        except Exception as e:
            errors.append(f"from {module_name} import {attr}: {type(e).__name__}: {e}")
    ns.update(PRELUDE_HELPERS)
    for name in UNAVAILABLE_HELPERS:
        ns[name] = Unavailable(name)
    return module, errors


# --------------------------------------------------------------------------
# Cell execution
# --------------------------------------------------------------------------

def preprocess(code: str) -> str:
    """Replace IPython-style magic lines (``%...``, ``!...``) with ``pass``."""
    out = []
    for line in code.split("\n"):
        stripped = line.lstrip()
        if stripped.startswith(("%", "!")):
            out.append(line[: len(line) - len(stripped)] + "pass")
        else:
            out.append(line)
    return "\n".join(out)


def exception_name(exc: BaseException) -> str:
    """Qualified class name: ``ValueError``, ``json.decoder.JSONDecodeError``."""
    cls = type(exc)
    if cls.__module__ == "builtins":
        return cls.__qualname__
    return f"{cls.__module__}.{cls.__qualname__}"


def _safe_str(exc: BaseException) -> str:
    try:
        return str(exc)
    except BaseException as e:
        return f"<str() failed: {type(e).__name__}>"


def _text_stream(fd: int) -> io.TextIOWrapper:
    """Unbuffered UTF-8 text stream over ``fd`` (keeps order with subprocess output)."""
    raw = io.FileIO(fd, "w", closefd=False)
    return io.TextIOWrapper(raw, encoding="utf-8", errors="backslashreplace",
                            line_buffering=True, write_through=True)


def _private_file(tag: str) -> int:
    """Open an anonymous read/write temp file (created and immediately unlinked)."""
    tmpdir = os.environ.get("TMPDIR") or "/tmp"
    base = os.path.join(tmpdir, f".replay-kernel-{os.getpid()}-{tag}")
    for attempt in range(100):
        path = f"{base}-{attempt}"
        try:
            fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            continue
        os.unlink(path)
        return fd
    raise RuntimeError(f"cannot create a capture file under {tmpdir}")


def _read_file(fd: int) -> str:
    """Return the whole content of a capture file, decoded as UTF-8."""
    os.lseek(fd, 0, os.SEEK_SET)
    chunks = []
    while chunk := os.read(fd, 1 << 16):
        chunks.append(chunk)
    return b"".join(chunks).decode("utf-8", "replace")


def _flush_all(*streams) -> None:
    for stream in streams:
        with contextlib.suppress(Exception):
            stream.flush()


class Kernel:
    """Persistent cell executor with fd-level output capture."""

    def __init__(self) -> None:
        # The script itself runs as __main__; keep its module alive once the
        # cell namespace takes over sys.modules["__main__"].
        self.host_module = sys.modules["__main__"]
        self.out_fd = _private_file("stdout")
        self.err_fd = _private_file("stderr")
        self.module: types.ModuleType | None = None
        self.loop: asyncio.AbstractEventLoop | None = None
        self.reset()

    def reset(self) -> None:
        """Install a fresh ``__main__`` namespace and a fresh event loop."""
        if self.loop is not None:
            with contextlib.suppress(Exception):
                self.loop.close()
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self.module, errors = new_main_module()
        sys.modules["__main__"] = self.module
        for error in errors:
            print(f"kernel: prelude {error}", file=sys.__stderr__, flush=True)

    def _evaluate(self, code: types.CodeType):
        """Run a compiled cell part in the namespace, driving coroutine code on the loop."""
        result = eval(code, self.module.__dict__)
        if code.co_flags & CO_COROUTINE:
            result = self.loop.run_until_complete(result)
        return result

    def _run_cell(self, source: str, err_stream: io.TextIOBase) -> tuple[str | None, dict | None]:
        """Compile and run one cell; returns ``(display, exc)``."""
        try:
            tree = compile(preprocess(source), CELL_FILENAME, "exec",
                           ast.PyCF_ONLY_AST | COMPILE_FLAGS)
            last = None
            if tree.body and isinstance(tree.body[-1], ast.Expr):
                last = ast.Expression(tree.body.pop().value)
            self._evaluate(compile(tree, CELL_FILENAME, "exec", COMPILE_FLAGS))
            if last is not None:
                value = self._evaluate(compile(last, CELL_FILENAME, "eval", COMPILE_FLAGS))
                if value is not None:
                    return repr(value), None
            return None, None
        except BaseException as exc:
            with contextlib.suppress(Exception):
                traceback.print_exception(exc, file=err_stream)
            return None, {"type": exception_name(exc), "msg": _safe_str(exc)}

    def execute(self, source: str) -> dict:
        """Run one cell with fd 1/2 captured and return the protocol response."""
        start = time.perf_counter()
        for fd in (self.out_fd, self.err_fd):
            os.ftruncate(fd, 0)
            os.lseek(fd, 0, os.SEEK_SET)
        _flush_all(sys.stdout, sys.stderr, sys.__stdout__, sys.__stderr__)
        saved_fds = os.dup(1), os.dup(2)
        saved_streams = sys.stdout, sys.stderr
        os.dup2(self.out_fd, 1)
        os.dup2(self.err_fd, 2)
        out_stream, err_stream = _text_stream(1), _text_stream(2)
        sys.stdout, sys.stderr = out_stream, err_stream
        asyncio.set_event_loop(self.loop)  # a cell's asyncio.run() unsets it
        try:
            display_value, exc = self._run_cell(source, err_stream)
        finally:
            _flush_all(sys.stdout, sys.stderr, out_stream, err_stream,
                       sys.__stdout__, sys.__stderr__)
            sys.stdout, sys.stderr = saved_streams
            os.dup2(saved_fds[0], 1)
            os.dup2(saved_fds[1], 2)
            os.close(saved_fds[0])
            os.close(saved_fds[1])
            asyncio.set_event_loop(self.loop)
        stdout = _read_file(self.out_fd)
        if len(stdout) > STDOUT_LIMIT:
            stdout = stdout[:STDOUT_LIMIT] + f"\n[kernel: {len(stdout) - STDOUT_LIMIT} chars truncated]\n"
        return {
            "stdout": stdout,
            "stderr": _read_file(self.err_fd)[-STDERR_TAIL:],
            "exc": exc,
            "display": display_value,
            "ms": round((time.perf_counter() - start) * 1000, 3),
        }


# --------------------------------------------------------------------------
# Protocol loop
# --------------------------------------------------------------------------

def _send(fd: int, obj: dict) -> None:
    data = (json.dumps(obj, ensure_ascii=True) + "\n").encode("ascii")
    while data:
        data = data[os.write(fd, data):]


def main() -> None:
    """Serve requests from stdin until ``quit`` or EOF, then exit immediately."""
    # Cells must not import the harness modules that live next to this file.
    if sys.path and os.path.abspath(sys.path[0] or ".") == os.path.dirname(os.path.abspath(__file__)):
        del sys.path[0]
    proto_out = os.dup(1)
    proto_in = os.fdopen(os.dup(0), "rb")
    null_fd = os.open(os.devnull, os.O_RDWR)
    os.dup2(null_fd, 0)
    os.dup2(null_fd, 1)
    os.close(null_fd)

    kernel = Kernel()
    for line in proto_in:
        if not line.strip():
            continue
        try:
            request = json.loads(line)
            op = request["op"]
        except (ValueError, KeyError, TypeError) as e:
            _send(proto_out, {"error": f"bad request: {e}"})
            continue
        if op == "exec":
            _send(proto_out, kernel.execute(str(request.get("code", ""))))
        elif op == "reset":
            kernel.reset()
            _send(proto_out, {"ok": True})
        elif op == "quit":
            _send(proto_out, {"ok": True})
            break
        else:
            _send(proto_out, {"error": f"unknown op: {op!r}"})
    _flush_all(sys.stdout, sys.stderr, sys.__stdout__, sys.__stderr__)
    # Skip interpreter shutdown: cells may leave non-daemon threads or children behind.
    os._exit(0)


if __name__ == "__main__":
    main()
