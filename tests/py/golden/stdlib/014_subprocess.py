"""L0 golden: subprocess with deterministic POSIX commands only."""
import errno
import subprocess
import sys

r = subprocess.run(["echo", "hi"], capture_output=True, text=True)
print("run echo", repr(r.stdout), repr(r.stderr), r.returncode)
print("args", r.args)
print("repr", repr(r))

r = subprocess.run(["echo", "bytes"], capture_output=True)
print("bytes out", r.stdout, type(r.stdout).__name__)

r = subprocess.run(["false"])
print("false rc", r.returncode, r.stdout)
r = subprocess.run(["true"])
print("true rc", r.returncode)
r = subprocess.run(["sh", "-c", "exit 7"])
print("exit 7", r.returncode)
print("check_returncode ok", subprocess.run(["true"]).check_returncode())

r = subprocess.run(["cat"], input="line1\nline2\n", capture_output=True, text=True)
print("cat input", repr(r.stdout))
r = subprocess.run(["cat"], input=b"\x00\x01binary", capture_output=True)
print("cat bytes", r.stdout)
r = subprocess.run(["tr", "a-z", "A-Z"], input="shout", stdout=subprocess.PIPE, text=True)
print("tr", r.stdout, r.stderr)

try:
    subprocess.run(["sh", "-c", "echo out; echo err >&2; exit 3"], check=True, capture_output=True, text=True)
except subprocess.CalledProcessError as e:
    print("CalledProcessError", e.returncode, e.cmd)
    print("  stdout", repr(e.stdout), "output", repr(e.output))
    print("  stderr", repr(e.stderr))
    print("  str", str(e))
try:
    subprocess.run(["false"], check=True)
except subprocess.CalledProcessError as e:
    print("false check", str(e))
try:
    subprocess.run(["sh", "-c", "kill -TERM $$"], check=True)
except subprocess.CalledProcessError as e:
    print("signal", e.returncode, str(e))

r = subprocess.run("echo $((6 * 7)) | tr 4 X", shell=True, capture_output=True, text=True)
print("shell", repr(r.stdout))
r = subprocess.run("printf '%s-' a b c", shell=True, capture_output=True, text=True)
print("shell printf", repr(r.stdout))
r = subprocess.run("exit 4", shell=True)
print("shell exit", r.returncode)

try:
    subprocess.run(["sleep", "5"], timeout=0.2)
except subprocess.TimeoutExpired as e:
    print("TimeoutExpired", e.cmd, e.timeout, e.output)
    print("  str", str(e))
try:
    subprocess.run(["sh", "-c", "echo early; sleep 5"], timeout=0.5, capture_output=True, text=True)
except subprocess.TimeoutExpired as e:
    print("timeout partial", type(e).__name__, e.timeout)

print("check_output", subprocess.check_output(["echo", "co"]))
print("check_output text", repr(subprocess.check_output(["printf", "a\\nb"], text=True)))
try:
    subprocess.check_output(["sh", "-c", "echo partial; exit 2"])
except subprocess.CalledProcessError as e:
    print("check_output fail", e.returncode, e.output)
print("check_call", subprocess.check_call(["true"]))
print("call", subprocess.call(["sh", "-c", "exit 5"]))
print("getoutput", repr(subprocess.getoutput("echo go; exit 1")))
print("getstatusoutput", subprocess.getstatusoutput("echo gs; exit 3"))

r = subprocess.run(
    ["sh", "-c", "echo to-out; echo to-err >&2"],
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
)
print("stderr->stdout", repr(r.stdout), r.stderr)
r = subprocess.run(["sh", "-c", "echo gone; echo gone2 >&2"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print("devnull", r.stdout, r.stderr, r.returncode)
r = subprocess.run(["sh", "-c", "echo $0 $1 $2", "zero", "one", "two"], capture_output=True, text=True)
print("argv", repr(r.stdout))
r = subprocess.run(["sh", "-c", "echo $FOO"], env={"FOO": "bar", "PATH": "/usr/bin:/bin"}, capture_output=True, text=True)
print("env", repr(r.stdout))

with subprocess.Popen(["cat"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as p:
    out, err = p.communicate("via popen")
print("Popen communicate", repr(out), err, p.returncode)
with subprocess.Popen(["sh", "-c", "exit 9"]) as p:
    print("Popen wait", p.wait(), p.poll())

r = subprocess.run([sys.executable, "-c", "print(6 * 7)"], capture_output=True, text=True)
print("self", r.stdout.strip(), r.returncode)

try:
    subprocess.run(["definitely-not-a-command-xyz"])
except FileNotFoundError as e:
    print("no such command", type(e).__name__, errno.errorcode[e.errno])
try:
    subprocess.run(["echo"], input="x", stdin=subprocess.PIPE)
except ValueError as e:
    print("input+stdin", type(e).__name__, e)
try:
    subprocess.run(["echo"], capture_output=True, stdout=subprocess.PIPE)
except ValueError as e:
    print("capture+stdout", type(e).__name__, e)
print("constants", subprocess.PIPE, subprocess.STDOUT, subprocess.DEVNULL)
