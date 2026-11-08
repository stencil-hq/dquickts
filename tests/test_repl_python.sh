#!/bin/sh
# The REPL of qjs is bilingual: \py and \js switch the language of the
# inputs, '\py CODE' and '\js CODE' run one input in the other language.
#
#   tests/test_repl_python.sh [path/to/qjs]
#
# The commands come from a pipe (the line editor then reads plain lines).

QJS=${1:-./qjs}
dir=$(mktemp -d "${TMPDIR:-/tmp}/repl_py.XXXXXX") || exit 1
trap 'rm -rf "$dir"' EXIT
out=$dir/out

fail() {
    echo "FAIL: $*"
    echo "--- output:"
    cat "$out"
    exit 1
}

# expect TEXT: the output of the last run has a line containing TEXT
expect() {
    grep -qF -- "$1" "$out" || fail "missing: $1"
}

# run: a REPL reading the commands on stdin
run() {
    "$QJS" > "$out" 2>&1
}

# 1. JS and Python share the runtime: js.NAME reads the JS globals
# (top-level let, const and class too), py is __main__ in JS. Python
# starts with the first Python input; the multi-line inputs, the
# tracebacks and the asynchronous JS results keep their order.
run <<'EOF'
typeof Object.getOwnPropertyDescriptor(globalThis, "py").get
\js function add(a, b) { return a + b }
\py js.add(2, 3)
\py def sq(x): return x * x

\js py.sq(7)
let n = 3
\py js.n
class K { v = 9 }
\py
import sys
x = 10
for i in range(3):
    x += i

x
print("K:", js.K.new().v, "ps1:", sys.ps1 == ">>> ")
1/0
print("after")
\js
await new Promise((resolve) => os.setTimeout(() => resolve(py.x * 2), 10))
py.y = 1
\py y + 1
EOF
[ $? -eq 0 ] || fail "exit status"
expect 'qjs> "function"'
expect 'qjs> 5'
expect 'qjs> 49'
expect 'qjs> 3'
expect '>>> 13'
expect '>>> K: 9 ps1: True'
expect 'Traceback (most recent call last):'
expect 'ZeroDivisionError: division by zero'
expect '>>> after'
expect 'qjs> 26'
expect 'qjs> 2'

# 2. py works before any Python input; \h lists the commands; SystemExit
# ends qjs with its status
printf 'py.z = 7\n\\py z * 6\n\\h\n\\py raise SystemExit(4)\n' | run
[ $? -eq 4 ] || fail "SystemExit status"
expect 'qjs> 42'
expect '\py [code]'
expect '\js [code]'

echo "test_repl_python: OK"
