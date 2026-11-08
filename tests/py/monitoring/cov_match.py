# coverage sample (run.sh): match: sequence, or, mapping, class patterns, guards, wildcard
def m(cmd):
    match cmd:
        case ["go", direction]:
            return direction
        case ["look"] | ["l"]:
            return "look"
        case {"x": x, **rest} if x > 0:
            return x
        case int(n) if n > 10:
            return "big"
        case int():
            return "int"
        case _:
            return None

def m2(p):
    match p:
        case (0, 0):
            r = "origin"
        case (x, 0):
            r = "x-axis"
        case _:
            r = "other"
    return r

for c in [["go", "n"], ["l"], {"x": 1}, {"x": 0}, 50, 5, "z"]:
    print(m(c))
print(m2((0, 0)), m2((1, 0)))
