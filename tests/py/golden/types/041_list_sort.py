"""L0 golden: list.sort and sorted.

Sorting is stable (also with reverse=True, which keeps equal elements in
original order), key functions are called once per element, tuple keys sort
lexicographically, and comparing incompatible types raises TypeError.

Usage: python3.15 -I -X utf8 tests/py/golden/types/041_list_sort.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


class Counted:
    """Key wrapper that counts how many times the key function is called."""

    calls = 0

    @classmethod
    def key(cls, value: int) -> int:
        cls.calls += 1
        return -value


records = [("bob", 25), ("alice", 30), ("carol", 25), ("dave", 30), ("eve", 22), ("frank", 25)]
show("sorted by age (stable)", lambda: sorted(records, key=lambda r: r[1]))
show("sorted by age reverse (stable)", lambda: sorted(records, key=lambda r: r[1], reverse=True))
show("reversed(sorted) differs", lambda: list(reversed(sorted(records, key=lambda r: r[1]))))
show("sorted tuple key", lambda: sorted(records, key=lambda r: (-r[1], r[0])))
show("sorted default tuples", lambda: sorted(records))
show("sorted by len then alpha", lambda: sorted(["bb", "a", "ccc", "aa", "b"], key=lambda s: (len(s), s)))
show("two-pass stable sort", lambda: sorted(sorted(records), key=lambda r: r[1]))

nums = [3, 1, -2, 10, 0, -2, 7]
show("sorted", lambda: sorted(nums))
show("sorted reverse", lambda: sorted(nums, reverse=True))
show("sorted abs", lambda: sorted(nums, key=abs))
show("sorted abs reverse", lambda: sorted(nums, key=abs, reverse=True))
show("original untouched", lambda: nums)
show("sort returns None", lambda: nums.sort())
show("after sort", lambda: nums)
nums.sort(key=lambda v: v % 3)
show("sort by mod 3 stable", lambda: nums)
nums.sort(reverse=True)
show("sort reverse", lambda: nums)
Counted.calls = 0
show("key once per element", lambda: (sorted(range(10), key=Counted.key), Counted.calls))
show("sorted str", lambda: sorted("banana"))
show("sorted dict", lambda: sorted({"b": 1, "a": 2}))
show("sorted dict items by value", lambda: sorted({"b": 1, "a": 2, "c": 0}.items(), key=lambda kv: kv[1]))
show("sorted empty", lambda: sorted([]))
show("sorted floats", lambda: sorted([2.5, -0.0, 0.0, 1, True, -1e300]))
show("sorted mixed int str", lambda: sorted([1, "a", 2]))
show("sorted None", lambda: sorted([None, 1]))
show("sorted lists", lambda: sorted([[2], [1, 5], [1], []]))
show("sorted list vs tuple", lambda: sorted([[1], (1,)]))
show("sorted complex", lambda: sorted([1j, 2j]))
show("sorted key=None", lambda: sorted([3, 1], key=None))
show("sorted positional key", lambda: sorted([3, 1], len))
show("sort positional key", lambda: [3, 1].sort(len))
show("sorted bad key", lambda: sorted([1, 2], key=5))
show("sorted key raises", lambda: sorted([1, 0], key=lambda v: 1 // v))
show("sorted reverse int", lambda: sorted([1, 3, 2], reverse=1))
show("sorted reverse str", lambda: sorted([1, 3, 2], reverse="yes"))
show("sorted generator", lambda: sorted(x * x for x in range(-3, 4)))
show("sorted by str", lambda: sorted([10, 9, 100, 1], key=str))
show("sorted case-insensitive", lambda: sorted(["b", "A", "a", "B"], key=str.lower))
show("sorted bytes", lambda: sorted([b"b", b"A", b"a"]))
show("sorted with nan stays", lambda: len(sorted([3.0, float("nan"), 1.0])))
show("sorted bool", lambda: sorted([True, False, True]))
show("sorted big ints", lambda: sorted([2 ** 64, -(2 ** 64), 2 ** 63, 0]))

data = [5, 1, 4]


def bad_key(value: int) -> int:
    """Key function that mutates the list being sorted."""
    data.append(value)
    return value


show("mutation during sort", lambda: data.sort(key=bad_key))
show("list after mutation error", lambda: data)
