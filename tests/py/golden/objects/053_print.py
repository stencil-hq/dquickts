# print(): sep, end, flush, empty calls, non-string objects, keyword errors.

print()
print("plain")
print("a", "b", "c")
print("a", "b", "c", sep="")
print("a", "b", "c", sep=", ")
print("a", "b", sep="\n")
print("no newline", end="")
print(" <- continued")
print("custom end", end=" |END|\n")
print("multi", "args", sep="-", end="!\n")
print(end="")
print("sep None", "x", sep=None)
print("end None", end=None)
print("flush", flush=True)
print("flush false", flush=False)
print(sep="ignored")
print("", "", sep="[]")
print("empty string arg", "")
print(1, 2.5, None, True, [1, "a"], (1,), {"k": "v"}, b"bytes")
print("nested str inside containers uses repr", ["it's", 'say "hi"'])
print("unicode", "héllo", "世界", "emoji 👍", "\u00e9")
print("escapes", "tab\there", "quote\"s", "back\\slash")
print("multi-line\nstring")
print(*range(5))
print(*range(5), sep=",")
print(*"abc", sep="|", end=".\n")
print(*[])
print("star empty above")


class Shown:
    def __str__(self):
        return "Shown.__str__"

    def __repr__(self):
        return "Shown.__repr__"


print(Shown(), [Shown()])

for bad in [{"sep": 5}, {"end": 1}, {"sep": b","}, {"bogus": 1}]:
    try:
        print("x", **bad)
    except TypeError as e:
        print(type(e).__name__, e)


class BadStr:
    def __str__(self):
        raise ValueError("str failed")


try:
    print("before", BadStr(), "after")
except ValueError as e:
    print(type(e).__name__, e)

print("file None goes to stdout", file=None)
result = print("print returns", end=" ")
print(repr(result))

for i in range(3):
    print(i, end=" " if i < 2 else "\n")

lines = ["one", "two", "three"]
print("\n".join(lines))
print("%s=%d" % ("x", 5), "{}={}".format("y", 6), f"{'z'}={7}")
print("width", f"[{'ab':>5}]", f"[{'ab':<5}]", f"[{'ab':^6}]", f"[{42:05}]")
print("big int", 2**100)
print("float formats", 0.1, 1e16, 1e-7, 123456789.0, 1.5e300, -0.0, float("inf"), float("nan"))
print("complex", 1j, 1 + 2j, -1.5j, complex(0, -0.0))
print("sep with unicode", "a", "b", sep=" → ")
print("long", "x" * 80)
print(chr(0x1F600), "\N{BULLET}", "\x00".encode())
print("bool sep", "a", "b", sep="" or "-")
print("end empty then print()", end="")
print()
print("done")
