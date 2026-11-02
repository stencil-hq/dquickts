# Operator precedence and associativity.

print("mul before add", 2 + 3 * 4, (2 + 3) * 4)
print("pow right assoc", 2 ** 3 ** 2, (2 ** 3) ** 2)
print("unary minus vs pow", -2 ** 2, (-2) ** 2, 2 ** -1, -2 ** -2)
print("pow vs unary on right", 2 ** -2 ** 2, 10 ** -1)
print("unary chain", - - 3, -+-3, ~~5, ~-3, -~3, not not 0)
print("sub left assoc", 10 - 4 - 3, 10 - (4 - 3))
print("div left assoc", 100 / 10 / 5, 100 / (10 / 5))
print("floordiv/mod left assoc", 100 // 7 % 3, 100 // (7 % 3), 7 % 4 * 3, 7 % (4 * 3))
print("mixed mul div", 2 * 6 / 4, 2 * (6 / 4), 7 // 2 * 2)
print("shift vs add", 1 << 2 + 1, (1 << 2) + 1, 64 >> 1 + 1, 1 + 2 << 3)
print("and vs or bitwise", 1 | 2 & 3, (1 | 2) & 3, 6 ^ 3 & 5, (6 ^ 3) & 5, 1 | 2 ^ 3)
print("bitwise vs compare", 1 & 3 == 1, (1 & 3) == 1, 4 | 1 > 3)
print("shift vs bitwise", 1 << 3 | 1, 1 << (3 | 1), 0xF0 >> 4 & 0x3)
print("not vs compare", not 1 == 2, (not 1) == 2, not 1 < 0)
print("not vs and/or", not 0 and 0, not (0 and 0), not 1 or 1)
print("and before or", 1 or 0 and 0, (1 or 0) and 0, 0 and 1 or 2)
print("compare vs arith", 1 + 1 == 2, 2 * 3 > 5, -1 < 0 == False)
print("in vs arith", 1 + 1 in [2], "a" + "b" in "abc")
print("is vs not", None is not None, not None is None)
print("ternary lowest", 1 if True else 2 + 10, (1 if False else 2) + 10)
print("ternary vs or", 0 or 1 if 0 else 2, (0 or 1) if 1 else 2)
print("lambda lowest", (lambda: 1 + 2)(), (lambda x: x if x else -1)(0))
print("unary vs mul", -3 * -3, -(3 * -3), -3 ** 2 * 2)
print("matmul-free mod pow", -7 % 3, -(7 % 3), 2 ** 3 % 5, 2 ** (3 % 5))
print("power of negative literal", (-8) ** (1 / 3) == (-8) ** (1 / 3), (-1) ** 0.5 == 1j)
print("subscript/call binds tightest", -[1, 2][0], -"abc".count("a"), 2 ** [3][0])
print("attribute on literal", (1).__add__(2), -1 .__abs__(), 2.5.__neg__())
print("await-free primary", [1, 2, 3][1:][0], {"a": [9]}["a"][-1])

# comparison chains with mixed operators
print("chain mixed", 1 < 2 == 2 > 1, 1 < 2 != 3 >= 3, 0 == 0 in [True], 1 in [1] is True)

# string ops precedence
print("string ops", "a" + "b" * 3, ("a" + "b") * 3, "%d" % 3 + "x", "%s" % (1 + 1))

# precedence of assignment expressions
y = None
if (x := 2 + 3) and (y := x * 2):
    print("walrus groups whole expr", x, y)

# precedence with unary not inside comparisons requires parentheses
for src in ["1 + not 2", "a == not b", "- not 1", "not - 1"]:
    try:
        compile(src, "<p>", "eval")
        print("compiles", repr(src))
    except SyntaxError as e:
        print("syntax", repr(src), e.msg)

# evaluation order: left operand before right
log = []
def v(tag, val):
    log.append(tag)
    return val

r = v("a", 2) ** v("b", 3) ** v("c", 1)
print("pow eval order", r, log)
log.clear()
r = v("a", 1) + v("b", 2) * v("c", 3)
print("arith eval order", r, log)
log.clear()
r = v("f", lambda *a: a)(v("x", 1), v("y", 2))
print("call eval order", r, log)
log.clear()
d = {v("k1", "a"): v("v1", 1), v("k2", "b"): v("v2", 2)}
print("dict eval order", log)
log.clear()
v("obj", [0, 0])[v("idx", 1)] = v("val", 9)
print("assignment eval order", log)

# int division and modulo sign rules
for a, b in [(7, 2), (-7, 2), (7, -2), (-7, -2)]:
    print("divmod", a, b, a // b, a % b, divmod(a, b), a / b)
for a, b in [(7.5, 2), (-7.5, 2)]:
    print("float divmod", a, b, a // b, a % b)
