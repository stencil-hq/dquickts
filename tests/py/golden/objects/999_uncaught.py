# Uncaught exception at top level: stdout up to the failure is kept, the process exits with
# status 1, and the last stderr line is the exception summary (`ValueError: boom`).


def validate(value):
    if value < 0:
        raise ValueError("boom")
    return value * 2


print("start")
results = []
for v in [3, 2, 1, 0]:
    results.append(validate(v))
    print("validated", v, "->", results[-1])

try:
    validate(-5)
except ValueError as e:
    print("first failure handled:", type(e).__name__, e)


class Stage:
    def __init__(self, name):
        self.name = name

    def __enter__(self):
        print("enter", self.name)
        return self

    def __exit__(self, exc_type, exc, tb):
        print("exit", self.name, exc_type.__name__ if exc_type else None)
        return False


def pipeline(values):
    out = []
    for value in values:
        out.append(validate(value))
    return out


print("pipeline ok", pipeline([1, 2, 3]))
print("results so far", results)
total = 0
for r in results:
    total += r
print("total", total)


def level3():
    validate(-1)


def level2():
    try:
        level3()
    finally:
        print("finally in level2 runs before exit")


def level1():
    with Stage("outer"):
        level2()


print("about to fail")
for n in range(3):
    print("countdown", 3 - n)
level1()
print("never printed")
