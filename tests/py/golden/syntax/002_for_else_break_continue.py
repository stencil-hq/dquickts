# for/else, break, continue, and their interaction.

def find(seq, target):
    for i, x in enumerate(seq):
        if x == target:
            print("  found at", i)
            break
    else:
        print("  not found")
        return -1
    return i

print("find hit", find([4, 5, 6], 5))
print("find miss", find([4, 5, 6], 9))
print("find empty", find([], 1))

# else runs when loop completes normally
for i in range(2):
    print("body", i)
else:
    print("else ran, i =", i)

# else runs for an empty loop
for i in []:
    print("never")
else:
    print("else on empty")

# continue does not skip else
for i in range(3):
    if i == 1:
        continue
    print("continue body", i)
else:
    print("else after continue")

# continue skips the rest of the body
out = []
for i in range(10):
    if i % 3:
        continue
    out.append(i)
print("multiples of 3", out)

# break skips else
for i in range(5):
    if i == 2:
        break
else:
    print("never printed")
print("break at", i)

# break in nested loop only exits inner loop
pairs = []
for a in range(3):
    for b in range(3):
        if b > a:
            break
        pairs.append((a, b))
    else:
        pairs.append(("inner-else", a))
print("nested break", pairs)

# continue in inner loop
res = []
for a in range(3):
    for b in range(3):
        if a == b:
            continue
        res.append(a * 10 + b)
print("inner continue", res)

# break inside try/finally: finally still runs
log = []
for i in range(3):
    try:
        if i == 1:
            break
        log.append(("try", i))
    finally:
        log.append(("finally", i))
print("break finally", log)

# continue inside try/finally
log = []
for i in range(3):
    try:
        if i % 2 == 0:
            continue
        log.append(("odd", i))
    finally:
        log.append(("fin", i))
print("continue finally", log)

# break out of while/else inside for
found = None
for n in range(2, 20):
    d = 2
    while d * d <= n:
        if n % d == 0:
            break
        d += 1
    else:
        if n > 10:
            found = n
            break
print("first prime > 10", found)

# primes via for/else
primes = []
for n in range(2, 30):
    for p in primes:
        if n % p == 0:
            break
    else:
        primes.append(n)
print("primes", primes)

# else not run if exception escapes
try:
    for i in range(3):
        if i == 1:
            raise ValueError("boom at 1")
    else:
        print("never")
except ValueError as e:
    print("exception escapes", type(e).__name__, e)

# break from loop over a generator closes nothing visible but stops consuming
consumed = []
def g():
    for i in range(5):
        consumed.append(i)
        yield i

for x in g():
    if x == 2:
        break
print("consumed until break", consumed)

# loop else with return in body
def first_even(seq):
    for x in seq:
        if x % 2 == 0:
            return x
    else:
        return None

print("first even", first_even([1, 3, 4, 6]))
print("first even none", first_even([1, 3]))
