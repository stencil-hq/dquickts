#!/usr/bin/env python3.15
"""Single entry point of the dquickts Python test harness.

Runs under host CPython 3.15 (never under the implementation under test, the
"IUT").  Subcommands:

``golden-regen``
    Regenerate ``tests/py/golden/<group>/NNN_name.out`` with host CPython.
``l0``
    Run the golden scripts with the IUT and compare with the ``.out`` files;
    expected failures live in ``tests/py/l0_errors.txt``.
``l3``
    Run CPython ``Lib/test`` modules listed in ``tests/py/cpython.conf`` with
    the IUT (one process per module, via ``support/testpy_reporter.py``);
    expected failures live in ``tests/py/cpython_errors.txt``.
``ast`` / ``replay``
    Exec ``tests/py/astdiff.py`` / ``tests/py/replay.py`` with the remaining
    arguments.

Examples::

    python3.15 tests/py/run.py golden-regen
    python3.15 tests/py/run.py l0 --impl python3.15
    python3.15 tests/py/run.py l3 --impl python3.15 --set l3a test_int test_bool
    python3.15 tests/py/run.py l3 --impl ./qpy -u

Exit codes: 0 = no unexpected failures (or ``-u`` rewrote the errors file),
1 = unexpected failures, 2 = harness/setup error.
"""

import argparse
import concurrent.futures
import dataclasses
import difflib
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from collections.abc import Callable, Iterable, Iterator
from pathlib import Path
from typing import NoReturn

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
PY_DIR = REPO_ROOT / "tests" / "py"
GOLDEN_DIR = PY_DIR / "golden"
SUPPORT_DIR = PY_DIR / "support"
L0_ERRORS = PY_DIR / "l0_errors.txt"
L3_CONF = PY_DIR / "cpython.conf"
L3_ERRORS = PY_DIR / "cpython_errors.txt"
CPYTHON_TEST_DIR = REPO_ROOT / "cpython" / "Lib" / "test"
# pylib/test -> ../cpython/Lib/test (gitignored): in a CPython install `test`
# is part of the stdlib, so `qpy -I` subprocesses of the tests (script_helper)
# import it from there; PYTHONPATH (TESTROOT) serves the test runs themselves.
PYLIB_TEST_LINK = REPO_ROOT / "pylib" / "test"
PYLIB_TEST_TARGET = os.path.join(os.pardir, "cpython", "Lib", "test")
BUILD_DIR = REPO_ROOT / "build" / "testpy"
TESTROOT = BUILD_DIR / "testroot"
# test-only C extension modules (_testcapi, _testlimitedcapi, ...): `make
# capi-testmods`.  Like CPython's lib-dynload they are also importable by
# `qpy -I` subprocesses of the tests: pylib/<module>.so symlinks (gitignored)
CAPI_TESTMODS_DIR = BUILD_DIR / "capi-testmods"
L3_RESULTS = BUILD_DIR / "l3-results.json"

GOLDEN_GROUPS = ("syntax", "types", "objects", "stdlib")
L3_SETS = ("l3a", "l3b", "l3c")
L3_CONF_SECTIONS = (*L3_SETS, "exclude")
PASSING_OUTCOMES = frozenset({"pass", "skip", "xfail"})
DONE_ID = "__done__"
DIFF_LINES = 40
TOOL_SCRIPTS = {"ast": "astdiff.py", "replay": "replay.py"}

L0_ERRORS_HEADER = [
    "# Expected-failing L0 golden scripts: one `group/NNN_name` per line, sorted.",
]
L3_ERRORS_HEADER = [
    "# Expected-failing CPython Lib/test ids: one id per line, sorted.",
]


class HarnessError(Exception):
    """Setup or configuration problem: reported in one line, exit code 2."""


# ---------------------------------------------------------------------------
# Child processes


@dataclasses.dataclass
class ProcessResult:
    """Outcome of one child process; ``returncode`` is None after a timeout."""

    returncode: int | None
    stdout: bytes
    stderr: bytes
    seconds: float

    @property
    def timed_out(self) -> bool:
        """True if the process was killed because it hit the timeout."""
        return self.returncode is None


class _ProcessGroups:
    """Process groups of running children, so Ctrl-C can kill them all.

    Children run in their own session (``start_new_session=True``) and so do
    not receive the terminal's SIGINT themselves.
    """

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._pgids: set[int] = set()
        self._closed = False

    def add(self, pgid: int) -> None:
        """Track ``pgid``; kill it at once if shutdown already started."""
        with self._lock:
            if not self._closed:
                self._pgids.add(pgid)
                return
        kill_group(pgid)

    def discard(self, pgid: int) -> None:
        """Stop tracking ``pgid``."""
        with self._lock:
            self._pgids.discard(pgid)

    def kill_all(self) -> None:
        """Kill every tracked group and refuse to track new ones."""
        with self._lock:
            self._closed = True
            pgids = list(self._pgids)
        for pgid in pgids:
            kill_group(pgid)


_GROUPS = _ProcessGroups()


def kill_group(pgid: int) -> None:
    """SIGKILL process group ``pgid``, ignoring groups that are gone."""
    try:
        os.killpg(pgid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def run_process(cmd: list[str], *, env: dict[str, str], timeout: float,
                workdir: Path) -> ProcessResult:
    """Run ``cmd`` in a fresh ``workdir/cwd`` and capture its output.

    stdout/stderr go to files in ``workdir`` (so lingering grandchildren
    cannot block the harness on a pipe).  The child leads its own process
    group; on timeout, and after it exits, the whole group is killed.
    """
    cwd = workdir / "cwd"
    cwd.mkdir()
    out_path = workdir / "stdout"
    err_path = workdir / "stderr"
    start = time.monotonic()
    with open(out_path, "wb") as out, open(err_path, "wb") as err:
        try:
            proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                    stdout=out, stderr=err, start_new_session=True)
        except OSError as exc:
            raise HarnessError(f"cannot run {cmd[0]}: {exc}") from exc
        _GROUPS.add(proc.pid)
        try:
            try:
                returncode: int | None = proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                returncode = None
            kill_group(proc.pid)
            proc.wait()
        finally:
            _GROUPS.discard(proc.pid)
    return ProcessResult(returncode, out_path.read_bytes(), err_path.read_bytes(),
                         time.monotonic() - start)


def child_env(extra: dict[str, str]) -> dict[str, str]:
    """Return the harness environment for a child: ours without ``PYTHON*``
    variables (they would change how the IUT starts), plus ``extra``."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("PYTHON")}
    env.update(extra)
    return env


def resolve_command(spec: str, what: str) -> list[str]:
    """Split ``spec`` with shlex and make its executable an absolute path.

    A first element containing ``/`` is resolved against the repo root when
    relative; a bare name is looked up on PATH.  Raises HarnessError if the
    executable does not exist.
    """
    try:
        argv = shlex.split(spec)
    except ValueError as exc:
        raise HarnessError(f"cannot parse {what} command {spec!r}: {exc}") from exc
    if not argv:
        raise HarnessError(f"empty {what} command")
    exe = argv[0]
    if "/" in exe:
        path = Path(exe) if os.path.isabs(exe) else REPO_ROOT / exe
        if not (path.is_file() and os.access(path, os.X_OK)):
            raise HarnessError(f"{what} not found: {exe} (no executable at {path})")
        found = str(path)
    else:
        which = shutil.which(exe)
        if which is None:
            raise HarnessError(f"{what} not found: {exe} (not on PATH)")
        found = os.path.abspath(which)
    return [found, *argv[1:]]


def run_parallel[T, R](func: Callable[[T], R], items: Iterable[T],
                       jobs: int) -> Iterator[tuple[T, R]]:
    """Yield ``(item, func(item))`` in completion order, ``jobs`` at a time.

    If the consumer is interrupted, queued items are cancelled and every
    running child process group is killed.
    """
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=jobs)
    try:
        futures = {pool.submit(func, item): item for item in items}
        for future in concurrent.futures.as_completed(futures):
            yield futures[future], future.result()
    except BaseException:
        pool.shutdown(wait=False, cancel_futures=True)
        _GROUPS.kill_all()
        raise
    finally:
        pool.shutdown(wait=True)


def decode(data: bytes) -> str:
    """Decode child output as UTF-8, keeping undecodable bytes (surrogateescape)."""
    return data.decode("utf-8", "surrogateescape")


def last_line(text: str) -> str:
    """Return the last non-empty line of ``text`` (right-stripped), or ''."""
    for line in reversed(text.splitlines()):
        if line.strip():
            return line.rstrip()
    return ""


def positive_int(text: str) -> int:
    """argparse type: an integer >= 1."""
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError(f"must be >= 1: {text}")
    return value


# ---------------------------------------------------------------------------
# Expected-failure files and the summary


@dataclasses.dataclass
class ErrorsFile:
    """An expected-failures file: ``#`` header lines plus one id per line."""

    path: Path
    header: list[str]
    ids: set[str]

    @classmethod
    def load(cls, path: Path, default_header: list[str]) -> "ErrorsFile":
        """Parse ``path``; a missing file is empty with ``default_header``.

        The header is the leading run of comment and blank lines; ids may
        carry inline comments after whitespace + ``#``.
        """
        if not path.exists():
            return cls(path, list(default_header), set())
        header: list[str] = []
        ids: set[str] = set()
        for line in path.read_text(encoding="utf-8").splitlines():
            entry = strip_comment(line)
            if entry:
                ids.add(entry)
            elif not ids:
                header.append(line.rstrip())
        while header and not header[-1]:
            header.pop()
        return cls(path, header, ids)

    def save(self, ids: Iterable[str]) -> None:
        """Rewrite the file with the header and ``ids`` sorted."""
        lines = [*self.header, *sorted(set(ids))]
        self.path.write_text("".join(line + "\n" for line in lines), encoding="utf-8")


def strip_comment(line: str) -> str:
    """Drop a full-line ``#`` comment or an inline whitespace + ``#`` comment."""
    stripped = line.strip()
    if stripped.startswith("#"):
        return ""
    return re.sub(r"\s+#.*$", "", stripped)


@dataclasses.dataclass
class Comparison:
    """Current failing ids compared with the expected-failure list."""

    expected_failing: list[str]
    new_failures: list[str]
    new_passes: list[str]
    updated_ids: set[str]


def compare(listed: set[str], failing: set[str],
            in_scope: Callable[[str], bool]) -> Comparison:
    """Compare ``failing`` with ``listed``; only in-scope listed ids (those
    this run could have produced) can become new passes or be dropped."""
    return Comparison(
        expected_failing=sorted(failing & listed),
        new_failures=sorted(failing - listed),
        new_passes=sorted(i for i in listed if in_scope(i) and i not in failing),
        updated_ids={i for i in listed if not in_scope(i)} | failing,
    )


def finish(errors: ErrorsFile, cmp: Comparison, total: int, passed: int,
           update: bool, describe: Callable[[str], str]) -> int:
    """Print the summary, apply ``-u`` and return the exit code."""
    print(f"total {total}, passed {passed}, failing {total - passed} "
          f"({len(cmp.expected_failing)} expected, {len(cmp.new_failures)} new), "
          f"new passes {len(cmp.new_passes)}")
    if cmp.new_failures:
        print(f"NEW failures ({len(cmp.new_failures)}):")
        for test_id in cmp.new_failures:
            detail = describe(test_id)
            print(f"  {test_id}" + (f": {detail}" if detail else ""))
    if cmp.new_passes:
        print(f"NEW passes ({len(cmp.new_passes)}; `-u` removes them from "
              f"{errors.path.relative_to(REPO_ROOT)}):")
        for test_id in cmp.new_passes:
            print(f"  {test_id}")
    if update:
        errors.save(cmp.updated_ids)
        print(f"updated {errors.path.relative_to(REPO_ROOT)} "
              f"({len(cmp.updated_ids)} entries)")
        return 0
    return 1 if cmp.new_failures else 0


# ---------------------------------------------------------------------------
# L0 golden scripts


@dataclasses.dataclass(frozen=True)
class GoldenScript:
    """One ``tests/py/golden/<group>/NNN_name.py`` script."""

    group: str
    source: Path

    @property
    def id(self) -> str:
        """``group/NNN_name``, the id used in l0_errors.txt."""
        return f"{self.group}/{self.source.stem}"

    @property
    def expected_path(self) -> Path:
        """The ``.out`` file next to the script."""
        return self.source.with_suffix(".out")


def parse_groups(text: str) -> list[str]:
    """Parse ``--groups`` (comma list) and check each group directory exists."""
    groups = [g.strip() for g in text.split(",") if g.strip()]
    if not groups:
        raise HarnessError("--groups is empty")
    for group in groups:
        if not (GOLDEN_DIR / group).is_dir():
            raise HarnessError(f"golden group not found: {GOLDEN_DIR / group}")
    return groups


def discover_golden(groups: list[str]) -> list[GoldenScript]:
    """All golden scripts of ``groups``, sorted by id."""
    return [GoldenScript(group, path)
            for group in groups
            for path in sorted((GOLDEN_DIR / group).glob("*.py"))]


def run_golden(cmd: list[str], script: GoldenScript, timeout: float) -> ProcessResult:
    """Run ``script`` with ``cmd`` under the L0 execution rules."""
    env = child_env({"PYTHONHASHSEED": "0", "PYTHONIOENCODING": "utf-8",
                     "LC_ALL": "C.UTF-8"})
    with tempfile.TemporaryDirectory(prefix="testpy-l0-",
                                     ignore_cleanup_errors=True) as tmp:
        return run_process([*cmd, "-I", "-X", "utf8", str(script.source)],
                           env=env, timeout=timeout, workdir=Path(tmp))


def golden_output(result: ProcessResult) -> bytes:
    """The comparable output of a golden run.

    stdout, plus ``[exit N] <last non-empty stderr line>`` on its own line if
    the exit code is non-zero (``[timeout]`` if the run was killed).
    """
    if result.returncode == 0:
        return result.stdout
    if result.timed_out:
        marker = "[timeout]"
    else:
        marker = f"[exit {result.returncode}]"
        stderr_line = last_line(decode(result.stderr))
        if stderr_line:
            marker += " " + stderr_line
    out = result.stdout
    if out and not out.endswith(b"\n"):
        out += b"\n"
    return out + marker.encode("utf-8", "surrogateescape") + b"\n"


def _decode_lines(data: bytes) -> list[str]:
    return decode(data).splitlines(keepends=True)


def _show_line(line: str | None) -> str:
    return "<eof>" if line is None else repr(line.rstrip("\n"))[:70]


def mismatch_reason(expected: bytes, actual: bytes) -> str:
    """One-line description of the first differing line."""
    exp, act = _decode_lines(expected), _decode_lines(actual)
    for lineno in range(1, max(len(exp), len(act)) + 1):
        e = exp[lineno - 1] if lineno <= len(exp) else None
        a = act[lineno - 1] if lineno <= len(act) else None
        if e != a:
            return f"line {lineno}: expected {_show_line(e)}, got {_show_line(a)}"
    return "output differs"


def unified_diff(expected: bytes, actual: bytes) -> str:
    """The first DIFF_LINES lines of a unified diff, expected -> actual."""
    lines = list(difflib.unified_diff(_decode_lines(expected), _decode_lines(actual),
                                      "expected", "actual"))[:DIFF_LINES]
    return "".join(line if line.endswith("\n") else line + "\n" for line in lines)


def cmd_golden_regen(args: argparse.Namespace) -> int:
    """``golden-regen``: write every ``.out`` from host CPython."""
    python = resolve_command(args.python, "python")
    groups = parse_groups(args.groups)
    scripts = discover_golden(groups)
    updated = 0
    problems: list[str] = []
    for script, result in run_parallel(
            lambda s: run_golden(python, s, args.timeout), scripts, args.jobs):
        if result.timed_out:
            problems.append(f"{script.id}: timeout after {args.timeout}s")
            continue
        data = golden_output(result)
        path = script.expected_path
        if not path.exists() or path.read_bytes() != data:
            path.write_bytes(data)
            updated += 1
    removed = 0
    for group in groups:
        for out in sorted((GOLDEN_DIR / group).glob("*.out")):
            if not out.with_suffix(".py").exists():
                out.unlink()
                removed += 1
    print(f"golden-regen: {len(scripts)} scripts, {updated} .out written, "
          f"{removed} stale .out removed")
    for problem in sorted(problems):
        print(f"  ERROR {problem}")
    return 2 if problems else 0


def cmd_l0(args: argparse.Namespace) -> int:
    """``l0``: run golden scripts with the IUT and compare with ``.out``."""
    impl = resolve_command(args.impl, "IUT")
    scripts = discover_golden(parse_groups(args.groups))
    if args.patterns:
        scripts = [s for s in scripts if any(p in s.id for p in args.patterns)]
    if not scripts:
        raise HarnessError("no golden scripts selected")
    missing = [s.id for s in scripts if not s.expected_path.exists()]
    if missing:
        raise HarnessError(f"{len(missing)} golden script(s) have no .out file "
                           f"(first: {missing[0]}); run `run.py golden-regen` first")
    reasons: dict[str, str] = {}
    diffs: dict[str, str] = {}
    for script, result in run_parallel(
            lambda s: run_golden(impl, s, args.timeout), scripts, args.jobs):
        expected = script.expected_path.read_bytes()
        actual = golden_output(result)
        if actual != expected:
            reasons[script.id] = ("timeout" if result.timed_out
                                  else mismatch_reason(expected, actual))
            diffs[script.id] = unified_diff(expected, actual)
    errors = ErrorsFile.load(L0_ERRORS, L0_ERRORS_HEADER)
    run_ids = {s.id for s in scripts}
    cmp = compare(errors.ids, set(reasons), run_ids.__contains__)
    if args.verbose:
        for script_id in sorted(reasons):
            tag = "expected " if script_id in errors.ids else ""
            print(f"{tag}FAIL {script_id}: {reasons[script_id]}")
            print(diffs[script_id], end="")
    print(f"L0 ({' '.join(impl)}):")
    return finish(errors, cmp, len(scripts), len(scripts) - len(reasons),
                  args.update, lambda i: reasons.get(i, ""))


# ---------------------------------------------------------------------------
# L3 CPython Lib/test


def parse_conf(path: Path) -> dict[str, list[str]]:
    """Parse cpython.conf into ``{"l3a": [...], ..., "exclude": [...]}``."""
    if not path.exists():
        raise HarnessError(f"missing {path}")
    sections: dict[str, list[str]] = {name: [] for name in L3_CONF_SECTIONS}
    current: list[str] | None = None
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        entry = strip_comment(line)
        if not entry:
            continue
        if entry.startswith("[") and entry.endswith("]"):
            name = entry[1:-1].strip()
            if name not in sections:
                raise HarnessError(f"{path}:{lineno}: unknown section [{name}]")
            current = sections[name]
        elif current is None:
            raise HarnessError(f"{path}:{lineno}: entry outside a section: {entry}")
        elif entry not in current:
            current.append(entry)
    return sections


def ensure_symlink(link: Path, target: str) -> None:
    """Make ``link`` a symlink to ``target`` (replacing another symlink)."""
    if link.is_symlink():
        if os.readlink(link) == target:
            return
        link.unlink()
    elif link.exists():
        raise HarnessError(f"{link} exists and is not a symlink")
    link.symlink_to(target, target_is_directory=True)


def ensure_testroot() -> Path:
    """Make ``build/testpy/testroot/test`` and ``pylib/test`` symlinks to
    ``cpython/Lib/test``, and ``pylib/<module>.so`` symlinks to the built
    test-only C extension modules."""
    if not CPYTHON_TEST_DIR.is_dir():
        raise HarnessError(f"CPython test package not found at {CPYTHON_TEST_DIR} "
                           "(check out CPython v3.15.2 into cpython/)")
    TESTROOT.mkdir(parents=True, exist_ok=True)
    ensure_symlink(TESTROOT / "test", str(CPYTHON_TEST_DIR))
    ensure_symlink(PYLIB_TEST_LINK, PYLIB_TEST_TARGET)
    for so in CAPI_TESTMODS_DIR.glob("_test*.so"):
        ensure_symlink(PYLIB_TEST_LINK.parent / so.name,
                       os.path.relpath(so, PYLIB_TEST_LINK.parent))
    return TESTROOT


def module_of(test_id: str) -> str:
    """The Lib/test module (first dotted component) of a test id."""
    return test_id.split(".", 1)[0]


@dataclasses.dataclass
class ModuleRun:
    """Result of running one Lib/test module under the IUT."""

    module: str
    set_name: str
    status: str  # "ok" | "crash" | "timeout"
    returncode: int | None
    seconds: float
    message: str
    stderr_tail: str
    records: list[dict[str, str]]

    def failing_ids(self) -> set[str]:
        """Ids with at least one failing record."""
        return {r["id"] for r in self.records if r["outcome"] not in PASSING_OUTCOMES}


def read_results(path: Path, module: str) -> tuple[list[dict[str, str]], bool]:
    """Read a reporter JSON lines file; returns (records, saw ``__done__``).

    Unparsable lines (e.g. a line cut short by a crash) are ignored.
    """
    records: list[dict[str, str]] = []
    done = False
    if not path.exists():
        return records, done
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if not isinstance(obj, dict) or not isinstance(obj.get("id"), str):
            continue
        if obj["id"] == DONE_ID:
            done = True
            continue
        records.append({"id": obj["id"], "outcome": str(obj.get("outcome")),
                        "detail": str(obj.get("detail", "")), "module": module})
    return records, done


def run_l3_module(impl: list[str], module: str, set_name: str,
                  excluded: list[str], timeout: float) -> ModuleRun:
    """Run ``test.<module>`` through testpy_reporter with the IUT.

    A crash or timeout before the reporter's ``__done__`` marker adds a
    failing record for the module id.
    """
    with tempfile.TemporaryDirectory(prefix="testpy-l3-",
                                     ignore_cleanup_errors=True) as tmp:
        workdir = Path(tmp)
        results = workdir / "results.jsonl"
        env = child_env({
            "PYTHONPATH": os.pathsep.join([str(SUPPORT_DIR), str(TESTROOT),
                                           str(CAPI_TESTMODS_DIR)]),
            "TESTPY_RESULTS": str(results),
            "TESTPY_EXCLUDE": ",".join(excluded),
        })
        proc = run_process([*impl, "-X", "utf8", "-m", "testpy_reporter",
                            f"test.{module}"], env=env, timeout=timeout,
                           workdir=workdir)
        records, done = read_results(results, module)
    if done:
        status, message = "ok", ""
    elif proc.timed_out:
        status, message = "timeout", "timeout"
    else:
        status = "crash"
        message = "crash: " + (last_line(decode(proc.stderr))[:300]
                               or f"exit code {proc.returncode}")
    if status != "ok":
        records.append({"id": module, "outcome": "error", "detail": message,
                        "module": module})
    stderr_tail = proc.stderr[-2000:].decode("utf-8", "replace")
    return ModuleRun(module, set_name, status, proc.returncode, proc.seconds,
                     message, stderr_tail, records)


def select_l3_modules(args: argparse.Namespace,
                      conf: dict[str, list[str]]) -> tuple[list[str], dict[str, str]]:
    """Modules to run (positional MODULEs override ``--set``) and their sets."""
    set_of: dict[str, str] = {}
    for name in L3_SETS:
        for module in conf[name]:
            set_of.setdefault(module, name)
    if args.modules:
        wanted = [m.removeprefix("test.") for m in args.modules]
    else:
        names = L3_SETS if args.set == "all" else (args.set,)
        wanted = [m for name in names for m in conf[name]]
    modules = list(dict.fromkeys(wanted))
    return modules, {m: set_of.get(m, "other") for m in modules}


def cmd_l3(args: argparse.Namespace) -> int:
    """``l3``: run CPython Lib/test modules with the IUT."""
    impl = resolve_command(args.impl, "IUT")
    conf = parse_conf(L3_CONF)
    excluded = conf["exclude"]
    modules, set_of = select_l3_modules(args, conf)
    skipped = [m for m in modules if m in excluded]
    modules = [m for m in modules if m not in excluded]
    if skipped:
        print(f"excluded modules (not run): {' '.join(skipped)}")
    if not modules:
        raise HarnessError("no L3 modules selected")
    ensure_testroot()
    if not any(CAPI_TESTMODS_DIR.glob("_testlimitedcapi.*")):
        print(f"note: no _testlimitedcapi in {CAPI_TESTMODS_DIR} (make capi-testmods): "
              "test_bytes and the other modules that need it are skipped")

    runs: list[ModuleRun] = []
    jobs = run_parallel(
        lambda m: run_l3_module(impl, m, set_of[m], excluded, args.timeout),
        modules, args.jobs)
    for index, (module, run) in enumerate(jobs, 1):
        runs.append(run)
        line = (f"[{index}/{len(modules)}] {module:<32} {run.status:<7} "
                f"{len(run.records):5d} tests {len(run.failing_ids()):4d} failing "
                f"{run.seconds:7.1f}s")
        if run.message:
            line += f"  {run.message}"
        print(line, flush=True)
    runs.sort(key=lambda r: modules.index(r.module))

    errors = ErrorsFile.load(L3_ERRORS, L3_ERRORS_HEADER)
    # A crashed module did not report every test: its listed failures that
    # were not reported are carried forward as failing.
    for run in runs:
        if run.status == "ok":
            continue
        reported = {r["id"] for r in run.records}
        for test_id in sorted(errors.ids):
            if module_of(test_id) == run.module and test_id not in reported:
                run.records.append({"id": test_id, "outcome": "error",
                                    "detail": run.status, "module": run.module})

    failing: set[str] = set()
    test_module: dict[str, str] = {}
    details: dict[str, str] = {}
    for run in runs:
        for record in run.records:
            test_module.setdefault(record["id"], run.module)
            if record["outcome"] not in PASSING_OUTCOMES:
                failing.add(record["id"])
                details.setdefault(record["id"], record["detail"])

    full_run = not args.modules and args.set == "all"
    run_modules = {r.module for r in runs}
    cmp = compare(errors.ids, failing,
                  lambda i: full_run or i in test_module or module_of(i) in run_modules)

    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    L3_RESULTS.write_text(json.dumps({
        "impl": impl,
        "modules": [{k: v for k, v in dataclasses.asdict(r).items() if k != "records"}
                    for r in runs],
        "records": [rec for r in runs for rec in r.records],
    }, indent=1) + "\n", encoding="utf-8")

    print(f"L3 ({' '.join(impl)}):")
    new_failures = set(cmp.new_failures)
    for set_name in (*L3_SETS, "other"):
        set_runs = [r for r in runs if r.set_name == set_name]
        if not set_runs:
            continue
        ids = {i for i, m in test_module.items() if set_of[m] == set_name}
        set_failing = ids & failing
        broken = sum(r.status != "ok" for r in set_runs)
        print(f"  {set_name}: {len(set_runs)} modules ({broken} crashed/timed out), "
              f"{len(ids)} tests, {len(ids) - len(set_failing)} passed, "
              f"{len(set_failing)} failing ({len(set_failing & new_failures)} new)")
    if args.verbose:
        for test_id in cmp.new_failures:
            print(f"--- {test_id}\n{details[test_id]}")
    print(f"records: {L3_RESULTS.relative_to(REPO_ROOT)}")
    passed = len(test_module) - len(failing)

    def describe(test_id: str) -> str:
        return last_line(details.get(test_id, ""))[:120]

    return finish(errors, cmp, len(test_module), passed, args.update, describe)


# ---------------------------------------------------------------------------
# Tool passthrough and command line


def exec_tool(script_name: str, args: list[str]) -> NoReturn:
    """Replace this process with ``tests/py/<script_name> ARGS...``."""
    script = PY_DIR / script_name
    if not script.is_file():
        raise HarnessError(f"missing {script}")
    sys.stdout.flush()
    sys.stderr.flush()
    os.execv(sys.executable, [sys.executable, str(script), *args])


def build_parser() -> argparse.ArgumentParser:
    """The argparse parser for all subcommands."""
    parser = argparse.ArgumentParser(
        prog="run.py", description="dquickts Python test harness",
        epilog="exit codes: 0 ok, 1 unexpected failures, 2 harness error")
    sub = parser.add_subparsers(dest="command", required=True)
    jobs_default = os.cpu_count() or 1

    regen = sub.add_parser("golden-regen", help="regenerate golden .out files")
    regen.add_argument("--python", default="python3.15",
                       help="reference interpreter (default: python3.15)")
    regen.add_argument("--groups", default=",".join(GOLDEN_GROUPS),
                       help="comma-separated golden groups (default: all)")
    regen.add_argument("-j", "--jobs", type=positive_int, default=jobs_default)
    regen.add_argument("--timeout", type=float, default=10.0,
                       help="seconds per script (default: 10)")
    regen.set_defaults(func=cmd_golden_regen)

    def add_iut_options(p: argparse.ArgumentParser, timeout: float) -> None:
        p.add_argument("--impl", default="./qpy",
                       help="IUT command, shlex-split (default: ./qpy)")
        p.add_argument("-u", "--update", action="store_true",
                       help="rewrite the expected-failures file and exit 0")
        p.add_argument("-j", "--jobs", type=positive_int, default=jobs_default)
        p.add_argument("--timeout", type=float, default=timeout,
                       help=f"seconds per process (default: {timeout:g})")
        p.add_argument("-v", "--verbose", action="store_true",
                       help="print failure details")

    l0 = sub.add_parser("l0", help="run golden scripts with the IUT")
    add_iut_options(l0, 10.0)
    l0.add_argument("--groups", default=",".join(GOLDEN_GROUPS),
                    help="comma-separated golden groups (default: all)")
    l0.add_argument("patterns", nargs="*", metavar="PATTERN",
                    help="only scripts whose group/name contains PATTERN")
    l0.set_defaults(func=cmd_l0)

    l3 = sub.add_parser("l3", help="run CPython Lib/test modules with the IUT")
    add_iut_options(l3, 300.0)
    l3.add_argument("--set", choices=(*L3_SETS, "all"), default="all",
                    help="module set from cpython.conf (default: all)")
    l3.add_argument("modules", nargs="*", metavar="MODULE",
                    help="run these modules instead of --set")
    l3.set_defaults(func=cmd_l3)

    for name, script in TOOL_SCRIPTS.items():
        tool = sub.add_parser(name, help=f"exec tests/py/{script} ARGS...")
        tool.add_argument("args", nargs=argparse.REMAINDER)
    return parser


def main(argv: list[str] | None = None) -> int:
    """Command-line entry point; returns the exit code."""
    argv = list(sys.argv[1:] if argv is None else argv)
    # Child output may contain undecodable bytes (kept as surrogates).
    sys.stdout.reconfigure(errors="backslashreplace")
    try:
        if argv and argv[0] in TOOL_SCRIPTS:
            exec_tool(TOOL_SCRIPTS[argv[0]], argv[1:])
        args = build_parser().parse_args(argv)
        return args.func(args)
    except HarnessError as exc:
        print(f"run.py: error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        _GROUPS.kill_all()
        print("run.py: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
