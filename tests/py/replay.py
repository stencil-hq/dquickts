"""L1 corpus differential replay: run omp kernel cells under the IUT and CPython.

Cells extracted by ``tests/py/corpus.py extract`` are replayed in two kernels
(``tests/py/kernel.py``) side by side: one under the implementation under test
(``--impl``, default ``./qpy``) and one under the reference CPython (``--ref``).
Each kernel gets its own fresh scratch working directory and private TMPDIR;
on macOS both run under ``sandbox-exec`` with ``tests/py/sandbox.sb``, which
denies file writes outside those directories and all non-unix network access.

Modes:

``sessions``
    Replay each replayable session (or the replayable prefix of a session)
    cell by cell in one persistent kernel pair, so session state carries over.
``selfcontained``
    Replay each self-contained, side-effect-free cell in a fresh kernel pair.

Per cell the responses are compared after replacing, on each side, strings
that are environmental rather than behavioural with placeholders:

- the kernel's scratch and TMP paths (``<SCRATCH>`` / ``<TMP>``);
- ``sys.version`` (``<SYS.VERSION>``): it embeds the build date and compiler;
- ``sys.executable`` (``<EXECUTABLE>``): where each interpreter is installed;
- the stdlib directory, i.e. the directory of ``os.py`` and its realpath
  (``<STDLIB>``): ``pylib/`` vs. the CPython install's ``lib/python3.15``,
  so module ``__file__`` paths compare equal.

The kernel reports the last three in its ``info`` reply at start; the
strings of both interpreters are replaced in both outputs (a cell may run
either interpreter by path).  Both
kernels also hide third-party packages (``kernel.drop_site_packages``): the
packages pip-installed for each interpreter are host state, so ``sys.path``
ends before the first site-packages directory on both sides.  Then
``stdout``, ``display`` and the exception type must be equal; a differing
exception message alone is "soft".
After the first divergence in a session the remaining cells still run but
are reported as "after-divergence" and excluded from the match percentage.
A kernel crash or timeout restarts both kernels and counts as a divergence.

A cell on which both kernels time out is "both-timeout": it is excluded from
the match percentage (own counter), both kernels are restarted, and the rest
of the session is after-divergence because the session state was lost.

``--calibrate`` runs the reference against itself; cells whose two reference
runs differ (reason ``output``) or both time out (reason ``timeout``) are added
to ``<corpus>/nondeterministic.json`` and skipped (still executed, not
compared) in normal runs.

The full report is written to ``build/testpy/replay-report.json``
(``replay-calibrate-report.json`` with ``--calibrate``).  Exit status: 0 if the
match percentage is at least ``--min-match-pct`` (default 0: report only),
1 otherwise, 2 on setup errors.

Usage::

    python3.15 tests/py/corpus.py extract
    python3.15 tests/py/replay.py --impl python3.15 --calibrate --limit 200
    python3.15 tests/py/replay.py --impl python3.15 --mode sessions --limit 50
    python3.15 tests/py/replay.py --mode selfcontained -j 8 --min-match-pct 95
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import contextlib
import dataclasses
import datetime
import difflib
import json
import os
import random
import select
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
KERNEL_SCRIPT = REPO_ROOT / "tests" / "py" / "kernel.py"
SANDBOX_PROFILE = REPO_ROOT / "tests" / "py" / "sandbox.sb"
REPORT_DIR = REPO_ROOT / "build" / "testpy"
DEFAULT_CORPUS = "~/.cache/dquickts/corpus"
NONDETERMINISTIC_FILE = "nondeterministic.json"

#: Cell statuses that count as compared-and-failed, in reporting order.
MISMATCH_STATUSES = ("stdout", "display", "exc_type", "crash", "timeout")
#: Both sides timed out: not compared, but the kernels were restarted.
BOTH_TIMEOUT = "both-timeout"
#: Statuses that count as compared-and-passed.
PASS_STATUSES = ("match", "soft")
#: Seconds allowed for a kernel to start and answer its first request.
STARTUP_TIMEOUT = 60.0
#: Mismatches listed in the console summary.
SUMMARY_MISMATCHES = 20
#: Cap on response text stored per side in the JSON report.
REPORT_TEXT_LIMIT = 2000


class SetupError(Exception):
    """Harness/setup problem; reported as one line, exit status 2."""


class KernelCrash(Exception):
    """The kernel process died, closed its pipe or answered garbage."""


class KernelTimeout(Exception):
    """The kernel did not answer before the deadline."""


# --------------------------------------------------------------------------
# Corpus
# --------------------------------------------------------------------------

@dataclasses.dataclass(slots=True)
class Corpus:
    """Extracted corpus: sources by id, sessions, self-contained ids, skip list."""

    path: Path
    sources: dict[str, str]
    sessions: list[dict[str, Any]]
    selfcontained: list[str]
    #: Cell id -> reason (``output`` or ``timeout``) for cells skipped in comparison.
    nondeterministic: dict[str, str]


@dataclasses.dataclass(slots=True)
class Item:
    """One unit of replay: a sequence of cells run in one kernel pair."""

    name: str
    cell_ids: list[str]


def _read_jsonl(path: Path) -> list[dict[str, Any]]:
    with path.open(encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def load_corpus(path: Path) -> Corpus:
    """Load an extracted corpus directory; raises ``SetupError`` if incomplete."""
    needed = [path / name for name in ("cells.jsonl", "sessions.jsonl", "selfcontained.jsonl")]
    missing = [p.name for p in needed if not p.is_file()]
    if missing:
        raise SetupError(
            f"no corpus in {path} (missing {', '.join(missing)}); "
            f"run `python3.15 tests/py/corpus.py extract` first"
        )
    sources = {rec["id"]: rec["source"] for rec in _read_jsonl(needed[0])}
    nondeterministic: dict[str, str] = {}
    nd_path = path / NONDETERMINISTIC_FILE
    if nd_path.is_file():
        for entry in json.loads(nd_path.read_text(encoding="utf-8")).get("cells", []):
            nondeterministic[entry["id"]] = entry["reason"]
    return Corpus(
        path=path,
        sources=sources,
        sessions=_read_jsonl(needed[1]),
        selfcontained=[rec["id"] for rec in _read_jsonl(needed[2])],
        nondeterministic=nondeterministic,
    )


def select_items(corpus: Corpus, mode: str, limit: int | None, seed: int | None) -> list[Item]:
    """Build the replay items for ``mode``, optionally shuffled by ``seed`` and limited."""
    if mode == "sessions":
        items = [
            Item(rec["session"], [cell["id"] for cell in rec["cells"][: rec["prefix_len"]]])
            for rec in corpus.sessions
            if rec["prefix_len"] > 0
        ]
    else:
        items = [Item(f"cell-{cid}", [cid]) for cid in corpus.selfcontained]
    if seed is not None:
        random.Random(seed).shuffle(items)
    if limit is not None:
        items = items[:limit]
    return items


# --------------------------------------------------------------------------
# Kernel processes
# --------------------------------------------------------------------------

def resolve_command(cmd: str, what: str) -> list[str]:
    """Split ``cmd`` with shlex and resolve its executable (relative paths against the repo root)."""
    argv = shlex.split(cmd)
    if not argv:
        raise SetupError(f"empty {what} command")
    exe = argv[0]
    if os.sep in exe:
        path = Path(exe) if os.path.isabs(exe) else REPO_ROOT / exe
        if not (path.is_file() and os.access(path, os.X_OK)):
            raise SetupError(f"{what} not found or not executable: {path}")
        argv[0] = str(path)
    else:
        found = shutil.which(exe)
        if found is None:
            raise SetupError(f"{what} not found on PATH: {exe}")
        argv[0] = found
    return argv


_LIVE_KERNELS: set[Kernel] = set()
_LIVE_LOCK = threading.Lock()


class Kernel:
    """One ``kernel.py`` process with its own scratch cwd and private TMPDIR."""

    def __init__(self, label: str, argv: list[str], workdir: Path, sandbox: bool):
        self.label = label
        self.argv = argv
        self.sandbox = sandbox
        scratch = workdir / "scratch"
        tmp = workdir / "tmp"
        scratch.mkdir(parents=True)
        tmp.mkdir(parents=True)
        # Real paths: the sandbox matches resolved paths (/private/var/..., not /var/...).
        self.scratch = os.path.realpath(scratch)
        self.tmp = os.path.realpath(tmp)
        self.info: dict[str, Any] = {}
        self._replacements = self._path_replacements()
        self.proc: subprocess.Popen[bytes] | None = None
        self._buf = bytearray()
        self._stderr: collections.deque[str] = collections.deque(maxlen=40)

    def normalize(self, text: str | None) -> str | None:
        """Replace this kernel's scratch/TMP paths and interpreter strings with placeholders."""
        if text is None:
            return None
        for path, placeholder in self._replacements:
            text = text.replace(path, placeholder)
        return text

    def command(self) -> list[str]:
        """Full argv, wrapped in ``sandbox-exec`` when sandboxing."""
        cmd = [*self.argv, str(KERNEL_SCRIPT)]
        if self.sandbox:
            cmd = ["sandbox-exec", "-f", str(SANDBOX_PROFILE),
                   "-D", f"SCRATCH={self.scratch}", "-D", f"TMP={self.tmp}", *cmd]
        return cmd

    def start(self) -> None:
        """Start the process and wait for it to answer a ``reset`` request."""
        env = dict(os.environ, TMPDIR=self.tmp, PYTHONHASHSEED="0",
                   PYTHONIOENCODING="utf-8", PYTHONDONTWRITEBYTECODE="1")
        self._buf.clear()
        self._stderr.clear()
        try:
            self.proc = subprocess.Popen(
                self.command(), cwd=self.scratch, env=env, bufsize=0,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                start_new_session=True,
            )
        except OSError as e:
            raise KernelCrash(f"cannot start kernel: {e}") from e
        with _LIVE_LOCK:
            _LIVE_KERNELS.add(self)
        threading.Thread(target=self._drain_stderr, args=(self.proc,), daemon=True).start()
        try:
            self.send({"op": "reset"})
            response = self.receive(time.monotonic() + STARTUP_TIMEOUT)
        except (KernelCrash, KernelTimeout) as e:
            self.stop()
            raise KernelCrash(f"kernel failed to start ({e}): {self.stderr_tail()}") from e
        if response != {"ok": True}:
            self.stop()
            raise KernelCrash(f"kernel failed to start: unexpected reply {response!r}")
        try:
            self.send({"op": "info"})
            info = self.receive(time.monotonic() + STARTUP_TIMEOUT)
        except (KernelCrash, KernelTimeout) as e:
            self.stop()
            raise KernelCrash(f"kernel info request failed ({e}): {self.stderr_tail()}") from e
        self.info = info if isinstance(info, dict) else {}
        self._replacements = self._path_replacements(self.info)

    def share_info(self, other: Kernel) -> None:
        """Also replace the other kernel's interpreter strings (a cell may run
        either interpreter by path, e.g. a hardcoded python3.15 under the IUT)."""
        merged = {key: [] for key in ("version", "executable", "stdlib")}
        for info in (self.info, other.info):
            for key in ("version", "executable"):
                if isinstance(info.get(key), str):
                    merged[key].append(info[key])
            merged["stdlib"].extend(info.get("stdlib") or ())
        self._replacements = self._path_replacements(merged)

    def _path_replacements(self, info: dict[str, Any] | None = None) -> list[tuple[str, str]]:
        """(text, placeholder) pairs, longest text first.

        Scratch/TMP paths always; with the kernel's ``info`` reply also the
        interpreter strings that differ between hosts and builds: the full
        ``sys.version`` (build date, compiler), ``sys.executable`` and the
        stdlib directory (the directory of ``os.py`` and its realpath).
        """
        pairs = []
        for real, placeholder in ((self.scratch, "<SCRATCH>"), (self.tmp, "<TMP>")):
            pairs.append((real, placeholder))
            if real.startswith("/private/"):
                pairs.append((real[len("/private"):], placeholder))
        if info:
            for key, placeholder in (("version", "<SYS.VERSION>"), ("executable", "<EXECUTABLE>")):
                values = info.get(key)
                for value in values if isinstance(values, list) else [values]:
                    if isinstance(value, str) and value:
                        pairs.append((value, placeholder))
            for path in info.get("stdlib") or ():
                if isinstance(path, str) and len(path) > 1:
                    pairs.append((path, "<STDLIB>"))
        return sorted(pairs, key=lambda pair: len(pair[0]), reverse=True)

    def _drain_stderr(self, proc: subprocess.Popen[bytes]) -> None:
        for line in proc.stderr:
            self._stderr.append(line.decode("utf-8", "replace").rstrip("\n"))

    def stderr_tail(self, lines: int = 5) -> str:
        """Last lines the kernel process wrote to its own stderr (outside cells)."""
        return " | ".join(list(self._stderr)[-lines:]) or "(no stderr)"

    def send(self, request: dict[str, Any]) -> None:
        """Write one request line."""
        assert self.proc is not None and self.proc.stdin is not None
        try:
            self.proc.stdin.write((json.dumps(request) + "\n").encode("utf-8", "surrogatepass"))
        except (BrokenPipeError, OSError) as e:
            raise KernelCrash(f"cannot write request: {e}") from e

    def receive(self, deadline: float) -> dict[str, Any]:
        """Read one response line before ``deadline`` (``time.monotonic()``)."""
        assert self.proc is not None and self.proc.stdout is not None
        fd = self.proc.stdout.fileno()
        while (newline := self._buf.find(b"\n")) < 0:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise KernelTimeout("no response before the deadline")
            ready, _, _ = select.select([fd], [], [], remaining)
            if not ready:
                continue
            chunk = os.read(fd, 1 << 16)
            if not chunk:
                code = self.proc.poll()
                raise KernelCrash(f"kernel exited (status {code}): {self.stderr_tail()}")
            self._buf += chunk
        line = bytes(self._buf[:newline])
        del self._buf[: newline + 1]
        try:
            response = json.loads(line)
        except ValueError as e:
            raise KernelCrash(f"bad response line {line[:200]!r}") from e
        if not isinstance(response, dict):
            raise KernelCrash(f"bad response {response!r:.200}")
        return response

    def stop(self) -> None:
        """Ask the kernel to quit, then kill its whole process group."""
        proc = self.proc
        if proc is None:
            return
        self.proc = None
        if proc.poll() is None:
            with contextlib.suppress(OSError, ValueError):
                proc.stdin.write(b'{"op": "quit"}\n')
            with contextlib.suppress(subprocess.TimeoutExpired):
                proc.wait(timeout=2)
        with contextlib.suppress(ProcessLookupError, PermissionError):
            os.killpg(proc.pid, signal.SIGKILL)
        with contextlib.suppress(subprocess.TimeoutExpired):
            proc.wait(timeout=5)
        for stream in (proc.stdin, proc.stdout):
            with contextlib.suppress(OSError):
                stream.close()
        with _LIVE_LOCK:
            _LIVE_KERNELS.discard(self)


def kill_all_kernels() -> None:
    """Stop every kernel still running (used on interrupt/exit)."""
    with _LIVE_LOCK:
        kernels = list(_LIVE_KERNELS)
    for kernel in kernels:
        kernel.stop()


@dataclasses.dataclass(slots=True)
class Outcome:
    """One side's result for a cell: ``ok`` with a response, ``crash`` or ``timeout``."""

    kind: str
    response: dict[str, Any] | None = None
    detail: str = ""


def exec_pair(a: Kernel, b: Kernel, source: str, timeout: float) -> tuple[Outcome, Outcome]:
    """Run ``source`` in both kernels concurrently and collect both outcomes."""
    request = {"op": "exec", "code": source}
    outcomes: list[Outcome | None] = [None, None]
    kernels = (a, b)
    for i, kernel in enumerate(kernels):
        try:
            kernel.send(request)
        except KernelCrash as e:
            outcomes[i] = Outcome("crash", detail=str(e))
    deadline = time.monotonic() + timeout
    for i, kernel in enumerate(kernels):
        if outcomes[i] is not None:
            continue
        try:
            outcomes[i] = Outcome("ok", kernel.receive(deadline))
        except KernelTimeout as e:
            outcomes[i] = Outcome("timeout", detail=str(e))
        except KernelCrash as e:
            outcomes[i] = Outcome("crash", detail=str(e))
    return outcomes[0], outcomes[1]


# --------------------------------------------------------------------------
# Comparison
# --------------------------------------------------------------------------

def normalized(kernel: Kernel, response: dict[str, Any]) -> dict[str, Any]:
    """The compared fields of a response, with kernel-specific paths replaced."""
    exc = response.get("exc")
    if exc is not None:
        exc = {"type": exc.get("type"), "msg": kernel.normalize(exc.get("msg"))}
    return {
        "stdout": kernel.normalize(response.get("stdout")),
        "display": kernel.normalize(response.get("display")),
        "exc": exc,
    }


def compare(a: dict[str, Any], b: dict[str, Any]) -> tuple[str, list[str]]:
    """Classify two normalized responses: ``(status, differing fields)``.

    Status is ``match``, ``soft`` (only the exception message differs) or the
    first differing field among ``exc_type``, ``stdout``, ``display``.
    """
    a_exc, b_exc = a["exc"], b["exc"]
    fields = []
    if (a_exc and a_exc["type"]) != (b_exc and b_exc["type"]):
        fields.append("exc_type")
    if a["stdout"] != b["stdout"]:
        fields.append("stdout")
    if a["display"] != b["display"]:
        fields.append("display")
    if fields:
        return fields[0], fields
    if a_exc and b_exc and a_exc["msg"] != b_exc["msg"]:
        return "soft", []
    return "match", []


def _clip(text: str | None, limit: int = REPORT_TEXT_LIMIT) -> str | None:
    if text is None or len(text) <= limit:
        return text
    return text[:limit] + f"... [{len(text) - limit} more chars]"


def _report_side(outcome: Outcome, norm: dict[str, Any] | None) -> dict[str, Any]:
    if norm is None:
        return {"kind": outcome.kind, "detail": outcome.detail}
    exc = norm["exc"]
    return {
        "kind": outcome.kind,
        "stdout": _clip(norm["stdout"]),
        "display": _clip(norm["display"]),
        "exc": exc and {"type": exc["type"], "msg": _clip(exc["msg"], 500)},
        "stderr": _clip(outcome.response.get("stderr"), 1000),
    }


# --------------------------------------------------------------------------
# Replaying one item
# --------------------------------------------------------------------------

@dataclasses.dataclass(slots=True)
class ItemResult:
    """Per-item counts by status plus the non-matching cell records."""

    name: str
    cells: int
    counts: collections.Counter[str]
    events: list[dict[str, Any]]
    ms: dict[str, float]


@dataclasses.dataclass(slots=True)
class ReplayConfig:
    """Settings shared by every item of a run."""

    a_label: str
    a_argv: list[str]
    b_label: str
    b_argv: list[str]
    sandbox: bool
    timeout: float
    calibrate: bool
    workroot: Path


def replay_item(index: int, item: Item, corpus: Corpus, cfg: ReplayConfig) -> ItemResult:
    """Replay one item's cells in a fresh kernel pair and classify every cell."""
    workdir = cfg.workroot / f"{index:06d}"
    a = Kernel(cfg.a_label, cfg.a_argv, workdir / cfg.a_label, cfg.sandbox)
    b = Kernel(cfg.b_label, cfg.b_argv, workdir / cfg.b_label, cfg.sandbox)
    result = ItemResult(item.name, len(item.cell_ids), collections.Counter(), [],
                        {cfg.a_label: 0.0, cfg.b_label: 0.0})
    diverged = False
    try:
        start_error = _start_pair(a, b)
        for pos, cid in enumerate(item.cell_ids):
            event: dict[str, Any] = {"index": pos, "id": cid}
            if start_error is not None:
                status = "after-divergence" if diverged and not cfg.calibrate else "crash"
                event.update(status=status, raw_status="crash", detail=start_error)
                result.counts[status] += 1
                result.events.append(event)
                diverged = True
                continue
            oa, ob = exec_pair(a, b, corpus.sources[cid], cfg.timeout)
            if oa.kind != "ok" or ob.kind != "ok":
                if oa.kind == ob.kind == "timeout":
                    raw = BOTH_TIMEOUT
                elif "timeout" in (oa.kind, ob.kind):
                    raw = "timeout"
                else:
                    raw = "crash"
                event.update(a=_report_side(oa, None) if oa.kind != "ok" else {"kind": "ok"},
                             b=_report_side(ob, None) if ob.kind != "ok" else {"kind": "ok"})
                start_error = _restart_pair(a, b)
                status = raw
            else:
                for label, outcome in ((cfg.a_label, oa), (cfg.b_label, ob)):
                    result.ms[label] += float(outcome.response.get("ms") or 0.0)
                if not cfg.calibrate and cid in corpus.nondeterministic:
                    result.counts["nondeterministic"] += 1
                    continue
                na, nb = normalized(a, oa.response), normalized(b, ob.response)
                raw, fields = compare(na, nb)
                if raw != "match":
                    event.update(fields=fields, a=_report_side(oa, na), b=_report_side(ob, nb))
                status = raw
            if diverged and not cfg.calibrate:
                event = {"index": pos, "id": cid, "raw_status": raw}
                status = "after-divergence"
            elif status not in PASS_STATUSES:
                diverged = True
            result.counts[status] += 1
            if status != "match":
                event["status"] = status
                result.events.append(event)
    finally:
        a.stop()
        b.stop()
        shutil.rmtree(workdir, ignore_errors=True)
    return result


def _start_pair(a: Kernel, b: Kernel) -> str | None:
    """Start both kernels; returns an error description instead of raising."""
    try:
        a.start()
        b.start()
    except KernelCrash as e:
        a.stop()
        b.stop()
        return str(e)
    a.share_info(b)
    b.share_info(a)
    return None


def _restart_pair(a: Kernel, b: Kernel) -> str | None:
    """Stop and restart both kernels (same scratch dirs) after a crash/timeout."""
    a.stop()
    b.stop()
    return _start_pair(a, b)


# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------

def summarize(results: list[ItemResult]) -> dict[str, Any]:
    """Aggregate per-item counts into run totals and the match percentage."""
    counts: collections.Counter[str] = collections.Counter()
    ms: collections.Counter[str] = collections.Counter()
    for res in results:
        counts.update(res.counts)
        ms.update(res.ms)
    mismatched = {status: counts[status] for status in MISMATCH_STATUSES}
    compared = counts["match"] + counts["soft"] + sum(mismatched.values())
    passed = counts["match"] + counts["soft"]
    return {
        "items": len(results),
        "cells": sum(res.cells for res in results),
        "compared": compared,
        "matched": counts["match"],
        "soft": counts["soft"],
        "mismatched": mismatched,
        "after_divergence": counts["after-divergence"],
        "nondeterministic_skipped": counts["nondeterministic"],
        "both_timeout": counts[BOTH_TIMEOUT],
        "match_pct": round(100.0 * passed / compared, 3) if compared else 100.0,
        "ms": {label: round(value, 1) for label, value in ms.items()},
    }


def _short_diff(event: dict[str, Any], a_label: str, b_label: str) -> list[str]:
    """A few lines showing how the two sides of a mismatch differ."""
    a, b = event.get("a", {}), event.get("b", {})
    status = event["status"]
    if status in ("crash", "timeout") or "detail" in event:
        lines = [f"{a_label}: {a.get('kind', '?')} {a.get('detail', '')}".rstrip(),
                 f"{b_label}: {b.get('kind', '?')} {b.get('detail', '')}".rstrip()]
        if "detail" in event:
            lines = [event["detail"]]
        return [line[:300] for line in lines]
    if status == "stdout":
        diff = difflib.unified_diff(
            (b.get("stdout") or "").splitlines(), (a.get("stdout") or "").splitlines(),
            b_label, a_label, n=1, lineterm="",
        )
        lines = [line[:200] for line in diff]
        return lines[:12] + ([f"... ({len(lines) - 12} more diff lines)"] if len(lines) > 12 else [])
    if status == "display":
        return [f"{b_label}: {str(b.get('display'))[:200]}", f"{a_label}: {str(a.get('display'))[:200]}"]

    def exc_text(side: dict[str, Any]) -> str:
        exc = side.get("exc")
        return f"{exc['type']}: {exc['msg']}"[:200] if exc else "no exception"

    return [f"{b_label}: {exc_text(b)}", f"{a_label}: {exc_text(a)}"]


def print_summary(totals: dict[str, Any], results: list[ItemResult], corpus: Corpus,
                  cfg: ReplayConfig, mode: str, report_path: Path) -> None:
    """Print totals and the first mismatches with short diffs."""
    unit = "sessions" if mode == "sessions" else "cells (fresh kernel each)"
    print(f"replay {mode}: {cfg.a_label}={shlex.join(cfg.a_argv)} vs {cfg.b_label}={shlex.join(cfg.b_argv)}"
          f" ({'sandboxed' if cfg.sandbox else 'NOT sandboxed'})")
    print(f"  {unit}: {totals['items']}, cells: {totals['cells']}")
    print(f"  compared: {totals['compared']}, matched: {totals['matched']}, soft: {totals['soft']}")
    mismatched = ", ".join(f"{k} {v}" for k, v in totals["mismatched"].items())
    print(f"  mismatched: {sum(totals['mismatched'].values())} ({mismatched})")
    print(f"  after-divergence: {totals['after_divergence']}, "
          f"nondeterministic skipped: {totals['nondeterministic_skipped']}, "
          f"both timed out: {totals['both_timeout']}")
    ms = ", ".join(f"{label} {value / 1000:.1f}s" for label, value in totals["ms"].items())
    print(f"  cell time: {ms}")
    print(f"  match: {totals['match_pct']:.2f}%")

    shown = 0
    for res in results:
        for event in res.events:
            if event["status"] not in MISMATCH_STATUSES:
                continue
            if shown == SUMMARY_MISMATCHES:
                print(f"  ... more mismatches in {report_path}")
                return
            shown += 1
            first_line = next((ln.strip() for ln in corpus.sources[event["id"]].splitlines() if ln.strip()), "")
            print(f"\n[{event['status']}] {res.name} cell {event['index']} ({event['id']}): {first_line[:80]}")
            for line in _short_diff(event, cfg.a_label, cfg.b_label):
                print(f"    {line}")


def write_json(path: Path, value: Any) -> None:
    """Write pretty JSON, creating the parent directory."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=1) + "\n", encoding="utf-8")


def update_nondeterministic(corpus: Corpus, results: list[ItemResult], ref_argv: list[str]) -> int:
    """Merge cells that differed between two reference runs into the skip list.

    Each entry records its reason: ``timeout`` if both reference runs timed
    out, ``output`` for any other difference.  Returns the number of newly
    added cell ids.
    """
    # Kernel start failures ("detail") say nothing about the cell itself.
    found = {
        event["id"]: "timeout" if event["status"] == BOTH_TIMEOUT else "output"
        for res in results for event in res.events
        if event["status"] not in PASS_STATUSES and "detail" not in event
    }
    added = found.keys() - corpus.nondeterministic.keys()
    merged = corpus.nondeterministic | found
    write_json(corpus.path / NONDETERMINISTIC_FILE, {
        "cells": [{"id": cid, "reason": merged[cid]} for cid in sorted(merged)],
        "ref": shlex.join(ref_argv),
        "updated_at": datetime.datetime.now(datetime.UTC).isoformat(timespec="seconds"),
    })
    return len(added)


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def parse_args(argv: list[str] | None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Differential replay of omp kernel cells under the IUT and CPython.")
    parser.add_argument("--impl", default="./qpy", help="IUT command (default ./qpy, relative to the repo root)")
    parser.add_argument("--ref", default="python3.15", help="reference interpreter command (default python3.15)")
    parser.add_argument("--corpus", default=DEFAULT_CORPUS, help=f"corpus directory (default {DEFAULT_CORPUS})")
    parser.add_argument("--mode", choices=("sessions", "selfcontained"), default="sessions")
    parser.add_argument("--limit", type=int, default=None, help="replay at most N items")
    parser.add_argument("--seed", type=int, default=None, help="shuffle items with this seed before --limit")
    parser.add_argument("-j", "--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2),
                        help="kernel pairs running in parallel")
    parser.add_argument("--timeout", type=float, default=30.0, help="seconds per cell (default 30)")
    parser.add_argument("--calibrate", action="store_true",
                        help="run the reference twice and record nondeterministic cells")
    parser.add_argument("--min-match-pct", type=float, default=0.0,
                        help="exit 1 if the match percentage is below this (default 0)")
    parser.add_argument("--no-sandbox", action="store_true", help="do not use sandbox-exec")
    return parser.parse_args(argv)


def run(args: argparse.Namespace) -> int:
    """Run a replay as configured by ``args``; returns the exit status."""
    corpus = load_corpus(Path(args.corpus).expanduser())
    ref_argv = resolve_command(args.ref, "reference interpreter")
    if args.calibrate:
        a_label, a_argv, b_label = "ref1", ref_argv, "ref2"
    else:
        a_label, a_argv, b_label = "iut", resolve_command(args.impl, "IUT"), "ref"

    sandbox = not args.no_sandbox
    if sandbox and not (sys.platform == "darwin" and shutil.which("sandbox-exec")):
        print("warning: sandbox-exec unavailable; running kernels unsandboxed", file=sys.stderr)
        sandbox = False
    elif not sandbox:
        print("warning: --no-sandbox: kernels can write anywhere and use the network", file=sys.stderr)

    items = select_items(corpus, args.mode, args.limit, args.seed)
    missing = {cid for item in items for cid in item.cell_ids if cid not in corpus.sources}
    if missing:
        raise SetupError(f"corpus is inconsistent: {len(missing)} cell ids missing from cells.jsonl")

    with tempfile.TemporaryDirectory(prefix="dquickts-replay-") as workroot:
        cfg = ReplayConfig(a_label, a_argv, b_label, ref_argv, sandbox, args.timeout,
                           args.calibrate, Path(workroot))
        _preflight(cfg)
        results = _replay_all(items, corpus, cfg, args.jobs)

    totals = summarize(results)
    name = "replay-calibrate-report.json" if args.calibrate else "replay-report.json"
    report_path = REPORT_DIR / name
    write_json(report_path, {
        "mode": args.mode,
        "calibrate": args.calibrate,
        "sides": {a_label: shlex.join(a_argv), b_label: shlex.join(ref_argv)},
        "sandbox": sandbox,
        "corpus": str(corpus.path),
        "limit": args.limit,
        "seed": args.seed,
        "timeout": args.timeout,
        "totals": totals,
        "items": [
            {"name": res.name, "cells": res.cells, "counts": dict(res.counts), "ms": res.ms,
             "events": res.events}
            for res in results
        ],
    })
    print_summary(totals, results, corpus, cfg, args.mode, report_path)
    print(f"\nreport: {report_path}")

    if args.calibrate:
        added = update_nondeterministic(corpus, results, ref_argv)
        print(f"nondeterministic cells: {added} added, list at {corpus.path / NONDETERMINISTIC_FILE}")
        return 0
    return 0 if totals["match_pct"] >= args.min_match_pct else 1


def _preflight(cfg: ReplayConfig) -> None:
    """Start one reference kernel (sandboxed if configured) to catch setup errors early."""
    kernel = Kernel(cfg.b_label, cfg.b_argv, cfg.workroot / "preflight", cfg.sandbox)
    try:
        kernel.start()
    except KernelCrash as e:
        raise SetupError(f"reference kernel does not start: {e}") from e
    finally:
        kernel.stop()


def _replay_all(items: list[Item], corpus: Corpus, cfg: ReplayConfig, jobs: int) -> list[ItemResult]:
    """Replay all items with ``jobs`` workers, preserving item order in the result."""
    results: list[ItemResult | None] = [None] * len(items)
    progress = sys.stderr.isatty()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
        futures = {pool.submit(replay_item, i, item, corpus, cfg): i for i, item in enumerate(items)}
        try:
            for done, future in enumerate(concurrent.futures.as_completed(futures), 1):
                results[futures[future]] = future.result()
                if progress:
                    print(f"\r  {done}/{len(items)} done", end="", file=sys.stderr, flush=True)
        except BaseException:
            for future in futures:
                future.cancel()
            kill_all_kernels()
            raise
    if progress:
        print(file=sys.stderr)
    return [res for res in results if res is not None]


def main(argv: list[str] | None = None) -> int:
    """Command-line entry point; returns the exit status."""
    args = parse_args(argv)
    try:
        return run(args)
    except SetupError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        kill_all_kernels()
        print("interrupted", file=sys.stderr)
        return 2
    finally:
        kill_all_kernels()


if __name__ == "__main__":
    sys.exit(main())
