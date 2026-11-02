from rpds import HashTrieMap, HashTrieSet, List
m = HashTrieMap({"a": 1, "b": 2})
m2 = m.insert("c", 3)
print(sorted(m.items()), sorted(m2.items()), len(m), "c" in m2)
print(m2.remove("a") == HashTrieMap({"b": 2, "c": 3}))
s = HashTrieSet([1, 2, 3]).insert(4)
print(sorted(s), 4 in s)
l = List([1, 2, 3]).push_front(0)
print(list(l), l.first, list(l.rest))
try:
    m.remove("zz")
except KeyError as e:
    print("KeyError", e)
