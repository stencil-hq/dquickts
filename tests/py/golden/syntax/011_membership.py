# Membership tests: in / not in on str, list, tuple, dict, set, range, bytes, custom.

print("substr", "ell" in "hello", "hello" in "hello", "" in "abc", "" in "")
print("substr miss", "xyz" in "hello", "Hello" in "hello", "lo!" in "hello")
print("not in str", "z" not in "abc", "a" not in "abc")
print("list in", 2 in [1, 2, 3], 4 in [1, 2, 3], [1] in [[1], 2])
print("list eq based", 1.0 in [1], True in [1], 0 in [False])
print("tuple in", "a" in ("a", "b"), None in (None,), () in ((),))
print("dict keys", "a" in {"a": 1}, 1 in {"a": 1}, "missing" not in {"a": 1})
print("dict values", 1 in {"a": 1}.values(), ("a", 1) in {"a": 1}.items())
print("set in", 3 in {1, 2, 3}, 3.0 in {3}, "x" in set())
print("frozenset in", 1 in frozenset([1]))
print("range in", 5 in range(10), 10 in range(10), 4 in range(0, 10, 2), 5 in range(0, 10, 2))
print("range neg", -3 in range(0, -10, -1), -10 in range(0, -10, -1))
print("range float", 3.0 in range(5), 3.5 in range(5))
print("range big", 10**18 in range(0, 10**20, 10**6), 10**18 + 1 in range(0, 10**20, 10**6))
print("bytes in", b"b" in b"abc", 98 in b"abc", 200 in b"abc", b"" in b"x")
print("bytearray in", b"bc" in bytearray(b"abcd"), 97 in bytearray(b"a"))

# nan identity shortcut: containers check identity before equality
nan = float("nan")
print("nan same object", nan in [nan], float("nan") in [float("nan")])

# errors
def test(label, fn):
    try:
        print(label, fn())
    except Exception as e:
        print(label, type(e).__name__, e)

test("int in str", lambda: 1 in "123")
test("list in set", lambda: [1] in {1})
test("in int", lambda: 1 in 5)
test("in None", lambda: None in None)
test("dict key unhashable", lambda: [] in {})
test("bytes in str", lambda: b"a" in "abc")
test("str in bytes", lambda: "a" in b"abc")
test("256 in bytes", lambda: 256 in b"abc")
test("float in bytes", lambda: 1.5 in b"abc")

# custom __contains__
class Evens:
    def __contains__(self, x):
        print("  __contains__", x)
        return x % 2 == 0

print("contains", 4 in Evens(), 3 in Evens(), 3 not in Evens())

class ContainsReturnsInt:
    def __contains__(self, x):
        return 42

print("contains non-bool coerced", 1 in ContainsReturnsInt())

# fallback to __iter__
class It:
    def __iter__(self):
        print("  __iter__ called")
        return iter([1, 2, 3])

print("iter fallback", 2 in It(), 5 in It())

# fallback to __getitem__
class GI:
    def __getitem__(self, i):
        if i > 4:
            raise IndexError
        return i * 10

print("getitem fallback", 30 in GI(), 35 in GI())

# membership consumes iterators
it = iter([1, 2, 3, 4])
print("iterator in", 2 in it, list(it))
gen = (x * x for x in range(10))
print("generator in", 9 in gen, next(gen))

# precedence: in binds like comparisons and chains
print("in chain", 1 in [1] in [[1]], 1 in [1] == True)
print("not in vs not", not 1 in [2], (not 1) in [False])

# membership in strings is substring, not element
print("multi-char", "ab" in ["a", "b"], "ab" in "cab")
print("case sensitive", "A" in "abc")
print("unicode", "é" in "café", "\u00e9" in "cafe\u0301")
