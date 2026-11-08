"""L0 golden: itertools building blocks."""
import itertools as it
import operator

print("chain", list(it.chain([1, 2], (3,), "ab")))
print("chain.from_iterable", list(it.chain.from_iterable([[1], [], [2, 3]])))
print("islice stop", list(it.islice("abcdefg", 3)))
print("islice start stop step", list(it.islice(range(20), 2, 12, 3)))
print("islice none", list(it.islice("abc", 1, None)))
try:
    it.islice("abc", -1)
except ValueError as e:
    print("islice negative", type(e).__name__, e)
print("count", list(it.islice(it.count(10, 5), 4)))
print("count float", list(it.islice(it.count(0.5, 0.25), 3)))
print("count repr", repr(it.count(3, 2)))
print("cycle", list(it.islice(it.cycle("xyz"), 7)))
print("repeat", list(it.repeat("r", 3)), list(it.islice(it.repeat(0), 2)))

data = [("a", 1), ("a", 2), ("b", 3), ("a", 4), ("c", 5), ("c", 6)]
print("groupby", [(k, [v for _, v in g]) for k, g in it.groupby(data, key=operator.itemgetter(0))])
print("groupby sorted", [(k, len(list(g))) for k, g in it.groupby(sorted("mississippi"))])
print("groupby no key", [k for k, _ in it.groupby("aaabbbcca")])
groups = it.groupby("aabb")
first_key, first_group = next(groups)
next(groups)
print("groupby consumed", first_key, list(first_group))

print("product", list(it.product("ab", [1, 2])))
print("product repeat", ["".join(p) for p in it.product("01", repeat=3)])
print("product empty", list(it.product()), list(it.product("ab", [])))
print("permutations", list(it.permutations("abc")))
print("permutations r", ["".join(p) for p in it.permutations("abcd", 2)])
print("permutations r>n", list(it.permutations("ab", 3)))
print("combinations", list(it.combinations("abcd", 2)))
print("combinations 0", list(it.combinations("ab", 0)))
print("combinations_with_replacement", list(it.combinations_with_replacement("abc", 2)))
print("accumulate", list(it.accumulate([1, 2, 3, 4, 5])))
print("accumulate mul", list(it.accumulate([1, 2, 3, 4, 5], operator.mul)))
print("accumulate max", list(it.accumulate([3, 1, 4, 1, 5, 9, 2], max)))
print("accumulate initial", list(it.accumulate([1, 2], initial=100)))
print("accumulate empty", list(it.accumulate([])), list(it.accumulate([], initial=0)))
print("pairwise", list(it.pairwise("abcd")), list(it.pairwise("a")))
print("batched", list(it.batched("abcdefg", 3)))
print("batched exact", list(it.batched(range(6), 2)))
try:
    list(it.batched("abcde", 2, strict=True))
except ValueError as e:
    print("batched strict", type(e).__name__, e)
try:
    it.batched("ab", 0)
except ValueError as e:
    print("batched 0", type(e).__name__, e)
print("zip_longest", list(it.zip_longest("abc", [1])))
print("zip_longest fill", list(it.zip_longest("ab", "wxyz", fillvalue="-")))
print("starmap", list(it.starmap(pow, [(2, 3), (3, 2), (10, 0)])))
print("takewhile", list(it.takewhile(lambda x: x < 4, [1, 3, 5, 2, 1])))
print("dropwhile", list(it.dropwhile(lambda x: x < 4, [1, 3, 5, 2, 1])))
print("filterfalse", list(it.filterfalse(lambda x: x % 2, range(10))))
print("filterfalse None", list(it.filterfalse(None, [0, 1, "", "x", None])))
print("compress", list(it.compress("abcdef", [1, 0, 1, 0, 1, 1])))
t1, t2 = it.tee(iter([1, 2, 3]))
print("tee", list(t1), list(t2))
a, b, c = it.tee("xy", 3)
print("tee 3", list(a), list(b), list(c))

# laziness and exhaustion
gen = it.chain([1], [2])
print("exhaust", list(gen), list(gen))
calls = []


def noisy(x):
    calls.append(x)
    return x * x


lazy = map(noisy, it.count())
print("lazy islice", list(it.islice(lazy, 3)), calls)
print("next default", next(it.islice([], 1), "empty"))

# recipes
def flatten(list_of_lists):
    return list(it.chain.from_iterable(list_of_lists))


def powerset(iterable):
    s = list(iterable)
    return list(it.chain.from_iterable(it.combinations(s, r) for r in range(len(s) + 1)))


def sliding_window(iterable, n):
    iterator = iter(iterable)
    window = list(it.islice(iterator, n - 1))
    for x in iterator:
        window.append(x)
        yield tuple(window)
        del window[0]


print("flatten", flatten([[1, 2], [3], []]))
print("powerset", powerset("abc"))
print("sliding_window", list(sliding_window(range(6), 3)))
print("unique_justseen", [k for k, _ in it.groupby("AAAABBBCCDAABBB")])
print("dotproduct", sum(it.starmap(operator.mul, zip([1, 2, 3], [4, 5, 6]))))
print("roundrobin-ish", [x for x in it.chain.from_iterable(it.zip_longest("ABC", "D", "EF")) if x is not None])
