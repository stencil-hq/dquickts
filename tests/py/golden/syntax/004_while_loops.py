# while loops: conditions, else, break, continue, nested, truthiness conditions.

n = 0
while n < 3:
    print("while n", n)
    n += 1
print("after while n", n)

# while/else runs when condition becomes false
n = 0
while n < 2:
    n += 1
else:
    print("while else n", n)

# else runs even when the loop body never executes
while False:
    print("never")
else:
    print("while else on false")

# break skips else
n = 0
while True:
    n += 1
    if n == 4:
        break
else:
    print("never")
print("break while true at", n)

# continue
n = 0
odds = []
while n < 10:
    n += 1
    if n % 2 == 0:
        continue
    odds.append(n)
print("odds", odds)

# truthy container conditions
stack = [1, 2, 3]
popped = []
while stack:
    popped.append(stack.pop())
print("pop until empty", popped, stack)

s = "abc"
while s:
    print("shrink str", repr(s))
    s = s[1:]

d = {"a": 1, "b": 2}
while d:
    k, v = d.popitem()
    print("popitem", k, v)

# countdown with multiple conditions
a, b = 10, 0
while a > b and a - b > 3:
    a -= 2
    b += 1
print("multi cond", a, b)

# nested while with break of inner only
i = 0
grid = []
while i < 3:
    j = 0
    while True:
        if j > i:
            break
        grid.append((i, j))
        j += 1
    i += 1
print("nested while", grid)

# collatz length
def collatz_len(x):
    steps = 0
    while x != 1:
        x = x // 2 if x % 2 == 0 else 3 * x + 1
        steps += 1
    return steps

print("collatz 27", collatz_len(27))
print("collatz 1", collatz_len(1))

# gcd
def gcd(a, b):
    while b:
        a, b = b, a % b
    return a

print("gcd 48 18", gcd(48, 18))
print("gcd 17 5", gcd(17, 5))
print("gcd 0 5", gcd(0, 5))

# while with return in body
def index_of(seq, x):
    i = 0
    while i < len(seq):
        if seq[i] == x:
            return i
        i += 1
    return -1

print("index_of hit", index_of("hello", "l"))
print("index_of miss", index_of("hello", "z"))

# while with try/finally and break
log = []
k = 0
while True:
    try:
        k += 1
        if k == 3:
            break
    finally:
        log.append(k)
print("while finally", log)

# exception in condition propagates
def cond():
    raise RuntimeError("bad condition")

try:
    while cond():
        pass
except RuntimeError as e:
    print("cond raises", type(e).__name__, e)

# condition evaluated each iteration
calls = []
def check(v):
    calls.append(v)
    return v < 3

v = 0
while check(v):
    v += 1
print("cond calls", calls)
