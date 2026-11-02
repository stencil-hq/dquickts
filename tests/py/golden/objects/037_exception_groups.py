# ExceptionGroup / BaseExceptionGroup, except* splitting, subgroup/split/derive, nesting.

eg = ExceptionGroup("batch", [ValueError(1), TypeError("t"), ValueError(2)])
print("str", str(eg))
print("repr", repr(eg))
print("message", eg.message)
print("exceptions", eg.exceptions)
print("type", type(eg).__name__, isinstance(eg, Exception))

try:
    raise eg
except* ValueError as group:
    print("except* ValueError", repr(group))
except* TypeError as group:
    print("except* TypeError", repr(group))


def partial_handle():
    try:
        raise ExceptionGroup("mixed", [KeyError("k"), OSError("o"), KeyError("k2")])
    except* KeyError as g:
        print("handled keys", [str(x) for x in g.exceptions])


try:
    partial_handle()
except ExceptionGroup as rest:
    print("unhandled remainder", repr(rest))

try:
    raise ValueError("naked")
except* ValueError as g:
    print("naked exception wrapped", type(g).__name__, repr(g.exceptions), g.message == "")

try:
    raise ExceptionGroup("tuple", [ValueError(), TypeError(), KeyError()])
except* (ValueError, TypeError) as g:
    print("tuple except*", [type(x).__name__ for x in g.exceptions])
except* LookupError as g:
    print("LookupError except*", [type(x).__name__ for x in g.exceptions])

nested = ExceptionGroup("outer", [ValueError("a"), ExceptionGroup("inner", [TypeError("b"), ValueError("c")])])
print("nested repr", repr(nested))
try:
    raise nested
except* ValueError as g:
    print("nested ValueErrors keep structure", repr(g))
except* TypeError as g:
    print("nested TypeErrors", repr(g))

vals, others = nested.split(ValueError)
print("split match", repr(vals))
print("split rest", repr(others))
print("subgroup", repr(nested.subgroup(TypeError)))
print("subgroup none", nested.subgroup(KeyError))
print("subgroup predicate", repr(nested.subgroup(lambda e: str(e) == "c")))
print("split all", nested.split(Exception)[0] is nested, nested.split(Exception)[1])

for args in [("msg", []), ("msg", [1]), (1, [ValueError()]), ("msg", ValueError())]:
    try:
        ExceptionGroup(*args)
    except (TypeError, ValueError) as e:
        print("bad ctor", type(e).__name__, e)

beg = BaseExceptionGroup("base", [KeyboardInterrupt()])
print("BaseExceptionGroup with base exc", type(beg).__name__)
auto = BaseExceptionGroup("auto", [ValueError()])
print("BaseExceptionGroup auto-narrows", type(auto).__name__)
try:
    ExceptionGroup("x", [KeyboardInterrupt()])
except TypeError as e:
    print(type(e).__name__, e)

try:
    try:
        raise ExceptionGroup("g", [ValueError("v")])
    except* ValueError:
        raise KeyError("raised in handler")
except (ExceptionGroup, KeyError) as e:
    print("raise in except* handler", type(e).__name__, e, "context", repr(e.__context__))

try:
    try:
        raise ExceptionGroup("g", [ValueError("v")])
    except* ValueError:
        raise
except ExceptionGroup as e:
    print("bare raise in except*", repr(e))

try:
    try:
        raise ExceptionGroup("g", [OSError("o")])
    except* OSError:
        pass
    print("fully handled continues")
except ExceptionGroup:
    print("must not happen")


class MyGroup(ExceptionGroup):
    def derive(self, excs):
        return MyGroup(self.message + "!", excs)


mg = MyGroup("custom", [ValueError(1), TypeError(2)])
print("custom derive", repr(mg.split(ValueError)[0]), type(mg.subgroup(TypeError)).__name__)

try:
    try:
        raise ExceptionGroup("e", [ValueError()])
    except* ExceptionGroup:
        print("must not happen")
except TypeError as e:
    print("except* ExceptionGroup invalid", type(e).__name__, e)

try:
    compile("try:\n    pass\nexcept* ValueError:\n    pass\nexcept TypeError:\n    pass\n", "<mix>", "exec")
except SyntaxError as e:
    print("mixing except and except*", type(e).__name__, e.msg, e.lineno)
