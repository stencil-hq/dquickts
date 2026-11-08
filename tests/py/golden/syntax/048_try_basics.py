# try/except/else/finally: clause ordering basics.

def run(label, fn):
    log = []
    try:
        result = fn(log)
        print(label, "->", repr(result), log)
    except Exception as e:
        print(label, "raised", type(e).__name__, e, log)

def no_error(log):
    try:
        log.append("try")
    except ValueError:
        log.append("except")
    else:
        log.append("else")
    finally:
        log.append("finally")
    return "done"

def with_error(log):
    try:
        log.append("try")
        raise ValueError("bad")
    except ValueError as e:
        log.append("except " + str(e))
    else:
        log.append("else")
    finally:
        log.append("finally")
    return "done"

def unhandled(log):
    try:
        log.append("try")
        raise KeyError("k")
    except ValueError:
        log.append("except")
    finally:
        log.append("finally")
    return "never"

def return_in_try(log):
    try:
        log.append("try")
        return "from try"
    finally:
        log.append("finally")

def return_in_except(log):
    try:
        1 / 0
    except ZeroDivisionError:
        log.append("except")
        return "from except"
    finally:
        log.append("finally")

def else_skipped_by_return(log):
    try:
        return "early"
    except Exception:
        log.append("except never")
    else:
        log.append("else never")
    finally:
        log.append("finally")

def error_in_else(log):
    try:
        log.append("try")
    except Exception:
        log.append("except never (else errors are not caught here)")
    else:
        log.append("else")
        raise RuntimeError("from else")
    finally:
        log.append("finally")

def error_in_finally(log):
    try:
        raise ValueError("original")
    finally:
        log.append("finally")
        raise TypeError("from finally")

run("no error", no_error)
run("with error", with_error)
run("unhandled", unhandled)
run("return in try", return_in_try)
run("return in except", return_in_except)
run("else skipped by return", else_skipped_by_return)
run("error in else", error_in_else)
run("error in finally", error_in_finally)

# multiple except clauses: first matching wins
def which(exc):
    try:
        raise exc
    except (KeyError, IndexError) as e:
        return "lookup-ish " + type(e).__name__
    except ArithmeticError as e:
        return "arith " + type(e).__name__
    except Exception as e:
        return "generic " + type(e).__name__

for exc in [KeyError("a"), IndexError(), ZeroDivisionError(), OverflowError(), ValueError(), LookupError()]:
    print("which", which(exc))

# PEP 758: except without parentheses for multiple types
try:
    raise IndexError("idx")
except KeyError, IndexError:
    print("unparenthesized tuple except")

# except variable is deleted after the block
try:
    raise ValueError("temp")
except ValueError as err:
    print("inside", err)
try:
    err
except NameError as e:
    print("except target cleared", type(e).__name__, e)

# bare except and re-raise
def reraise():
    try:
        raise ValueError("again")
    except:
        print("  bare except caught")
        raise

try:
    reraise()
except ValueError as e:
    print("reraised", type(e).__name__, e)

# finally runs on loop control
for i in range(3):
    try:
        if i == 1:
            continue
        if i == 2:
            break
    finally:
        print("finally on iteration", i)
