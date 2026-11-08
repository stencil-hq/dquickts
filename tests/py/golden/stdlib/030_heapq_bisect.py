"""L0 golden: heapq priority queues and bisect on sorted sequences."""
import bisect
import heapq

h = []
for x in [5, 1, 8, 3, 9, 2, 7]:
    heapq.heappush(h, x)
print("heap invariant", all(h[i] <= h[c] for i in range(len(h)) for c in (2 * i + 1, 2 * i + 2) if c < len(h)))
print("heap[0]", h[0])
print("pop order", [heapq.heappop(h) for _ in range(len(h))])
try:
    heapq.heappop([])
except IndexError as e:
    print("pop empty", type(e).__name__, e)

data = [9, 4, 7, 1, 8, 2]
heapq.heapify(data)
print("heapify", data)
print("pushpop", heapq.heappushpop(data, 0), data[0])
print("replace", heapq.heapreplace(data, 10), sorted(data))
print("nlargest", heapq.nlargest(3, [5, 1, 9, 3, 7]), heapq.nlargest(0, [1]))
print("nsmallest", heapq.nsmallest(2, [5, 1, 9, 3, 7]), heapq.nsmallest(10, [3, 1]))
words = ["kiwi", "banana", "fig", "cherry", "apple"]
print("nlargest key", heapq.nlargest(2, words, key=len), heapq.nsmallest(2, words, key=len))
print("merge", list(heapq.merge([1, 4, 7], [2, 5, 8], [0, 9])))
print("merge key", list(heapq.merge(["a", "ccc"], ["bb", "dddd"], key=len)))
print("merge reverse", list(heapq.merge([7, 4, 1], [8, 5, 2], reverse=True)))

tasks = []
for prio, seq, name in [(2, 0, "write"), (1, 1, "plan"), (2, 2, "test"), (0, 3, "urgent"), (1, 4, "review")]:
    heapq.heappush(tasks, (prio, seq, name))
print("task order", [heapq.heappop(tasks)[2] for _ in range(5)])
mx = []
for x in [3, 1, 4, 1, 5]:
    heapq.heappush(mx, -x)
print("max-heap via neg", [-heapq.heappop(mx) for _ in range(5)])
mh = [3, 1, 4, 1, 5]
heapq.heapify_max(mh)
print("heapify_max", mh[0], [heapq.heappop_max(mh) for _ in range(5)])
try:
    heapq.heappush([1], "a")
except TypeError as e:
    print("unorderable", type(e).__name__, e)

# bisect
a = [1, 2, 4, 4, 4, 7, 9]
for x in [0, 4, 5, 9, 10]:
    print("bisect", x, bisect.bisect_left(a, x), bisect.bisect_right(a, x), bisect.bisect(a, x))
print("lo/hi", bisect.bisect_left(a, 4, 3), bisect.bisect_right(a, 4, 0, 3))
b = list(a)
bisect.insort(b, 5)
bisect.insort_left(b, 4)
bisect.insort_right(b, 0)
print("insort", b)


def grade(score, breakpoints=(60, 70, 80, 90), grades="FDCBA"):
    return grades[bisect.bisect(breakpoints, score)]


print("grades", [grade(s) for s in [33, 99, 77, 70, 89, 90, 100, 60]])
records = [("ann", 25), ("bob", 31), ("cy", 40), ("dee", 52)]
ages = [r[1] for r in records]
print("key bisect", bisect.bisect_left(records, 40, key=lambda r: r[1]), bisect.bisect_left(ages, 40))
sorted_records = list(records)
bisect.insort(sorted_records, ("eve", 33), key=lambda r: r[1])
print("insort key", sorted_records)


def index(seq, x):
    i = bisect.bisect_left(seq, x)
    if i != len(seq) and seq[i] == x:
        return i
    raise ValueError(f"{x} not found")


print("index", index(a, 7))
try:
    index(a, 5)
except ValueError as e:
    print("index missing", type(e).__name__, e)
try:
    bisect.bisect_left(a, 1, lo=-1)
except ValueError as e:
    print("negative lo", type(e).__name__, e)
print("strings", bisect.bisect(["apple", "cherry", "fig"], "banana"))
print("empty", bisect.bisect([], 3), bisect.bisect_left([], 3))
