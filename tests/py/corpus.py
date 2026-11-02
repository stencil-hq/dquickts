"""Extract the L1 differential-replay corpus from the omp stats database.

The omp agent records every tool call in ``~/.omp/stats.db``.  Python cells run
through the ``eval``, ``python`` and ``mcp_ida_py_eval`` tools form the L1 corpus:
real-world code that ``tests/py/replay.py`` replays cell by cell under the
implementation under test and under CPython, comparing the results.

The corpus is private user data.  It is extracted into a local cache directory
(default ``~/.cache/dquickts/corpus``) and never copied into the repository;
only this tool is committed.

Output files (all in ``--out``):

``cells.jsonl``
    One record per unique cell source::

        {"id", "source", "source_pp", "parses", "top_level_await",
         "occurrences", "first_ts", "tool", "model"}

    ``id`` is the first 16 hex digits of ``sha1(source)``; ``source_pp`` is the
    source with IPython-style magic lines (``%...``, ``!...``) replaced by
    ``pass``; ``parses`` / ``top_level_await`` describe ``source_pp`` compiled by
    CPython with ``PyCF_ALLOW_TOP_LEVEL_AWAIT``.
``sessions.jsonl``
    One record per agent session::

        {"session", "cells": [{"id", "ts"}, ...], "replayable", "prefix_len"}

    Cells are ordered by timestamp; repeated sources are kept (re-executions).
    A session is replayable when every cell parses and needs only the stdlib,
    performs no file writes and makes no agent/tool calls; ``prefix_len`` is the
    number of leading cells that satisfy those rules.
``selfcontained.jsonl``
    ``{"id"}`` of every cell that is self-contained and side-effect free, i.e.
    replayable in a fresh kernel without any session state.
``stats.json``
    Summary counts (see ``REFERENCE_COUNTS`` for the values the plan quotes).

Usage::

    python3.15 tests/py/corpus.py extract
    python3.15 tests/py/corpus.py extract --db ~/.omp/stats.db --out /tmp/corpus
    python3.15 tests/py/corpus.py stats
"""

from __future__ import annotations

import argparse
import ast
import builtins
import collections
import contextlib
import dataclasses
import datetime
import hashlib
import inspect
import json
import os
import re
import sqlite3
import sys
import warnings
from collections.abc import Iterable, Iterator
from pathlib import Path
from typing import Any

DEFAULT_DB = "~/.omp/stats.db"
DEFAULT_OUT = "~/.cache/dquickts/corpus"

#: Counts reported in doc/python-plan.md for the database at planning time.
#: The database keeps growing, so current values are expected to be close to,
#: and usually somewhat above, these.
REFERENCE_COUNTS: dict[str, int] = {
    "cells": 104_519,
    "calls": 101_399,
    "unique": 92_146,
    "parse_ok": 91_765,
    "sessions": 10_690,
    "replayable_sessions": 4_602,
    "replayable_session_cells": 21_309,
    "prefix_cells": 31_370,
    "selfcontained": 1_921,
}

STAT_LABELS: dict[str, str] = {
    "cells": "cells (with re-executions)",
    "calls": "tool calls with Python cells",
    "unique": "unique sources",
    "parse_ok": "unique sources parsing (3.15)",
    "top_level_await": "unique sources with top-level await",
    "sessions": "sessions",
    "replayable_sessions": "fully replayable sessions",
    "replayable_session_cells": "cells in replayable sessions",
    "prefix_cells": "cells in replayable prefixes",
    "selfcontained": "self-contained unique sources",
    "selfcontained_occurrences": "self-contained cell occurrences",
}

PY_TOOLS = ("eval", "python", "mcp_ida_py_eval")
PY_LANGUAGES = ("py", "python", None)

#: Old eval tool input format: ``=== CELL name ===`` followed by a fenced block.
OLD_CELL_RE = re.compile(r"=== CELL[^\n]*\n```(\w+)?\n(.*?)\n```", re.S)

COMPILE_FLAGS = ast.PyCF_ALLOW_TOP_LEVEL_AWAIT

STDLIB_MODULES = sys.stdlib_module_names
BUILTIN_NAMES = frozenset(dir(builtins))

#: Free names that refer to a module imported by an earlier cell of the session.
FREE_NAME_MODULES: dict[str, str] = {
    "subprocess": "subprocess", "np": "numpy", "numpy": "numpy", "Image": "PIL",
    "collections": "collections", "Counter": "collections", "defaultdict": "collections",
    "deque": "collections", "shutil": "shutil", "struct": "struct", "glob": "glob",
    "ast": "ast", "tempfile": "tempfile", "sys": "sys", "pathlib": "pathlib",
    "sqlite3": "sqlite3", "hashlib": "hashlib", "itertools": "itertools",
    "textwrap": "textwrap", "difflib": "difflib", "datetime": "datetime",
    "base64": "base64", "shlex": "shlex", "yaml": "yaml", "plt": "matplotlib",
    "statistics": "statistics", "random": "random", "io": "io",
    "importlib": "importlib", "socket": "socket", "threading": "threading",
    "csv": "csv", "functools": "functools", "copy": "copy", "zlib": "zlib",
    "signal": "signal", "tarfile": "tarfile", "pd": "pandas",
}

#: Method names whose call means a file-system write, a process or a connection.
WRITE_ATTRS = frozenset({
    "write_text", "write_bytes", "mkdir", "unlink", "rmtree", "makedirs", "remove",
    "rename", "symlink", "chmod", "copy", "copytree", "move", "chdir", "Popen",
    "urlopen", "connect",
})

#: Prelude names that need the agent host (tool bridge, subagents, models).
AGENT_NAMES = frozenset({
    "tool", "agent", "completion", "parallel", "judge", "judge_batch", "workpool",
    "ctl", "g", "output",
})

#: Stdlib modules without I/O or global side effects.
SAFE_MODULES = frozenset({
    "json", "re", "collections", "itertools", "functools", "math", "statistics",
    "textwrap", "difflib", "string", "dataclasses", "enum", "typing", "struct",
    "base64", "hashlib", "heapq", "bisect", "copy", "operator", "fractions",
    "decimal", "datetime", "unicodedata", "pprint", "shlex", "fnmatch", "html",
    "binascii", "zlib", "io", "keyword", "types", "abc", "contextlib", "ast",
    "tokenize", "random",
})

#: Builtins (or free functions) with side effects or dynamic code execution.
SIDE_EFFECT_CALLS = frozenset({
    "open", "exec", "eval", "compile", "__import__", "input", "breakpoint",
})

#: Method names that touch the file system, processes, network, or the clock.
SIDE_EFFECT_ATTRS = frozenset({
    "read_text", "write_text", "read_bytes", "write_bytes", "mkdir", "unlink",
    "rmtree", "glob", "rglob", "exists", "iterdir", "stat", "run", "Popen",
    "check_output", "system", "urlopen", "sleep", "connect", "execute", "listdir",
    "walk", "makedirs", "remove", "rename", "symlink", "chmod", "copy", "copytree",
    "move", "getcwd", "chdir", "time", "now", "today", "perf_counter",
})

#: Prelude-provided names a self-contained cell may use without importing them.
SELF_CONTAINED_FREE_OK = frozenset({"json", "re", "math", "types", "typing", "__name__", "__file__"})


@dataclasses.dataclass(frozen=True, slots=True)
class CellOccurrence:
    """One execution of a Python cell recorded in the stats database."""

    call_id: int
    session_file: str
    tool: str
    ts: int
    model: str | None
    source: str


@dataclasses.dataclass(slots=True)
class Facts:
    """Static facts about a cell's AST used to classify it.

    ``imports``: top-level module names imported; ``free``: names loaded but
    neither defined, imported nor builtin; ``attr_calls``: method names called;
    ``free_calls``: called names that are free; ``builtin_calls``: builtins
    called (and not shadowed by a definition in the cell).
    """

    imports: set[str] = dataclasses.field(default_factory=set)
    free: set[str] = dataclasses.field(default_factory=set)
    attr_calls: set[str] = dataclasses.field(default_factory=set)
    free_calls: set[str] = dataclasses.field(default_factory=set)
    builtin_calls: set[str] = dataclasses.field(default_factory=set)


@dataclasses.dataclass(slots=True)
class SourceInfo:
    """Classification of one unique cell source."""

    id: str
    source: str
    source_pp: str
    parses: bool
    top_level_await: bool
    replay_ok: bool
    self_contained: bool


# --------------------------------------------------------------------------
# Extraction from tool-call arguments
# --------------------------------------------------------------------------

def _normalize_code(code: Any) -> str | None:
    """Return cell code as a string; a list of strings is joined (notebook style)."""
    if isinstance(code, list):
        code = "".join(part if isinstance(part, str) else "" for part in code)
    return code if isinstance(code, str) else None


def _iter_cell_entries(cells: Any) -> Iterator[tuple[Any, Any]]:
    """Yield ``(code, language)`` for each entry of a ``cells`` argument."""
    if isinstance(cells, str):
        yield cells, None
        return
    if isinstance(cells, dict):
        cells = [cells]
    for cell in cells or []:
        if isinstance(cell, str):
            yield cell, None
        elif isinstance(cell, dict):
            yield cell.get("code") or cell.get("source"), cell.get("language")


def python_cells_from_args(tool: str, args: dict[str, Any]) -> Iterator[Any]:
    """Yield the raw Python code values contained in one tool call's arguments.

    Handles every historical argument shape of the ``eval`` tool (``code``,
    ``cells`` and the old ``input`` text format), the ``python`` tool and the
    IDA ``mcp_ida_py_eval`` tool.  Values are not yet normalised.
    """
    lang = args.get("language")
    if tool == "eval":
        if "code" in args and lang in PY_LANGUAGES:
            yield args["code"]
        elif "cells" in args:
            for code, cell_lang in _iter_cell_entries(args["cells"]):
                if (cell_lang or lang) in PY_LANGUAGES:
                    yield code
        elif isinstance(args.get("input"), str):
            for match in OLD_CELL_RE.finditer(args["input"]):
                if match.group(1) in ("py", "python"):
                    yield match.group(2)
    elif tool == "python":
        if "cells" in args:
            for code, _ in _iter_cell_entries(args["cells"]):
                yield code
        elif "code" in args:
            yield args["code"]
    else:
        yield args.get("code")


def read_occurrences(db_path: Path) -> list[CellOccurrence]:
    """Read every Python cell from the stats database (opened read-only).

    Occurrences are returned ordered by call timestamp (then row id, then cell
    position within the call).  Empty or whitespace-only cells are dropped.
    """
    uri = db_path.resolve().as_uri() + "?mode=ro"
    placeholders = ",".join("?" for _ in PY_TOOLS)
    query = (
        "SELECT id, session_file, tool_name, timestamp, model, arg_json FROM ss_tool_calls "
        f"WHERE tool_name IN ({placeholders}) ORDER BY timestamp, id"
    )
    occurrences: list[CellOccurrence] = []
    with contextlib.closing(sqlite3.connect(uri, uri=True)) as db:
        for call_id, session_file, tool, ts, model, arg_json in db.execute(query, PY_TOOLS):
            try:
                args = json.loads(arg_json)
            except (TypeError, ValueError):
                continue
            if not isinstance(args, dict):
                continue
            for raw in python_cells_from_args(tool, args):
                code = _normalize_code(raw)
                if code and code.strip():
                    occurrences.append(CellOccurrence(call_id, session_file, tool, ts, model, code))
    return occurrences


# --------------------------------------------------------------------------
# Classification
# --------------------------------------------------------------------------

def preprocess(code: str) -> str:
    """Replace IPython-style magic lines with ``pass`` at the same indentation.

    A line whose stripped form starts with ``%`` (line/cell magics) or ``!``
    (shell escapes) becomes ``pass``; everything else is unchanged.
    """
    out = []
    for line in code.split("\n"):
        stripped = line.lstrip()
        if stripped.startswith(("%", "!")):
            out.append(line[: len(line) - len(stripped)] + "pass")
        else:
            out.append(line)
    return "\n".join(out)


def source_id(source: str) -> str:
    """Stable cell id: first 16 hex digits of the SHA-1 of the source."""
    return hashlib.sha1(source.encode("utf-8", "surrogatepass")).hexdigest()[:16]


def session_id(session_file: str) -> str:
    """Stable session id: first 16 hex digits of the SHA-1 of the session file name."""
    return hashlib.sha1(session_file.encode("utf-8", "surrogatepass")).hexdigest()[:16]


def _bound_names(tree: ast.AST) -> tuple[set[str], set[str]]:
    """Return ``(imported, defined)`` names bound anywhere in the tree.

    ``imported`` holds names bound by absolute imports; ``defined`` holds
    assignment/deletion targets, function/class names, parameters and
    exception-handler names.
    """
    imported: set[str] = set()
    defined: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                imported.add(alias.asname or alias.name.split(".")[0])
        elif isinstance(node, ast.ImportFrom) and not node.level:
            for alias in node.names:
                imported.add(alias.asname or alias.name)
        elif isinstance(node, ast.Name) and not isinstance(node.ctx, ast.Load):
            defined.add(node.id)
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            defined.add(node.name)
        elif isinstance(node, ast.arg):
            defined.add(node.arg)
        elif isinstance(node, ast.ExceptHandler) and node.name:
            defined.add(node.name)
    return imported, defined


def analyse(tree: ast.AST) -> Facts:
    """Compute the classification ``Facts`` of a parsed cell."""
    imported, defined = _bound_names(tree)
    facts = Facts()
    loaded: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                facts.imports.add(alias.name.split(".")[0])
        elif isinstance(node, ast.ImportFrom) and not node.level:
            facts.imports.add((node.module or "").split(".")[0])
        elif isinstance(node, ast.Name) and isinstance(node.ctx, ast.Load):
            loaded.add(node.id)
        elif isinstance(node, ast.Call):
            func = node.func
            if isinstance(func, ast.Attribute):
                facts.attr_calls.add(func.attr)
            elif isinstance(func, ast.Name) and func.id not in imported:
                if func.id in BUILTIN_NAMES and func.id not in defined:
                    facts.builtin_calls.add(func.id)
                elif func.id not in defined:
                    facts.free_calls.add(func.id)
    facts.free = loaded - defined - BUILTIN_NAMES - imported
    return facts


def modules_needed(facts: Facts) -> set[str]:
    """Modules a cell needs: its own imports plus modules behind known free names."""
    modules = set(facts.imports)
    modules.update(FREE_NAME_MODULES[name] for name in facts.free if name in FREE_NAME_MODULES)
    return modules


def replay_ok(facts: Facts) -> bool:
    """True if the cell needs only the stdlib, does not write files and calls no agent helper."""
    if any(module not in STDLIB_MODULES for module in modules_needed(facts)):
        return False
    if facts.attr_calls & WRITE_ATTRS:
        return False
    if (facts.free | facts.free_calls) & AGENT_NAMES:
        return False
    return "write" not in facts.free_calls


def self_contained(facts: Facts, top_level_await: bool) -> bool:
    """True if the cell runs in a fresh kernel and has no side effects.

    It may use only safe stdlib modules and the prelude modules in
    ``SELF_CONTAINED_FREE_OK``, must not touch files, processes, network or the
    clock, and must not use top-level ``await``.
    """
    if facts.free - SELF_CONTAINED_FREE_OK:
        return False
    if facts.imports - SAFE_MODULES:
        return False
    if (facts.builtin_calls | facts.free_calls) & SIDE_EFFECT_CALLS:
        return False
    if facts.attr_calls & SIDE_EFFECT_ATTRS:
        return False
    return not top_level_await


def classify(source: str) -> SourceInfo:
    """Preprocess, parse and classify one cell source.

    ``parses`` means CPython accepts the preprocessed source as an AST with
    ``PyCF_ALLOW_TOP_LEVEL_AWAIT``; ``top_level_await`` means the compiled code
    object is a coroutine (``CO_COROUTINE``).  Callers should suppress
    ``SyntaxWarning`` output (see ``extract``).
    """
    source_pp = preprocess(source)
    info = SourceInfo(source_id(source), source, source_pp, False, False, False, False)
    try:
        tree = compile(source_pp, "<cell>", "exec", ast.PyCF_ONLY_AST | COMPILE_FLAGS)
    except Exception:  # arbitrary input: SyntaxError, ValueError (NUL), RecursionError, ...
        return info
    info.parses = True
    try:
        code = compile(tree, "<cell>", "exec", COMPILE_FLAGS)
        info.top_level_await = bool(code.co_flags & inspect.CO_COROUTINE)
    except Exception:  # valid AST rejected by the compiler (e.g. 'return' outside function)
        pass
    facts = analyse(tree)
    info.replay_ok = replay_ok(facts)
    info.self_contained = self_contained(facts, info.top_level_await)
    return info


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------

def _write_jsonl(path: Path, records: Iterable[dict[str, Any]]) -> None:
    """Write records as JSON lines, atomically replacing ``path``."""
    tmp = path.with_name(path.name + ".tmp")
    with tmp.open("w", encoding="utf-8") as f:
        for record in records:
            f.write(json.dumps(record, ensure_ascii=True))
            f.write("\n")
    os.replace(tmp, path)


def _write_json(path: Path, value: Any) -> None:
    """Write one pretty-printed JSON document, atomically replacing ``path``."""
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def extract(db_path: Path, out_dir: Path) -> dict[str, Any]:
    """Extract and classify the corpus from ``db_path`` into ``out_dir``.

    Writes ``cells.jsonl``, ``sessions.jsonl``, ``selfcontained.jsonl`` and
    ``stats.json`` and returns the stats dictionary.
    """
    occurrences = read_occurrences(db_path)

    # Unique sources, keyed by source text, in order of first occurrence.
    first: dict[str, CellOccurrence] = {}
    counts: collections.Counter[str] = collections.Counter()
    for occ in occurrences:
        first.setdefault(occ.source, occ)
        counts[occ.source] += 1

    infos: dict[str, SourceInfo] = {}
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        for source in first:
            infos[source] = classify(source)

    by_session: dict[str, list[CellOccurrence]] = {}
    for occ in occurrences:
        by_session.setdefault(occ.session_file, []).append(occ)

    out_dir.mkdir(parents=True, exist_ok=True)

    _write_jsonl(out_dir / "cells.jsonl", (
        {
            "id": info.id,
            "source": source,
            "source_pp": info.source_pp,
            "parses": info.parses,
            "top_level_await": info.top_level_await,
            "occurrences": counts[source],
            "first_ts": first[source].ts,
            "tool": first[source].tool,
            "model": first[source].model,
        }
        for source, info in infos.items()
    ))

    session_records = []
    for session_file, occs in by_session.items():
        ok = [infos[o.source].parses and infos[o.source].replay_ok for o in occs]
        prefix_len = ok.index(False) if False in ok else len(ok)
        session_records.append({
            "session": session_id(session_file),
            "cells": [{"id": infos[o.source].id, "ts": o.ts} for o in occs],
            "replayable": prefix_len == len(occs),
            "prefix_len": prefix_len,
        })
    _write_jsonl(out_dir / "sessions.jsonl", session_records)

    self_contained_sources = [s for s, info in infos.items() if info.parses and info.self_contained]
    _write_jsonl(out_dir / "selfcontained.jsonl",
                 ({"id": infos[s].id} for s in self_contained_sources))

    replayable = [r for r in session_records if r["replayable"]]
    stats: dict[str, Any] = {
        "cells": len(occurrences),
        "calls": len({occ.call_id for occ in occurrences}),
        "unique": len(infos),
        "parse_ok": sum(info.parses for info in infos.values()),
        "top_level_await": sum(info.top_level_await for info in infos.values()),
        "sessions": len(session_records),
        "replayable_sessions": len(replayable),
        "replayable_session_cells": sum(len(r["cells"]) for r in replayable),
        "prefix_cells": sum(r["prefix_len"] for r in session_records),
        "selfcontained": len(self_contained_sources),
        "selfcontained_occurrences": sum(counts[s] for s in self_contained_sources),
        "db": str(db_path),
        "python": sys.version.split()[0],
        "extracted_at": datetime.datetime.now(datetime.UTC).isoformat(timespec="seconds"),
    }
    _write_json(out_dir / "stats.json", stats)
    return stats


def format_stats(stats: dict[str, Any]) -> str:
    """Render stats as an aligned table with the plan's reference values."""
    lines = [
        f"corpus extracted {stats.get('extracted_at', '?')} from {stats.get('db', '?')} "
        f"(CPython {stats.get('python', '?')})"
    ]
    width = max(len(label) for label in STAT_LABELS.values())
    for key, label in STAT_LABELS.items():
        if key not in stats:
            continue
        line = f"  {label:<{width}}  {stats[key]:>9,}"
        if key in REFERENCE_COUNTS:
            line += f"   (plan: {REFERENCE_COUNTS[key]:,})"
        lines.append(line)
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    """Command-line entry point; returns the process exit code."""
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p_extract = sub.add_parser("extract", help="extract the corpus from the stats database")
    p_extract.add_argument("--db", default=DEFAULT_DB, help=f"stats database (default {DEFAULT_DB})")
    p_extract.add_argument("--out", default=DEFAULT_OUT, help=f"output directory (default {DEFAULT_OUT})")
    p_stats = sub.add_parser("stats", help="print the stats of an extracted corpus")
    p_stats.add_argument("--out", default=DEFAULT_OUT, help=f"corpus directory (default {DEFAULT_OUT})")
    args = parser.parse_args(argv)

    out_dir = Path(args.out).expanduser()
    if args.command == "extract":
        db_path = Path(args.db).expanduser()
        if not db_path.is_file():
            print(f"error: stats database not found: {db_path}", file=sys.stderr)
            return 2
        try:
            stats = extract(db_path, out_dir)
        except sqlite3.Error as e:
            print(f"error: cannot read {db_path}: {e}", file=sys.stderr)
            return 2
        print(f"wrote corpus to {out_dir}")
        print(format_stats(stats))
        return 0

    stats_path = out_dir / "stats.json"
    if not stats_path.is_file():
        print(f"error: no corpus at {out_dir}; run `corpus.py extract` first", file=sys.stderr)
        return 2
    print(format_stats(json.loads(stats_path.read_text(encoding="utf-8"))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
