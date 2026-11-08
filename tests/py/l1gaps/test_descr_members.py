# Attributes CPython defines as PyMemberDef are member_descriptors (repr,
# readonly errors, T_BOOL set/delete errors); types whose getattro/setattro
# is the generic one have no __getattribute__/__setattr__ slot wrappers, and
# object.__setattr__ / object.__getattribute__ reach their instance dict.
import sys
import types


def f():
    pass


def g():
    yield


async def co():
    pass


coro = co()
SKIP = {"co_branches", "__del__"}   # 3.15 additions not implemented yet
for T in [types.FunctionType, types.MethodType, types.BuiltinFunctionType, types.CodeType,
          types.ModuleType, types.GeneratorType, types.CoroutineType, types.FrameType,
          types.CellType, types.MemberDescriptorType, types.GetSetDescriptorType,
          types.MethodDescriptorType, types.WrapperDescriptorType, types.MethodWrapperType,
          staticmethod, classmethod, property, types.SimpleNamespace, BaseException,
          types.TracebackType]:
    print(T.__name__)
    for k, v in sorted(T.__dict__.items()):
        if k not in SKIP:
            print("  ", k, type(v).__name__)
coro.close()

print(repr(types.FunctionType.__dict__["__globals__"]))
print(repr(types.CodeType.__dict__["co_name"]), types.CodeType.__dict__["co_name"].__doc__)
print(repr(types.MethodType.__dict__["__func__"]), types.MethodType.__dict__["__func__"].__doc__)
print(repr(type(str.upper).__dict__["__name__"]))

f.x = 1
print(f.x, f.__dict__)
object.__setattr__(f, "y", 2)
print(f.y, object.__getattribute__(f, "y"))
object.__delattr__(f, "y")
for stmt in ["f.__globals__ = {}", "del f.__closure__", "f.__code__.co_name = 'x'",
             "str.upper.__name__ = 'x'", "property().fget = 1",
             "object.__getattribute__(f, 'nope')", "object.__delattr__(f, 'nope')"]:
    try:
        exec(stmt)
        print(stmt, "ok")
    except Exception as e:
        print(stmt, type(e).__name__, e)
try:
    types.FunctionType.__dict__["__closure__"].__get__(1)
except TypeError as e:
    print(e)


class S(staticmethod):
    def __getattribute__(self, n):
        return super().__getattribute__(n)

    def __setattr__(self, n, v):
        super().__setattr__(n, v)


s = S(f)
s.z = 3
print(s.z, s.__func__ is f, sorted(s.__dict__))


class M(types.ModuleType):
    def __setattr__(self, n, v):
        super().__setattr__(n, v)

    def __getattr__(self, n):
        return "dyn"


m = M("m")
m.a = 5
print(m.a, m.__dict__["a"], m.qq)
del m.a
try:
    del m.a
except AttributeError as e:
    print(e, e.name, e.obj is m)
mod = types.ModuleType("mm")
object.__setattr__(mod, "b", 1)
print(mod.b)
for stmt in ["object.__getattribute__(mod, 'nope')", "object.__delattr__(mod, 'nope')",
             "mod.nope"]:
    try:
        exec(stmt)
    except AttributeError as e:
        print(stmt, e, e.name)

fr = sys._getframe()
fr.f_trace_lines = False
print(fr.f_trace_lines)
for stmt in ["fr.f_trace_lines = 1", "del fr.f_trace_lines", "del fr.f_trace_opcodes",
             "del fr.f_lineno"]:
    try:
        exec(stmt)
    except Exception as e:
        print(stmt, type(e).__name__, e)
e = ValueError()
for stmt in ["e.__suppress_context__ = 1", "del e.__suppress_context__"]:
    try:
        exec(stmt)
    except Exception as x:
        print(stmt, type(x).__name__, x)
p = property(doc="d")
p.__doc__ = "e"
print(p.__doc__)
print(len.__module__)
len.__module__ = "x"
print(len.__module__)
del len.__module__
print(len.__module__)
