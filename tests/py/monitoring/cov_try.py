# coverage sample (run.sh): try/except/else/finally, finally in loops with break/continue, with, return through finally
def f(x):
    try:
        if x == 1:
            return "one"
        elif x == 2:
            raise KeyError(x)
        y = 10 // x
    except ZeroDivisionError:
        y = -1
    except KeyError as e:
        y = -2
    else:
        y += 1
    finally:
        x = None
    return y

def g(xs):
    total = 0
    for x in xs:
        try:
            if x < 0:
                continue
            if x > 100:
                break
            total += x
        finally:
            total += 1
    return total

def h(v):
    with open(__file__) as fh:
        if v:
            return fh.readline()
    try:
        raise ValueError
    except (TypeError, ValueError):
        pass
    try:
        pass
    finally:
        pass
    return None

print(f(0), f(1), f(2), f(3), g([1, -1, 2]), g([200]), g([]), h(0), h(1)[:5])
