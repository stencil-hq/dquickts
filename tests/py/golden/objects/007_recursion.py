# Recursion: classic recursive functions, mutual recursion, RecursionError on unbounded depth.


def fact(n):
    return 1 if n <= 1 else n * fact(n - 1)


print("fact 0", fact(0))
print("fact 10", fact(10))
print("fact 30", fact(30))


def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)


print("fib 0..15", [fib(i) for i in range(16)])


def is_even(n):
    return True if n == 0 else is_odd(n - 1)


def is_odd(n):
    return False if n == 0 else is_even(n - 1)


print("mutual", is_even(10), is_odd(7), is_even(99))


def hanoi(n, src, dst, via, moves):
    if n == 0:
        return moves
    hanoi(n - 1, src, via, dst, moves)
    moves.append(src + dst)
    hanoi(n - 1, via, dst, src, moves)
    return moves


print("hanoi 3", hanoi(3, "A", "C", "B", []))


def flatten(x):
    if isinstance(x, list):
        out = []
        for item in x:
            out.extend(flatten(item))
        return out
    return [x]


print("flatten", flatten([1, [2, [3, [4, []]], 5], [[6]]]))


def permutations(items):
    if len(items) <= 1:
        return [items]
    result = []
    for i in range(len(items)):
        for rest in permutations(items[:i] + items[i + 1 :]):
            result.append(items[i : i + 1] + rest)
    return result


print("perms", permutations("abc"))


def ackermann(m, n):
    if m == 0:
        return n + 1
    if n == 0:
        return ackermann(m - 1, 1)
    return ackermann(m - 1, ackermann(m, n - 1))


print("ackermann 2 3", ackermann(2, 3))


def depth_sum(n):
    return 0 if n == 0 else n + depth_sum(n - 1)


print("depth 500", depth_sum(500))


def forever(n):
    return forever(n + 1)


try:
    forever(0)
except RecursionError as e:
    print("unbounded", type(e).__name__)
print("RecursionError base", RecursionError.__mro__[1].__name__)


class Node:
    def __init__(self, value, children=()):
        self.value = value
        self.children = list(children)

    def total(self):
        return self.value + sum(c.total() for c in self.children)

    def depth(self):
        return 1 + max((c.depth() for c in self.children), default=0)


tree = Node(1, [Node(2, [Node(4)]), Node(3, [Node(5, [Node(6)])])])
print("tree total", tree.total())
print("tree depth", tree.depth())


class Loop:
    def __repr__(self):
        return repr(self)


try:
    repr(Loop())
except RecursionError as e:
    print("repr recursion", type(e).__name__)

lst = [1]
lst.append(lst)
print("self-referencing list", lst)
d = {"k": 1}
d["self"] = d
print("self-referencing dict", d)


def recurse_after_error(n):
    try:
        return forever(n)
    except RecursionError:
        return "recovered"


print("recovered", recurse_after_error(0))
print("still works", fact(5))
