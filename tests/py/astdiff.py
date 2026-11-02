"""L2 parser differential: compare the AST (or compile verdict) of an implementation under test
against CPython, request by request, over the JSON AST protocol.

Runs under host CPython 3.15 (stdlib only).  The implementation under test (IUT, default
``./qpy``) and the reference are both driven through the line protocol documented in
``tests/py/ref_dump_ast.py`` (``CMD --dump-ast-json-batch``: one JSON request per stdin line,
exactly one JSON response per stdout line, same order).  This tool relies on two optional
request keys beyond the base contract:

- ``"top_level_await": true`` -- parse with ``ast.PyCF_ALLOW_TOP_LEVEL_AWAIT``; always set for
  corpus cells.
- ``"check": true`` -- compile fully (no ``PyCF_ONLY_AST``) so symtable/compiler SyntaxErrors
  are reported too; the success response is ``{"id": ..., "ok": true}`` without ``"ast"``.
  Used for every request with ``--check`` and always for ``--syntax-cases``.

Usage::

    python3.15 tests/py/astdiff.py --lib --impl "python3.15 tests/py/ref_dump_ast.py"
    python3.15 tests/py/astdiff.py --lib --corpus --syntax-cases -j 8
    python3.15 tests/py/astdiff.py --files a.py b.py --check --ref "python3.15 tests/py/ref_dump_ast.py"

Inputs (combinable; each is reported under its own source label):

- ``--lib``: every ``*.py`` under ``cpython/Lib`` (sorted; decoded via
  ``tokenize.detect_encoding``; undecodable files are counted as skipped).
- ``--corpus [PATH]``: ``cells.jsonl`` from ``tests/py/corpus.py`` (default
  ``~/.cache/dquickts/corpus/cells.jsonl``); each cell's ``source_pp`` is sent with
  ``top_level_await: true``.
- ``--files F...``: explicit files.
- ``--syntax-cases``: syntax snippets mined from CPython's tests: doctest examples in the module
  docstrings of ``test_syntax.py``/``test_grammar.py`` and literal first arguments of
  ``self._check_error``/``_check_error``/``self.assertSyntaxError``/
  ``check_syntax_error(self, ...)`` calls in a fixed list of ``Lib/test`` files.  Always
  check-mode; ids are ``file:lineno#n``.

Comparison: both responses are decoded into one canonical structure (tagged floats/complex
normalised through ``float.fromhex``, so NaN equals NaN) and compared exactly, positions
included and JSON types strict (``1``, ``1.0`` and ``true`` differ).  Both-error requests match
when ``type`` and ``lineno`` agree; a ``msg``/``offset`` difference is a *soft* mismatch
(reported, counted as passing).  Error on one side only is an error-kind mismatch.  The first
differing JSON path is reported as ``body[3].value.args[0].col_offset: 12 != 13`` with the
IUT value first (``impl != ref``).

The IUT runs in ``-j`` worker processes, each keeping one long-lived IUT process fed chunks of
up to ``--chunk-size`` requests.  A dead IUT, a malformed or out-of-order response line marks
the in-flight request ``crash``; no response within ``--timeout`` seconds marks it
``timeout``; either way a fresh IUT process continues with the next request.

Report: totals per source on stdout, the first 20 mismatches, and the full report (all
non-equal requests, capped at 10,000) in ``build/testpy/astdiff-report.json``.

Exit status: 0 if every selected source meets its equal-percentage threshold
(``--min-lib-pct`` 100, ``--min-corpus-pct`` 99.9, ``--min-files-pct`` 100,
``--min-syntax-pct`` 100), 1 otherwise, 2 on harness errors (missing IUT, missing inputs).
"""

from __future__ import annotations

import argparse
import ast
import atexit
import concurrent.futures
import doctest
import io
import json
import math
import os
import queue
import shlex
import shutil
import subprocess
import sys
import tempfile
import threading
import tokenize
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import ref_dump_ast  # noqa: E402  (needs HERE on sys.path)

REPO_ROOT = HERE.parent.parent
DEFAULT_IMPL = "./qpy"
DEFAULT_LIB_DIR = REPO_ROOT / "cpython" / "Lib"
DEFAULT_CORPUS = Path.home() / ".cache" / "dquickts" / "corpus" / "cells.jsonl"
DEFAULT_REPORT = REPO_ROOT / "build" / "testpy" / "astdiff-report.json"
REPORT_CAP = 10_000
PRINT_CAP = 20

SOURCES = ("lib", "corpus", "files", "syntax")

# Request outcome categories.
EQUAL = "equal"
SOFT = "soft"
AST_MISMATCH = "ast_mismatch"
ERROR_MISMATCH = "error_mismatch"
CRASH = "crash"
TIMEOUT = "timeout"
REF_FAILURE = "ref_failure"
STATUSES = (EQUAL, SOFT, AST_MISMATCH, ERROR_MISMATCH, CRASH, TIMEOUT, REF_FAILURE)
PASSING = frozenset({EQUAL, SOFT})

SYNTAX_DOCTEST_FILES = ("test_syntax.py", "test_grammar.py")
SYNTAX_CALL_FILES = (
    "test_syntax.py", "test_grammar.py", "test_exceptions.py", "test_fstring.py",
    "test_positional_only_arg.py", "test_keywordonlyarg.py", "test_unpack_ex.py",
    "test_patma.py", "test_named_expressions.py", "test_coroutines.py", "test_generators.py",
    "test_scope.py", "test_string_literals.py",
)


class HarnessError(Exception):
    """A setup problem that makes the run meaningless (exit status 2)."""


# --------------------------------------------------------------------------- inputs


@dataclass
class Case:
    """One protocol request plus the input source label it is reported under."""

    source: str
    req: dict[str, Any]


@dataclass
class Inputs:
    """Collected requests and the inputs that could not be turned into requests."""

    cases: list[Case] = field(default_factory=list)
    skipped: list[dict[str, str]] = field(default_factory=list)


def read_source(path: Path) -> str:
    """Decode a Python source file the way the tokenizer does (cookie/BOM aware)."""
    data = path.read_bytes()
    encoding, _ = tokenize.detect_encoding(io.BytesIO(data).readline)
    return data.decode(encoding)


def _request(req_id: str, filename: str, source: str, *, check: bool,
             top_level_await: bool = False) -> dict[str, Any]:
    """Build a protocol request; optional keys are only present when set."""
    req: dict[str, Any] = {"id": req_id, "filename": filename, "source": source, "mode": "exec"}
    if top_level_await:
        req["top_level_await"] = True
    if check:
        req["check"] = True
    return req


def collect_lib(lib_dir: Path, inputs: Inputs, check: bool) -> None:
    """Add every ``*.py`` under ``lib_dir`` (ids relative to its parent, e.g. ``Lib/os.py``)."""
    if not lib_dir.is_dir():
        raise HarnessError(f"CPython Lib directory not found: {lib_dir} (see --lib-dir)")
    for path in sorted(lib_dir.rglob("*.py")):
        rel = path.relative_to(lib_dir.parent).as_posix()
        try:
            source = read_source(path)
        except (SyntaxError, UnicodeDecodeError, LookupError, OSError) as exc:
            inputs.skipped.append({"source": "lib", "id": rel,
                                   "reason": f"{type(exc).__name__}: {exc}"})
            continue
        inputs.cases.append(Case("lib", _request(rel, rel, source, check=check)))


def collect_corpus(path: Path, inputs: Inputs, check: bool) -> None:
    """Add every cell of a corpus ``cells.jsonl`` (``source_pp`` with top-level await)."""
    if not path.is_file():
        raise HarnessError(f"corpus file not found: {path} (run tests/py/corpus.py first)")
    with path.open(encoding="utf-8", errors="surrogateescape") as fh:
        for lineno, line in enumerate(fh, 1):
            if not line.strip():
                continue
            try:
                cell = json.loads(line)
                cell_id = str(cell["id"])
                source = cell["source_pp"]
                if not isinstance(source, str):
                    raise TypeError("source_pp is not a string")
            except (ValueError, KeyError, TypeError) as exc:
                inputs.skipped.append({"source": "corpus", "id": f"{path.name}:{lineno}",
                                       "reason": f"bad cell: {type(exc).__name__}: {exc}"})
                continue
            req = _request(cell_id, f"<corpus {cell_id}>", source, check=check,
                           top_level_await=True)
            inputs.cases.append(Case("corpus", req))


def collect_files(paths: list[Path], inputs: Inputs, check: bool) -> None:
    """Add explicitly named files; a missing file is a harness error."""
    for path in paths:
        if not path.is_file():
            raise HarnessError(f"file not found: {path}")
        name = str(path)
        try:
            source = read_source(path)
        except (SyntaxError, UnicodeDecodeError, LookupError, OSError) as exc:
            inputs.skipped.append({"source": "files", "id": name,
                                   "reason": f"{type(exc).__name__}: {exc}"})
            continue
        inputs.cases.append(Case("files", _request(name, name, source, check=check)))


def literal_str(node: ast.expr) -> str | None:
    """Fold a string literal or a ``+`` chain of string literals; else return None."""
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
        left, right = literal_str(node.left), literal_str(node.right)
        if left is not None and right is not None:
            return left + right
    return None


def _is_self(node: ast.expr) -> bool:
    return isinstance(node, ast.Name) and node.id == "self"


def syntax_call_snippet(call: ast.Call) -> ast.expr | None:
    """Return the snippet argument of a recognised syntax-check helper call, if any."""
    func, args = call.func, call.args
    if isinstance(func, ast.Attribute) and _is_self(func.value):
        if func.attr in ("_check_error", "assertSyntaxError") and args:
            return args[0]
        return None
    if isinstance(func, ast.Name):
        if func.id == "_check_error" and args:
            return args[0]
        if func.id == "check_syntax_error" and len(args) >= 2 and _is_self(args[0]):
            return args[1]
    return None


def collect_syntax_cases(test_dir: Path, inputs: Inputs) -> None:
    """Add syntax snippets mined from CPython's syntax-related tests (always check-mode)."""
    if not test_dir.is_dir():
        raise HarnessError(f"CPython test directory not found: {test_dir} (see --lib-dir)")
    parser = doctest.DocTestParser()
    for name in dict.fromkeys(SYNTAX_DOCTEST_FILES + SYNTAX_CALL_FILES):
        path = test_dir / name
        if not path.is_file():
            continue
        snippets: list[tuple[int, str]] = []
        try:
            tree = ast.parse(read_source(path), filename=str(path))
            doc = ast.get_docstring(tree, clean=False)
            if name in SYNTAX_DOCTEST_FILES and doc is not None:
                # Example line numbers are 0-based offsets from the docstring's first line.
                base = tree.body[0].lineno
                snippets.extend((base + ex.lineno, ex.source)
                                for ex in parser.get_examples(doc, name))
        except (SyntaxError, UnicodeDecodeError, LookupError, ValueError) as exc:
            inputs.skipped.append({"source": "syntax", "id": name,
                                   "reason": f"{type(exc).__name__}: {exc}"})
            continue
        if name in SYNTAX_CALL_FILES:
            calls = sorted((n for n in ast.walk(tree) if isinstance(n, ast.Call)),
                           key=lambda n: (n.lineno, n.col_offset))
            for call in calls:
                arg = syntax_call_snippet(call)
                text = literal_str(arg) if arg is not None else None
                if text is not None:
                    snippets.append((call.lineno, text))
        for n, (lineno, text) in enumerate(snippets):
            case_id = f"{name}:{lineno}#{n}"
            inputs.cases.append(Case("syntax", _request(case_id, case_id, text, check=True)))


# --------------------------------------------------------------------------- protocol sessions


def _encode_request(req: dict[str, Any]) -> bytes:
    return json.dumps(req, ensure_ascii=True, separators=(",", ":")).encode("ascii") + b"\n"


def _pump_lines(stream: Any, lines: queue.SimpleQueue) -> None:
    """Reader thread: forward stdout lines of a protocol process; ``None`` marks EOF."""
    try:
        for line in stream:
            lines.put(line)
    except (OSError, ValueError):
        pass
    finally:
        lines.put(None)
        stream.close()


def _feed(stream: Any, payloads: list[bytes]) -> None:
    """Writer thread: send requests, flushing each so the process can answer immediately."""
    try:
        for payload in payloads:
            stream.write(payload)
            stream.flush()
    except (OSError, ValueError):
        pass  # process died or was killed; the reader reports it


def protocol_problem(req: dict[str, Any], resp: Any) -> str | None:
    """Describe why ``resp`` is not a well-formed answer to ``req``, or return None."""
    if not isinstance(resp, dict):
        return "response is not a JSON object"
    if resp.get("id") != req["id"]:
        return f"out-of-order response: id {resp.get('id')!r}, expected {req['id']!r}"
    ok = resp.get("ok")
    if not isinstance(ok, bool):
        return "response 'ok' is not a boolean"
    if ok:
        if not req.get("check") and not isinstance(resp.get("ast"), dict):
            return "ok response without an 'ast' object"
        return None
    err = resp.get("error")
    if not isinstance(err, dict) or not isinstance(err.get("type"), str):
        return "error response without an 'error' object with a string 'type'"
    return None


class Session:
    """A long-lived ``CMD --dump-ast-json-batch`` process, restarted after crashes/timeouts."""

    def __init__(self, argv: list[str], timeout: float) -> None:
        self.argv = [*argv, "--dump-ast-json-batch"]
        self.timeout = timeout
        self.proc: subprocess.Popen | None = None
        self.lines: queue.SimpleQueue | None = None
        self.writer: threading.Thread | None = None
        self.stderr: Any = None

    def _start(self) -> None:
        self.stderr = tempfile.TemporaryFile()
        try:
            self.proc = subprocess.Popen(self.argv, stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=self.stderr)
        except OSError as exc:
            raise HarnessError(f"cannot start {shlex.join(self.argv)}: {exc}") from exc
        self.lines = queue.SimpleQueue()
        threading.Thread(target=_pump_lines, args=(self.proc.stdout, self.lines),
                         daemon=True).start()

    def close(self) -> None:
        """Kill the process (if any) and release its pipes."""
        if self.proc is None:
            return
        self.proc.kill()
        self.proc.wait()
        if self.writer is not None:
            self.writer.join(timeout=5)
            self.writer = None
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        self.stderr.close()
        self.proc = None

    def _exit_detail(self) -> str:
        """Describe why the process stopped answering (exit status and stderr tail)."""
        try:
            code = self.proc.wait(timeout=2)
            what = f"process exited with status {code}"
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            what = "process closed stdout"
        self.stderr.seek(0, os.SEEK_END)
        size = self.stderr.tell()
        self.stderr.seek(max(0, size - 2000))
        tail = self.stderr.read().decode("utf-8", "replace").strip().splitlines()
        return f"{what}; stderr: {tail[-1][:300]}" if tail else what

    def _receive(self, req: dict[str, Any]) -> tuple[str, Any]:
        try:
            line = self.lines.get(timeout=self.timeout)
        except queue.Empty:
            return TIMEOUT, f"no response within {self.timeout:g}s"
        if line is None:
            return CRASH, self._exit_detail()
        try:
            resp = json.loads(line)
        except (ValueError, RecursionError) as exc:
            return CRASH, f"malformed response line ({type(exc).__name__}: {exc}): {line[:120]!r}"
        problem = protocol_problem(req, resp)
        if problem is not None:
            return CRASH, problem
        return "ok", resp

    def run(self, reqs: list[dict[str, Any]]) -> list[tuple[str, Any]]:
        """Answer ``reqs`` in order: ``("ok", response)`` or ``(CRASH|TIMEOUT, detail)``."""
        payloads = [_encode_request(r) for r in reqs]
        out: list[tuple[str, Any]] = []
        i = 0
        while i < len(reqs):
            if self.proc is None:
                self._start()
            self.writer = threading.Thread(target=_feed, args=(self.proc.stdin, payloads[i:]),
                                           daemon=True)
            self.writer.start()
            failed = False
            while i < len(reqs):
                outcome = self._receive(reqs[i])
                out.append(outcome)
                i += 1
                if outcome[0] != "ok":
                    failed = True
                    break
            if failed:
                self.close()
            else:
                self.writer.join(timeout=self.timeout)
                if self.writer.is_alive():
                    self.close()
                self.writer = None
        return out


# --------------------------------------------------------------------------- comparison


def _same(a: Any, b: Any) -> bool:
    """Strict scalar equality: JSON types must match too (``1``, ``1.0``, ``true`` differ)."""
    return type(a) is type(b) and a == b


def _fmt(value: Any, limit: int = 100) -> str:
    text = json.dumps(value, ensure_ascii=True)
    return text if len(text) <= limit else text[:limit] + "..."


def _join(path: str, key: str) -> str:
    return f"{path}.{key}" if path else key


def canonical_constant(tagged: dict[str, Any]) -> str:
    """Canonical text of a tagged constant; float/complex values normalised via fromhex."""
    canon = dict(tagged)
    if canon.get("t") in ("float", "complex") and isinstance(canon.get("v"), str):
        try:
            canon["v"] = float.fromhex(canon["v"]).hex()  # nan -> "nan", inf -> "inf"
        except (ValueError, OverflowError):
            pass  # malformed spelling: compared verbatim, so it mismatches
    return json.dumps(canon, sort_keys=True, ensure_ascii=True)


def first_difference(ref: Any, impl: Any, path: str = "") -> str | None:
    """Return the first differing JSON path as ``path: impl != ref``, or None if equal."""
    where = path or "<root>"
    if isinstance(ref, dict) and isinstance(impl, dict):
        if "t" in ref or "t" in impl:  # tagged constant (AST nodes have no field "t")
            if canonical_constant(ref) == canonical_constant(impl):
                return None
            return f"{where}: {_fmt(impl)} != {_fmt(ref)}"
        if not _same(ref.get("_type"), impl.get("_type")):
            return f"{_join(path, '_type')}: {_fmt(impl.get('_type'))} != {_fmt(ref.get('_type'))}"
        for key, value in ref.items():
            sub = _join(path, key)
            if key not in impl:
                return f"{sub}: missing in impl"
            diff = first_difference(value, impl[key], sub)
            if diff is not None:
                return diff
        for key in impl:
            if key not in ref:
                return f"{_join(path, key)}: unexpected in impl"
        return None
    if isinstance(ref, list) and isinstance(impl, list):
        for i, (r, m) in enumerate(zip(ref, impl)):
            diff = first_difference(r, m, f"{path}[{i}]")
            if diff is not None:
                return diff
        if len(ref) != len(impl):
            return f"{where}: length {len(impl)} != {len(ref)}"
        return None
    if _same(ref, impl):
        return None
    return f"{where}: {_fmt(impl)} != {_fmt(ref)}"


def _describe_response(resp: dict[str, Any]) -> str:
    if resp["ok"]:
        return "ok"
    err = resp["error"]
    return f"{err.get('type')}@{err.get('lineno')} {_fmt(err.get('msg'), 60)}"


def compare_responses(ref: dict[str, Any], impl: dict[str, Any]) -> tuple[str, str | None]:
    """Classify a pair of well-formed responses: (status, first-difference detail)."""
    if ref["ok"] and impl["ok"]:
        if "ast" not in ref:  # check mode: the verdict is all that is compared
            return EQUAL, None
        r, m = ref["ast"], impl["ast"]
        if json.dumps(r, sort_keys=True) == json.dumps(m, sort_keys=True):
            return EQUAL, None
        diff = first_difference(r, m)
        return (EQUAL, None) if diff is None else (AST_MISMATCH, diff)
    if ref["ok"] != impl["ok"]:
        return ERROR_MISMATCH, f"impl {_describe_response(impl)} != ref {_describe_response(ref)}"
    r_err, m_err = ref["error"], impl["error"]
    if not (_same(r_err.get("type"), m_err.get("type"))
            and _same(r_err.get("lineno"), m_err.get("lineno"))):
        return ERROR_MISMATCH, f"impl {_describe_response(impl)} != ref {_describe_response(ref)}"
    for key in ("msg", "offset"):
        if not _same(r_err.get(key), m_err.get(key)):
            return SOFT, f"error.{key}: {_fmt(m_err.get(key))} != {_fmt(r_err.get(key))}"
    return EQUAL, None


# --------------------------------------------------------------------------- workers

_WORKER: dict[str, Session | None] = {}


def _close_worker_sessions() -> None:
    for session in _WORKER.values():
        if session is not None:
            session.close()


def _worker_init(impl_argv: list[str], ref_argv: list[str] | None, timeout: float) -> None:
    """Process-pool initializer: create the (lazily started) IUT and reference sessions."""
    _WORKER["impl"] = Session(impl_argv, timeout)
    _WORKER["ref"] = Session(ref_argv, timeout) if ref_argv is not None else None
    atexit.register(_close_worker_sessions)


def run_chunk(chunk: list[tuple[int, Case]]) -> list[dict[str, Any]]:
    """Worker task: run one chunk through IUT and reference, return per-request results."""
    reqs = [case.req for _, case in chunk]
    impl_out = _WORKER["impl"].run(reqs)
    ref_session = _WORKER["ref"]
    if ref_session is None:
        ref_out = [("ok", ref_dump_ast.dump_request(req)) for req in reqs]
    else:
        ref_out = ref_session.run(reqs)
    results = []
    for (index, case), (ref_status, ref_val), (impl_status, impl_val) in zip(
            chunk, ref_out, impl_out):
        if ref_status != "ok":
            status, detail = REF_FAILURE, f"reference {ref_status}: {ref_val}"
        elif impl_status != "ok":
            status, detail = impl_status, impl_val
        else:
            status, detail = compare_responses(ref_val, impl_val)
        results.append({"index": index, "source": case.source, "id": case.req["id"],
                        "filename": case.req["filename"], "status": status, "detail": detail})
    return results


def run_cases(cases: list[Case], impl_argv: list[str], ref_argv: list[str] | None,
              timeout: float, jobs: int, chunk_size: int) -> list[dict[str, Any]]:
    """Run all cases on a process pool; results are returned in request order."""
    if not cases:
        return []
    size = max(1, min(chunk_size, math.ceil(len(cases) / (jobs * 4))))
    indexed = list(enumerate(cases))
    chunks = [indexed[i:i + size] for i in range(0, len(indexed), size)]
    results: list[dict[str, Any]] = []
    progress = sys.stderr.isatty()
    with concurrent.futures.ProcessPoolExecutor(
            max_workers=min(jobs, len(chunks)), initializer=_worker_init,
            initargs=(impl_argv, ref_argv, timeout)) as pool:
        futures = [pool.submit(run_chunk, chunk) for chunk in chunks]
        for future in concurrent.futures.as_completed(futures):
            try:
                results.extend(future.result())
            except Exception as exc:  # worker setup failure or harness bug: abort the run
                pool.shutdown(wait=False, cancel_futures=True)
                if isinstance(exc, HarnessError):
                    raise
                raise HarnessError(f"worker failed: {type(exc).__name__}: {exc}") from exc
            if progress:
                print(f"\rastdiff: {len(results)}/{len(cases)}", end="", file=sys.stderr,
                      flush=True)
    if progress:
        print(file=sys.stderr)
    results.sort(key=lambda r: r["index"])
    return results


# --------------------------------------------------------------------------- reporting


def resolve_command(cmd: str, what: str) -> list[str]:
    """Split a command string and check that its program exists (relative paths may also
    resolve against the repo root, which is how the default ``./qpy`` is found)."""
    argv = shlex.split(cmd)
    if not argv:
        raise HarnessError(f"empty {what} command")
    exe = argv[0]
    if "/" in exe:
        path = Path(exe)
        if not path.is_absolute() and not path.exists() and (REPO_ROOT / path).exists():
            path = REPO_ROOT / path
        if not (path.is_file() and os.access(path, os.X_OK)):
            raise HarnessError(f"{what} not found or not executable: {exe}")
        # Absolute: str(Path("./qpy")) is "qpy", which Popen would look up on PATH.
        argv[0] = str(path.resolve())
    elif shutil.which(exe) is None:
        raise HarnessError(f"{what} not found on PATH: {exe}")
    return argv


def summarize(sources: list[str], inputs: Inputs, results: list[dict[str, Any]],
              thresholds: dict[str, float]) -> dict[str, dict[str, Any]]:
    """Per-source totals, equal percentage (equal + soft) and threshold verdict."""
    summary: dict[str, dict[str, Any]] = {}
    for src in sources:
        counts = {status: 0 for status in STATUSES}
        for r in results:
            if r["source"] == src:
                counts[r["status"]] += 1
        requests = sum(counts.values())
        passing = counts[EQUAL] + counts[SOFT]
        pct = 100.0 * passing / requests if requests else 100.0
        summary[src] = {
            "requests": requests, **counts,
            "skipped": sum(1 for s in inputs.skipped if s["source"] == src),
            "equal_pct": pct, "min_pct": thresholds[src], "passed": pct >= thresholds[src],
        }
    return summary


def print_report(summary: dict[str, dict[str, Any]], results: list[dict[str, Any]]) -> None:
    """Print the totals table, overall equal percentage and the first mismatches."""
    cols = ("requests", EQUAL, AST_MISMATCH, ERROR_MISMATCH, SOFT, CRASH, TIMEOUT, REF_FAILURE,
            "skipped")
    heads = ("requests", "equal", "ast-mism", "err-mism", "soft", "crash", "timeout",
             "ref-fail", "skipped")
    print(f"{'source':<8}" + "".join(f"{h:>10}" for h in heads) + f"{'equal%':>10}{'min%':>8}")
    for src, row in summary.items():
        flag = "" if row["passed"] else "  FAIL"
        print(f"{src:<8}" + "".join(f"{row[c]:>10}" for c in cols)
              + f"{row['equal_pct']:>10.3f}{row['min_pct']:>8.1f}{flag}")
    total = len(results)
    passing = sum(1 for r in results if r["status"] in PASSING)
    pct = 100.0 * passing / total if total else 100.0
    print(f"overall equal: {pct:.3f}% ({passing}/{total})")
    bad = [r for r in results if r["status"] != EQUAL]
    bad.sort(key=lambda r: (r["status"] == SOFT, r["index"]))
    if bad:
        print(f"first mismatches (values shown as impl != ref), {len(bad)} total:")
        for r in bad[:PRINT_CAP]:
            name = r["id"] if r["id"] == r["filename"] else f"{r['id']} ({r['filename']})"
            print(f"  [{r['status']}] {r['source']} {name}: {r['detail']}")


def write_report(path: Path, args: argparse.Namespace, summary: dict[str, dict[str, Any]],
                 inputs: Inputs, results: list[dict[str, Any]]) -> None:
    """Write the full JSON report (non-equal requests capped at REPORT_CAP)."""
    bad = [{k: r[k] for k in ("source", "id", "filename", "status", "detail")}
           for r in results if r["status"] != EQUAL]
    report = {
        "impl": args.impl, "ref": args.ref, "check": args.check,
        "sources": summary,
        "mismatch_count": len(bad),
        "mismatches_truncated": len(bad) > REPORT_CAP,
        "mismatches": bad[:REPORT_CAP],
        "skipped": inputs.skipped[:REPORT_CAP],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(report, indent=1, ensure_ascii=True) + "\n", encoding="utf-8")


# --------------------------------------------------------------------------- CLI


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    """Parse the command line (argparse exits with status 2 on usage errors)."""
    p = argparse.ArgumentParser(
        description="L2 parser differential: IUT vs CPython over the JSON AST protocol.")
    p.add_argument("--lib", action="store_true", help="every *.py under --lib-dir")
    p.add_argument("--corpus", nargs="?", type=Path, const=DEFAULT_CORPUS, default=None,
                   metavar="PATH", help=f"corpus cells.jsonl (default {DEFAULT_CORPUS})")
    p.add_argument("--files", nargs="+", type=Path, default=[], metavar="F",
                   help="explicit source files")
    p.add_argument("--syntax-cases", action="store_true",
                   help="syntax snippets mined from CPython's Lib/test (check-mode)")
    p.add_argument("--check", action="store_true",
                   help="compile fully and compare only verdicts/errors (not ASTs)")
    p.add_argument("--impl", default=DEFAULT_IMPL,
                   help="implementation under test, run as CMD --dump-ast-json-batch")
    p.add_argument("--ref", default="inproc",
                   help="'inproc' (ref_dump_ast in the worker) or a reference command")
    p.add_argument("--lib-dir", type=Path, default=DEFAULT_LIB_DIR,
                   help=f"CPython Lib directory (default {DEFAULT_LIB_DIR})")
    p.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1)
    p.add_argument("--timeout", type=float, default=10.0, help="per-request timeout (s)")
    p.add_argument("--chunk-size", type=int, default=500,
                   help="max requests per worker task (reduced to balance small runs)")
    p.add_argument("--report", type=Path, default=DEFAULT_REPORT, help="JSON report path")
    p.add_argument("--min-lib-pct", type=float, default=100.0)
    p.add_argument("--min-corpus-pct", type=float, default=99.9)
    p.add_argument("--min-files-pct", type=float, default=100.0)
    p.add_argument("--min-syntax-pct", type=float, default=100.0)
    args = p.parse_args(argv)
    if not (args.lib or args.corpus or args.files or args.syntax_cases):
        p.error("select at least one input: --lib, --corpus, --files, --syntax-cases")
    if args.jobs < 1 or args.chunk_size < 1 or args.timeout <= 0:
        p.error("-j, --chunk-size and --timeout must be positive")
    return args


def run(args: argparse.Namespace) -> int:
    """Collect inputs, run the differential, report; return the exit status."""
    impl_argv = resolve_command(args.impl, "IUT")
    ref_argv = None if args.ref == "inproc" else resolve_command(args.ref, "reference")
    inputs = Inputs()
    sources: list[str] = []
    if args.lib:
        sources.append("lib")
        collect_lib(args.lib_dir, inputs, args.check)
    if args.corpus:
        sources.append("corpus")
        collect_corpus(args.corpus.expanduser(), inputs, args.check)
    if args.files:
        sources.append("files")
        collect_files(args.files, inputs, args.check)
    if args.syntax_cases:
        sources.append("syntax")
        collect_syntax_cases(args.lib_dir / "test", inputs)
    print(f"astdiff: impl={args.impl!r} ref={args.ref!r} check={args.check} "
          f"requests={len(inputs.cases)} skipped={len(inputs.skipped)} jobs={args.jobs}",
          flush=True)
    results = run_cases(inputs.cases, impl_argv, ref_argv, args.timeout, args.jobs,
                        args.chunk_size)
    thresholds = {"lib": args.min_lib_pct, "corpus": args.min_corpus_pct,
                  "files": args.min_files_pct, "syntax": args.min_syntax_pct}
    summary = summarize(sources, inputs, results, thresholds)
    print_report(summary, results)
    write_report(args.report, args, summary, inputs, results)
    print(f"report: {args.report}")
    return 0 if all(row["passed"] for row in summary.values()) else 1


def main(argv: list[str] | None = None) -> int:
    """Command-line entry point; harness errors become exit status 2."""
    args = parse_args(argv)
    try:
        return run(args)
    except HarnessError as exc:
        print(f"astdiff: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    # Re-enter through the importable module so worker processes can unpickle run_chunk
    # (and Case) as ``astdiff.*`` even when this file is started via runpy or as __main__.
    import astdiff

    sys.exit(astdiff.main())
