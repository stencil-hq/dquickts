"""Run one CPython ``Lib/test`` module and report every test as a JSON line.

This module runs under the implementation under test (IUT), so it only uses
plain, portable stdlib modules (sys, os, json, unittest, traceback).  It is
started by ``tests/py/run.py l3`` once per test module::

    TESTPY_RESULTS=/tmp/results.jsonl IUT -X utf8 -m testpy_reporter test.test_int

Environment:

``TESTPY_RESULTS``
    Path of the results file (required).  One JSON object per line is appended
    and flushed immediately, so a crash of the IUT loses at most the test that
    was running: ``{"id": "test_int.IntTestCases.test_basic", "outcome":
    "pass", "detail": ""}``.  Outcomes: pass, fail, error, skip, xfail, xpass.
    Ids drop the leading ``test.`` package prefix.  A final
    ``{"id": "__done__", ...}`` line marks a clean finish.
``TESTPY_EXCLUDE``
    Comma-separated test ids (or id prefixes such as a class or module) that
    are not run; each excluded test is reported as ``skip`` with detail
    ``excluded``.
``TESTPY_RESOURCES``
    Comma-separated ``test.support`` resources to enable (default: none).

The exit code is 0 whenever the run finished, even if tests failed.
"""

import json
import os
import sys
import traceback
import unittest

DETAIL_LIMIT = 500
DONE_ID = "__done__"

# Prefixes stripped from test ids, in order: unittest's synthetic tests for
# failed imports/loads and skipped modules, then the ``test`` package.
_ID_PREFIXES = (
    "unittest.loader._FailedTest.",
    "unittest.loader.ModuleSkipped.",
    "test.",
)


def normalize_id(raw: str) -> str:
    """Return the reported form of a unittest id.

    ``test.test_int.IntTestCases.test_basic`` becomes
    ``test_int.IntTestCases.test_basic``; class/module fixture errors such as
    ``setUpClass (test.test_x.Foo)`` become ``test_x.Foo.setUpClass``.
    """
    if raw.endswith(")") and " (" in raw:
        name, _, parent = raw[:-1].partition(" (")
        raw = parent + "." + name
    for prefix in _ID_PREFIXES:
        if raw.startswith(prefix):
            raw = raw[len(prefix):]
    return raw


def format_error(err, is_failure: bool) -> str:
    """Format ``sys.exc_info()``-style ``err`` without unittest's own frames.

    Leading frames from the unittest package are dropped; for assertion
    failures the trailing unittest frames (assert helpers) are dropped too,
    mirroring ``unittest.TestResult``.  The result is cut to DETAIL_LIMIT
    characters.
    """
    exctype, value, tb = err
    frames = []
    started = False
    for frame, lineno in traceback.walk_tb(tb):
        in_unittest = "__unittest" in frame.f_globals
        if not started:
            if in_unittest:
                continue
            started = True
        elif in_unittest and is_failure:
            break
        frames.append((frame, lineno))
    lines = ["Traceback (most recent call last):\n"]
    lines.extend(traceback.StackSummary.extract(iter(frames)).format())
    lines.extend(traceback.format_exception_only(exctype, value))
    return "".join(lines)[:DETAIL_LIMIT]


class ResultWriter:
    """Append result records to the JSON lines file, one flushed line each.

    The file is reopened for every record because tests may close inherited
    file descriptors; records written from forked children are dropped.
    """

    def __init__(self, path: str) -> None:
        self.path = path
        self.pid = os.getpid()

    def write(self, test_id: str, outcome: str, detail: str = "") -> None:
        """Append one ``{"id", "outcome", "detail"}`` record."""
        if os.getpid() != self.pid:
            return
        line = json.dumps({"id": test_id, "outcome": outcome,
                           "detail": detail[:DETAIL_LIMIT]})
        with open(self.path, "a", encoding="ascii") as f:
            f.write(line + "\n")
            f.flush()


_FAILING = ("fail", "error", "xpass")


class _TestState:
    """Outcome of one running test, combined from its subtests."""

    __slots__ = ("outcome", "detail", "subtest_skip")

    def __init__(self) -> None:
        self.outcome = None
        self.detail = ""
        self.subtest_skip = None

    def set(self, outcome: str, detail: str) -> None:
        """Record ``outcome``; the first failing outcome is never replaced."""
        if self.outcome not in _FAILING:
            self.outcome = outcome
            self.detail = detail


class JsonLinesResult(unittest.TestResult):
    """``unittest.TestResult`` that writes one record per test id.

    Records for started tests are emitted from ``stopTest`` so subtests fold
    into their parent (any failing subtest fails the parent).  Outcomes for
    tests that were never started (class/module fixture errors and skips) are
    emitted immediately.
    """

    def __init__(self, writer: ResultWriter) -> None:
        super().__init__()
        self.writer = writer
        self._running = {}

    @staticmethod
    def _parent(test):
        """Map a subtest to the test case that owns it."""
        sub_type = getattr(unittest.case, "_SubTest", None)
        if sub_type is not None and isinstance(test, sub_type):
            return test.test_case
        return test

    def _report(self, test, outcome: str, detail: str = "") -> None:
        test = self._parent(test)
        state = self._running.get(test.id())
        if state is None:
            self.writer.write(normalize_id(test.id()), outcome, detail)
        else:
            state.set(outcome, detail)

    def startTest(self, test) -> None:
        super().startTest(test)
        self._running[test.id()] = _TestState()

    def stopTest(self, test) -> None:
        state = self._running.pop(test.id(), None)
        if state is not None:
            outcome = state.outcome
            detail = state.detail
            if outcome is None:
                if state.subtest_skip is not None:
                    outcome, detail = "skip", state.subtest_skip
                else:
                    outcome = "pass"
            self.writer.write(normalize_id(test.id()), outcome, detail)
        super().stopTest(test)

    def addSuccess(self, test) -> None:
        self._report(test, "pass")

    def addFailure(self, test, err) -> None:
        self._report(test, "fail", format_error(err, True))

    def addError(self, test, err) -> None:
        self._report(test, "error", format_error(err, False))

    def addSkip(self, test, reason) -> None:
        parent = self._parent(test)
        state = self._running.get(parent.id())
        if parent is not test and state is not None:
            state.subtest_skip = str(reason)
        else:
            self._report(test, "skip", str(reason))

    def addExpectedFailure(self, test, err) -> None:
        self._report(test, "xfail", format_error(err, False))

    def addUnexpectedSuccess(self, test) -> None:
        self._report(test, "xpass", "unexpected success")

    def addSubTest(self, test, subtest, err) -> None:
        if err is None:
            return
        is_failure = issubclass(err[0], test.failureException)
        detail = str(subtest) + "\n" + format_error(err, is_failure)
        self._report(test, "fail" if is_failure else "error", detail)


def is_excluded(test_id: str, excluded: frozenset) -> bool:
    """Return True if ``test_id`` or one of its dotted prefixes is excluded."""
    parts = test_id.split(".")
    return any(".".join(parts[:n]) in excluded for n in range(1, len(parts) + 1))


def drop_excluded(suite, excluded: frozenset, writer: ResultWriter) -> None:
    """Remove excluded tests from ``suite`` in place, reporting them as skips.

    Suites are edited in place (not rebuilt) so suite subclasses and their
    fixtures keep working.
    """
    tests = getattr(suite, "_tests", None)
    if tests is None:
        return
    kept = []
    for test in tests:
        if isinstance(test, unittest.TestSuite):
            drop_excluded(test, excluded, writer)
            kept.append(test)
            continue
        test_id = normalize_id(test.id())
        if is_excluded(test_id, excluded):
            writer.write(test_id, "skip", "excluded")
        else:
            kept.append(test)
    tests[:] = kept


def configure_support() -> None:
    """Configure ``test.support`` the way regrtest would, if it imports."""
    try:
        from test import support
    except BaseException as exc:
        if isinstance(exc, KeyboardInterrupt):
            raise
        return
    resources = os.environ.get("TESTPY_RESOURCES", "").split(",")
    support.use_resources = [r for r in resources if r]
    support.verbose = False


def run_module(name: str, writer: ResultWriter, excluded: frozenset) -> None:
    """Import, load and run test module ``name``, writing all records."""
    module_id = normalize_id(name)
    configure_support()
    try:
        __import__(name)
        suite = unittest.defaultTestLoader.loadTestsFromName(name)
    except unittest.SkipTest as exc:
        writer.write(module_id, "skip", str(exc))
        return
    except BaseException as exc:
        if isinstance(exc, KeyboardInterrupt):
            raise
        writer.write(module_id, "error",
                     "".join(traceback.format_exception(*sys.exc_info())))
        return
    drop_excluded(suite, excluded, writer)
    suite.run(JsonLinesResult(writer))


def main(argv=None) -> int:
    """Command-line entry point: ``testpy_reporter test.<module>``."""
    args = sys.argv[1:] if argv is None else argv
    results = os.environ.get("TESTPY_RESULTS")
    if len(args) != 1 or not results:
        sys.stderr.write("usage: TESTPY_RESULTS=FILE python -m testpy_reporter "
                         "test.<module>\n")
        return 2
    excluded = frozenset(normalize_id(e.strip())
                         for e in os.environ.get("TESTPY_EXCLUDE", "").split(",")
                         if e.strip())
    writer = ResultWriter(os.path.abspath(results))
    run_module(args[0], writer, excluded)
    writer.write(DONE_ID, "pass", "")
    sys.stdout.flush()
    sys.stderr.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
