"""L0 golden: complex number basics.

Covers literals, repr formatting (signed zeros, integral parts, inf/nan),
real/imag/conjugate, abs, arithmetic and mixed types, complex() parsing
from strings and numbers, unsupported operations and their errors.

Usage: python3.15 -I -X utf8 tests/py/golden/types/022_complex.py
"""


def show(label: str, thunk) -> None:
    """Print label and repr(thunk()), or the exception type and message."""
    try:
        print(label, repr(thunk()))
    except Exception as e:
        print(label, type(e).__name__, str(e))


show("1j", lambda: 1j)
show("1j * 1j", lambda: 1j * 1j)
show("(1+2j)", lambda: 1 + 2j)
show("(1-2j)", lambda: 1 - 2j)
show("-1j", lambda: -1j)
show("0j", lambda: 0j)
show("-0j", lambda: -0j)
show("-(0j)", lambda: -(0j))
show("complex(-0.0, -0.0)", lambda: complex(-0.0, -0.0))
show("complex(0.0, -0.0)", lambda: complex(0.0, -0.0))
show("(1.5+0.5j)", lambda: 1.5 + 0.5j)
show("1e16j", lambda: 1e16j)
show("(1e-5+1e22j)", lambda: 1e-5 + 1e22j)
show("str(2+3j)", lambda: str(2 + 3j))
show("complex(1, 2)", lambda: complex(1, 2))
show("complex(1)", lambda: complex(1))
show("complex()", lambda: complex())
show("complex(1.5)", lambda: complex(1.5))
show("complex(True, False)", lambda: complex(True, False))
show("complex(inf, nan)", lambda: complex(float("inf"), float("nan")))
show("complex(-inf, -inf)", lambda: complex(float("-inf"), float("-inf")))
show("abs(3+4j)", lambda: abs(3 + 4j))
show("abs(-5j)", lambda: abs(-5j))
show("abs(1+1j)", lambda: abs(1 + 1j))
show("(3+4j).real", lambda: (3 + 4j).real)
show("(3+4j).imag", lambda: (3 + 4j).imag)
show("type((3+4j).real)", lambda: type((3 + 4j).real).__name__)
show("(3+4j).conjugate()", lambda: (3 + 4j).conjugate())
show("(1+2j) + (3-4j)", lambda: (1 + 2j) + (3 - 4j))
show("(1+2j) - (1+2j)", lambda: (1 + 2j) - (1 + 2j))
show("(1+2j) * (3-4j)", lambda: (1 + 2j) * (3 - 4j))
show("(1+2j) / (3-4j)", lambda: (1 + 2j) / (3 - 4j))
show("(4+2j) / 2", lambda: (4 + 2j) / 2)
show("1 / 1j", lambda: 1 / 1j)
show("1j ** 2", lambda: 1j ** 2)
show("(1+1j) ** 2", lambda: (1 + 1j) ** 2)
show("(1+1j) ** 0", lambda: (1 + 1j) ** 0)
show("0j ** 0", lambda: 0j ** 0)
show("0j ** -1", lambda: 0j ** -1)
show("2 + 1j", lambda: 2 + 1j)
show("2.5 * 2j", lambda: 2.5 * 2j)
show("True + 1j", lambda: True + 1j)
show("-(1+2j)", lambda: -(1 + 2j))
show("+(1+2j)", lambda: +(1 + 2j))
show("(1+2j) == (1+2j)", lambda: (1 + 2j) == (1 + 2j))
show("1+0j == 1", lambda: 1 + 0j == 1)
show("1j == 1", lambda: 1j == 1)
show("1j < 2j", lambda: 1j < 2j)
show("1j > 0", lambda: 1j > 0)
show("1j // 1", lambda: 1j // 1)
show("1j % 1", lambda: 1j % 1)
show("divmod(1j, 1)", lambda: divmod(1j, 1))
show("int(1j)", lambda: int(1j))
show("float(1+0j)", lambda: float(1 + 0j))
show("round(1j)", lambda: round(1j))
show("pow(1j, 2, 3)", lambda: pow(1j, 2, 3))
show("bool(0j)", lambda: bool(0j))
show("bool(-0j)", lambda: bool(-0j))
show("bool(1e-300j)", lambda: bool(1e-300j))

for text in ("1+2j", "1-2j", " (1+2j) ", "2j", "-2j", "j", "-j", "1", "1.5e3+2.5e-3j", "inf+nanj",
             "-infj", "1_0+2_0j", "1e3j", "+1J", "(3)", "1+2j ", "1 + 2j", "1+2", "j1", "", "(1+2j",
             "1+2jj", "1++2j", "nanj", "0x1j"):
    show("complex(%r)" % text, lambda: complex(text))

show("complex('1', 2)", lambda: complex("1", 2))
show("complex(1, '2')", lambda: complex(1, "2"))
show("complex(None)", lambda: complex(None))
show("complex(1j, 1j)", lambda: complex(1j, 1j))
show("complex(real=1, imag=2)", lambda: complex(real=1, imag=2))
show("complex(10**400)", lambda: complex(10 ** 400))
show("complex.from_number(5)", lambda: complex.from_number(5))
show("complex.from_number('5')", lambda: complex.from_number("5"))
show("format(1+2j, '.2f')", lambda: format(1 + 2j, ".2f"))
show("format(1+2j, '')", lambda: format(1 + 2j, ""))
show("format(1+2j, '>12')", lambda: format(1 + 2j, ">12"))
show("format(1.5+2j, 'g')", lambda: format(1.5 + 2j, "g"))
show("format(1+2j, '=10')", lambda: format(1 + 2j, "=10"))
show("format(1+2j, '010')", lambda: format(1 + 2j, "010"))
show("hash(1+0j) == hash(1)", lambda: hash(1 + 0j) == hash(1))
show("{1+0j: 'a', 1: 'b'}", lambda: {1 + 0j: "a", 1: "b"})
show("type(1j).__name__", lambda: type(1j).__name__)
show("sum complex", lambda: sum([1j, 2, 3.5]))
