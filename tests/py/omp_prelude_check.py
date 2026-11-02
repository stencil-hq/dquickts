"""Check that omp's eval-kernel prelude imports and its pure helpers run.

The prelude is loaded from the omp checkout (``OMP_PRELUDE`` or the default
path below) into a fresh namespace, the way omp's ``runner.py`` does: the
runner injects ``__omp_emit_status`` / ``__omp_emit_image`` first, then
executes the prelude source.  The script then exercises the helpers that need
no agent host (``env``, ``read``, ``write``, ``log``, ``phase``, ``display``
when the prelude defines it, ``read``/``write`` on internal URLs with an
injected ``PI_EVAL_LOCAL_ROOTS`` root) and checks the status events they emit.
Agent-dependent helpers (``output``, ``tool``, ``budget``) must fail with the
prelude's own "tool bridge is unavailable" error, since no bridge is set.

Exit status: 0 on success (or when the prelude path does not exist: skipped),
1 on the first failed check.

Usage::

    ./qpy tests/py/omp_prelude_check.py
    python3.15 tests/py/omp_prelude_check.py      # reference run
"""

import os
import sys
import tempfile

DEFAULT_PRELUDE = "/work/pi/packages/coding-agent/src/eval/py/prelude.py"
BRIDGE_VARS = ("PI_TOOL_BRIDGE_URL", "PI_TOOL_BRIDGE_TOKEN", "PI_TOOL_BRIDGE_SESSION")


def check(cond: bool, what: str) -> None:
    if not cond:
        print(f"FAIL: {what}")
        sys.exit(1)
    print(f"ok: {what}")


def load_prelude(path: str) -> tuple[dict, list]:
    """Execute the prelude in a fresh namespace; returns it and the status event list."""
    events: list = []
    ns = {"__name__": "__main__", "__builtins__": __builtins__}
    ns["__omp_emit_status"] = lambda kind, **fields: events.append((kind, fields))
    ns["__omp_emit_image"] = lambda payload: events.append(("image", payload))
    with open(path, encoding="utf-8") as f:
        source = f.read()
    exec(compile(source, path, "exec"), ns)
    return ns, events


def expect_error(fn, what: str) -> None:
    """``fn()`` must raise; prints the prelude's own message."""
    try:
        fn()
    except Exception as e:
        print(f"ok: {what} raises {type(e).__name__}: {e}")
        return
    check(False, f"{what} raises")


def main() -> int:
    path = os.environ.get("OMP_PRELUDE", DEFAULT_PRELUDE)
    if not os.path.exists(path):
        print(f"SKIP: omp prelude not found at {path} (set OMP_PRELUDE)")
        return 0
    for var in BRIDGE_VARS:
        os.environ.pop(var, None)

    ns, events = load_prelude(path)
    check(ns.get("__omp_prelude_loaded__") is True, "prelude executed")
    for name in ("env", "read", "write", "log", "phase", "output", "agent", "workpool"):
        check(callable(ns.get(name)), f"prelude defines {name}")
    check("tool" in ns and "budget" in ns, "prelude defines tool and budget")
    for mod in ("asyncio", "contextvars", "inspect", "json", "re", "typing", "urllib"):
        check(mod in ns, f"prelude imported {mod}")

    with tempfile.TemporaryDirectory() as tmp:
        # env
        check(ns["env"]("OMP_PRELUDE_CHECK", "v1") == "v1", "env(key, value) sets")
        check(os.environ.get("OMP_PRELUDE_CHECK") == "v1", "env set reaches os.environ")
        check(ns["env"]("OMP_PRELUDE_CHECK") == "v1", "env(key) gets")
        allvars = ns["env"]()
        check(isinstance(allvars, dict) and list(allvars) == sorted(allvars), "env() returns a sorted dict")
        check(events[-1][0] == "env" and events[-1][1].get("count") == len(allvars), "env emits status")

        # write / read on plain paths
        target = os.path.join(tmp, "sub", "dir", "f.txt")
        p = ns["write"](target, "alpha\nbeta\ngamma\n")
        check(str(p) == target and os.path.exists(target), "write creates parents, returns Path")
        check(events[-1] == ("write", {"path": target, "chars": 17}), "write emits status")
        check(ns["read"](target) == "alpha\nbeta\ngamma\n", "read whole file")
        check(ns["read"](target, offset=2) == "beta\ngamma\n", "read offset")
        check(ns["read"](target, offset=2, limit=1) == "beta\n", "read offset+limit")
        check(events[-1][0] == "read" and events[-1][1]["path"] == target, "read emits status")
        expect_error(lambda: ns["read"](os.path.join(tmp, "missing.txt")), "read of a missing file")

        # internal URLs: an injected root maps local:// onto disk, others are rejected
        os.environ["PI_EVAL_LOCAL_ROOTS"] = '{"local": "%s"}' % tmp
        try:
            p = ns["write"]("local://notes/n.md", "# hi\n")
            check(str(p) == os.path.join(os.path.abspath(tmp), "notes", "n.md"), "write local:// under its root")
            check(ns["read"]("local://notes/n.md") == "# hi\n", "read local:// from its root")
            expect_error(lambda: ns["write"]("local://../escape.md", "x"), "write local:// traversal")
        finally:
            del os.environ["PI_EVAL_LOCAL_ROOTS"]
        expect_error(lambda: ns["write"]("artifact://3", "x"), "write artifact://")

    # log / phase
    check(ns["log"]("hello") is None and events[-1] == ("log", {"message": "hello"}), "log emits status")
    check(ns["phase"]("Build") is None and events[-1] == ("phase", {"title": "Build"}), "phase emits status")
    check(ns.get("__omp_current_phase__") == "Build", "phase records __omp_current_phase__")

    # display: omp's runner (not every prelude version) provides it
    if callable(ns.get("display")):
        ns["display"]({"a": 1})
        print("ok: display ran")
    else:
        print("skip: display is not defined by this prelude (omp's runner displays trailing expressions)")

    # agent-dependent helpers: no tool bridge in this process
    expect_error(lambda: ns["output"]("agent-1"), "output() without the tool bridge")
    expect_error(lambda: ns["read"]("skill://x"), "read(skill://) without the tool bridge")
    print("budget:", repr(ns["budget"]))
    print("omp prelude check: all ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
