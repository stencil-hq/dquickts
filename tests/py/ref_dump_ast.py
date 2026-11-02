"""CPython reference implementation of the L2 parser-differential JSON AST protocol.

The parser under test (``./qpy``) and this script speak the same line protocol, so
``tests/py/astdiff.py`` can compare them request by request.  This file runs under host
CPython (3.15), never under the implementation under test.

Usage::

    python3.15 tests/py/ref_dump_ast.py --dump-ast-json-batch < requests.jsonl > responses.jsonl

    # or in-process (what astdiff.py does with ``--ref inproc``):
    from ref_dump_ast import dump_request
    dump_request({"id": "1", "filename": "<x>", "source": "x = 1\\n", "mode": "exec"})

Protocol (one JSON object per line on stdin, exactly one response line per request on
stdout, same order, flushed after every line):

Request::

    {"id": str, "filename": str, "source": str, "mode": "exec",
     "top_level_await": bool (optional), "check": bool (optional)}

- ``top_level_await: true`` parses with ``ast.PyCF_ALLOW_TOP_LEVEL_AWAIT`` (used for
  notebook/corpus cells).
- ``check: true`` compiles the source fully to bytecode instead of stopping at the AST (no
  ``PyCF_ONLY_AST``), which additionally surfaces SyntaxErrors detected by the symbol table
  and the compiler.  The success response then carries no ``"ast"`` key.

Response::

    {"id": ..., "ok": true, "ast": NODE}          # normal request
    {"id": ..., "ok": true}                       # check request
    {"id": ..., "ok": false, "error": {"type": str, "msg": str, "lineno": int|null,
                                       "offset": int|null, "end_lineno": int|null,
                                       "end_offset": int|null}}

``SyntaxError`` and its subclasses report their class name and positions; any other exception
raised by ``compile`` (e.g. ``ValueError: source code string cannot contain null bytes``)
reports its class name, ``str(exc)`` and null positions.  A request line that is not a JSON
object yields an error response with type ``ProtocolError`` and ``id`` null.

NODE encoding: ``{"_type": "<ClassName>", <field>: VALUE ... in _fields order,
<attribute>: int|null ... in _attributes order}``.  VALUE is a NODE, a JSON array of VALUEs,
a string, an int, or null.  ``Constant.value`` and ``MatchSingleton.value`` are tagged:
``{"t":"none"}``, ``{"t":"bool","v":b}``, ``{"t":"int","v":"<decimal>"}``,
``{"t":"float","v":"<float.hex()>"}`` (``"nan"``/``"inf"``/``"-inf"`` for non-finite values),
``{"t":"complex","v":"<hex of imag part>"}``, ``{"t":"str","v":s}``,
``{"t":"bytes","v":"<lowercase hex>"}``, ``{"t":"ellipsis"}``.  Output is ASCII-only JSON, so
lone surrogates appear as ``\\uXXXX`` escapes.
"""

from __future__ import annotations

import argparse
import ast
import json
import math
import sys
import warnings
from typing import Any

# Huge integer literals must round-trip to decimal, and deeply nested sources must encode.
sys.set_int_max_str_digits(0)
sys.setrecursionlimit(max(sys.getrecursionlimit(), 50_000))

_TAGGED_VALUE_TYPES = (ast.Constant, ast.MatchSingleton)


def _float_hex(x: float) -> str:
    """Return the protocol spelling of a float: ``float.hex()`` or nan/inf/-inf."""
    if math.isnan(x):
        return "nan"
    if math.isinf(x):
        return "inf" if x > 0 else "-inf"
    return x.hex()


def encode_constant(value: Any) -> dict[str, Any]:
    """Encode a ``Constant``/``MatchSingleton`` value as a tagged JSON object."""
    if value is None:
        return {"t": "none"}
    if value is Ellipsis:
        return {"t": "ellipsis"}
    if isinstance(value, bool):
        return {"t": "bool", "v": value}
    if isinstance(value, int):
        return {"t": "int", "v": str(value)}
    if isinstance(value, float):
        return {"t": "float", "v": _float_hex(value)}
    if isinstance(value, complex):
        return {"t": "complex", "v": _float_hex(value.imag)}
    if isinstance(value, str):
        return {"t": "str", "v": value}
    if isinstance(value, bytes):
        return {"t": "bytes", "v": value.hex()}
    raise TypeError(f"unsupported constant type {type(value).__name__}")


def encode_node(node: ast.AST) -> dict[str, Any]:
    """Encode an AST node generically over its ``_fields`` and ``_attributes``."""
    cls = type(node)
    out: dict[str, Any] = {"_type": cls.__name__}
    tagged = isinstance(node, _TAGGED_VALUE_TYPES)
    for name in cls._fields:
        value = getattr(node, name, None)
        if tagged and name == "value":
            out[name] = encode_constant(value)
        else:
            out[name] = _encode_value(value)
    for name in cls._attributes:
        out[name] = getattr(node, name, None)
    return out


def _encode_value(value: Any) -> Any:
    """Encode a non-constant field value (node, list, identifier, int or None)."""
    if isinstance(value, ast.AST):
        return encode_node(value)
    if isinstance(value, list):
        return [_encode_value(v) for v in value]
    if value is None or isinstance(value, (str, int)):
        return value
    raise TypeError(f"unsupported AST field value type {type(value).__name__}")


def _error(req_id: Any, exc: BaseException) -> dict[str, Any]:
    """Build an error response for an exception raised while compiling."""
    if isinstance(exc, SyntaxError):
        err = {
            "type": type(exc).__name__,
            "msg": exc.msg,
            "lineno": exc.lineno,
            "offset": exc.offset,
            "end_lineno": exc.end_lineno,
            "end_offset": exc.end_offset,
        }
    else:
        err = {
            "type": type(exc).__name__,
            "msg": str(exc),
            "lineno": None,
            "offset": None,
            "end_lineno": None,
            "end_offset": None,
        }
    return {"id": req_id, "ok": False, "error": err}


def dump_request(req: dict[str, Any]) -> dict[str, Any]:
    """Answer one protocol request; the result is a JSON-serialisable response dict."""
    req_id = req.get("id")
    check = bool(req.get("check"))
    flags = 0 if check else ast.PyCF_ONLY_AST
    if req.get("top_level_await"):
        flags |= ast.PyCF_ALLOW_TOP_LEVEL_AWAIT
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            result = compile(
                req["source"],
                req.get("filename", "<unknown>"),
                req.get("mode", "exec"),
                flags,
                dont_inherit=True,
            )
    except Exception as exc:  # noqa: BLE001 - every compile failure is a protocol answer
        return _error(req_id, exc)
    if check:
        return {"id": req_id, "ok": True}
    return {"id": req_id, "ok": True, "ast": encode_node(result)}


def serve_batch(stdin: Any = None, stdout: Any = None) -> None:
    """Run the line protocol: one response line per request line until EOF."""
    stdin = stdin if stdin is not None else sys.stdin
    stdout = stdout if stdout is not None else sys.stdout
    for line in stdin:
        if not line.strip():
            continue
        try:
            req = json.loads(line)
            if not isinstance(req, dict) or not isinstance(req.get("source"), str):
                raise ValueError("request must be a JSON object with a string 'source'")
        except ValueError as exc:
            resp = {
                "id": None,
                "ok": False,
                "error": {"type": "ProtocolError", "msg": str(exc), "lineno": None,
                          "offset": None, "end_lineno": None, "end_offset": None},
            }
        else:
            resp = dump_request(req)
        stdout.write(json.dumps(resp, ensure_ascii=True, separators=(",", ":")) + "\n")
        stdout.flush()


def main(argv: list[str] | None = None) -> int:
    """Command-line entry point."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dump-ast-json-batch", action="store_true", required=True,
                        help="serve the JSON-lines AST protocol on stdin/stdout")
    parser.parse_args(argv)
    sys.stdin.reconfigure(encoding="utf-8", errors="surrogateescape")
    serve_batch()
    return 0


if __name__ == "__main__":
    sys.exit(main())
