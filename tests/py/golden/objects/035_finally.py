# finally semantics: runs on normal exit, exceptions, return, break and continue;
# return/break inside finally overriding (PEP 765 warns at compile time; stderr only).

log = []


def ret_in_try():
    try:
        log.append("try")
        return "from try"
    finally:
        log.append("finally")


print("return in try", ret_in_try(), log)


def ret_value_evaluated_before_finally():
    x = ["before"]
    try:
        return x
    finally:
        x.append("mutated in finally")


print("returned object mutated", ret_value_evaluated_before_finally())


def rebinding_in_finally():
    x = 1
    try:
        return x
    finally:
        x = 2


print("rebinding does not change return", rebinding_in_finally())


def finally_overrides_return():
    try:
        return "try"
    finally:
        return "finally"


print("finally return overrides", finally_overrides_return())


def finally_swallows_exception():
    try:
        raise ValueError("lost")
    finally:
        return "swallowed"


print("finally return swallows", finally_swallows_exception())

log.clear()
for i in range(5):
    try:
        if i == 1:
            continue
        if i == 3:
            break
        log.append("body " + str(i))
    finally:
        log.append("finally " + str(i))
print("loop break continue", log)


def break_in_finally():
    for _ in range(3):
        try:
            raise KeyError("dropped")
        finally:
            break
    return "loop exited"


print("break in finally drops exception", break_in_finally())

log.clear()
try:
    try:
        raise ValueError("propagating")
    finally:
        log.append("finally ran during propagation")
except ValueError as e:
    log.append("caught " + str(e))
print("exception path", log)


def finally_raises():
    try:
        raise ValueError("original")
    finally:
        raise TypeError("replacement")


try:
    finally_raises()
except TypeError as e:
    print("finally exception replaces", e, "context", repr(e.__context__))


def nested_finally():
    out = []
    try:
        try:
            return out
        finally:
            out.append("inner")
    finally:
        out.append("outer")


print("nested finally order", nested_finally())


def while_finally():
    n = 0
    while True:
        try:
            n += 1
            if n < 3:
                continue
            return n
        finally:
            log.append("iter " + str(n))


log.clear()
print("while finally", while_finally(), log)


def finally_with_else():
    out = []
    try:
        out.append("try")
    except Exception:
        out.append("except")
    else:
        out.append("else")
        return out
    finally:
        out.append("finally")


print("else return then finally", finally_with_else())
