# except clauses: ordering, tuples, `as` name unbinding, try/else, nested try, bare raise.


def classify(exc):
    try:
        raise exc
    except ZeroDivisionError:
        return "ZeroDivisionError branch"
    except ArithmeticError:
        return "ArithmeticError branch"
    except (KeyError, IndexError) as e:
        return "tuple branch " + type(e).__name__
    except Exception as e:
        return "Exception branch " + type(e).__name__


for exc in [ZeroDivisionError(), OverflowError(), KeyError(1), IndexError(), ValueError()]:
    print("classify", classify(exc))


def first_match_wins():
    try:
        raise ZeroDivisionError("z")
    except ArithmeticError:
        return "broad first"
    except ZeroDivisionError:
        return "never"


print("order", first_match_wins())

try:
    1 / 0
except ZeroDivisionError as err:
    print("caught", err)
try:
    err
except NameError as ne:
    print("as name unbound at module", type(ne).__name__, ne)


def unbound_in_function():
    try:
        raise ValueError("x")
    except ValueError as e:
        pass
    try:
        return e
    except UnboundLocalError as ue:
        return type(ue).__name__ + ": " + str(ue)


print("as name unbound in function", unbound_in_function())


def keep_reference():
    saved = None
    try:
        raise ValueError("kept")
    except ValueError as e:
        saved = e
    return saved


print("saved exception", repr(keep_reference()))

e = "preexisting"
try:
    raise TypeError("t")
except TypeError as e:
    pass
print("preexisting name removed", "e" in globals())


def try_else(fail):
    log = []
    try:
        log.append("try")
        if fail:
            raise ValueError
    except ValueError:
        log.append("except")
    else:
        log.append("else")
    finally:
        log.append("finally")
    return log


print("else on success", try_else(False))
print("else skipped on error", try_else(True))


def else_raises():
    try:
        pass
    except ValueError:
        return "not here"
    else:
        raise KeyError("from else")


try:
    else_raises()
except KeyError as e:
    print("error in else not caught by same try", type(e).__name__, e)


def nested():
    log = []
    try:
        try:
            raise KeyError("inner")
        except ValueError:
            log.append("inner wrong type")
        finally:
            log.append("inner finally")
    except KeyError as e:
        log.append("outer caught " + str(e))
    return log


print("nested", nested())
