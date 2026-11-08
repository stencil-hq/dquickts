# coverage sample (run.sh): if/elif, for/else with break/continue, while, try/except, conditional expression, generator
def classify(n):
    if n < 0:
        return "neg"
    elif n == 0:
        return "zero"
    for i in range(n):
        if i % 2:
            continue
        if i > 10:
            break
    else:
        pass
    while n > 3:
        n -= 2
    try:
        if n == 3:
            raise ValueError(n)
    except ValueError:
        n = -n
    return "pos" if n > 0 else "odd"

def gen(k):
    for j in range(k):
        if j:
            yield j

print(classify(-1), classify(0), classify(4), classify(3), list(gen(3)))
