"""L0 golden: ast.parse/dump/unparse/literal_eval and node visitors."""
import ast

print("dump expr", ast.dump(ast.parse("1 + 2 * x", mode="eval")))
print("dump stmt", ast.dump(ast.parse("x = [a, b]")))
print("dump attrs", ast.dump(ast.parse("y = f(1)"), include_attributes=True))
print("dump indent")
print(ast.dump(ast.parse("if a:\n    pass\nelse:\n    b()"), indent=2))
print("dump unannotated", ast.dump(ast.parse("del x").body[0], annotate_fields=False))
print("dump show_empty", ast.dump(ast.parse("def f(): pass").body[0], show_empty=True))
print("dump func", ast.dump(ast.parse("async def g(a, /, b=1, *c, d, **e) -> int: return a").body[0]))
print("dump fstring", ast.dump(ast.parse("f'{x!r:>{w}}'", mode="eval").body))
print("dump comprehension", ast.dump(ast.parse("[i for i in range(3) if i]", mode="eval").body))
print("dump lambda", ast.dump(ast.parse("lambda x, *, y=2: x", mode="eval").body))
print("dump match", ast.dump(ast.parse("match p:\n case [1, *rest] if rest: pass").body[0]))
print("dump class", ast.dump(ast.parse("@dec\nclass C(B, metaclass=M): x: int = 1").body[0]))

tree = ast.parse("def f(x):\n    return x * 2\n")
fn = tree.body[0]
print("node types", type(tree).__name__, type(fn).__name__, type(fn.body[0].value).__name__)
print("positions", fn.lineno, fn.col_offset, fn.end_lineno, fn.end_col_offset)
print("fields", ast.FunctionDef._fields)
print("get_docstring", ast.get_docstring(ast.parse('def g():\n    """Doc  string.\n\n    More."""')
                                          .body[0]))
print("source segment", ast.get_source_segment("a = 1 + foo(2)", ast.parse("a = 1 + foo(2)").body[0].value))

for src in ["x = (1 +\n 2)", "lambda: (yield)", "print ( 'hi' , end = '' )", "a if b else c", "x[1:2, ::3]",
            "def f(*, a): ...", "class A[T]: pass", "(a := 1)", "with (a as b, c): pass", "f(*[2], **{'k': 1})",
            "-(-1) ** 2", "not a or b and c", "x = 'it\\'s'", "type X = int", "try:\n pass\nexcept* E:\n pass"]:
    print("unparse", repr(src), "->", repr(ast.unparse(ast.parse(src))))

for src in ["42", "-3.5", "'s'", "b'by'", "[1, (2, 3), {'a': None}]", "{1, 2}", "True", "1+2j", "set()", "()",
            "{'k': [1, -2]}", "...", "0x1f", "1_000"]:
    print("literal_eval", src, "->", repr(ast.literal_eval(src)))
for bad in ["x", "1 + x", "f()", "[i for i in x]", "a.b", "{**d}", "2 ** 3"]:
    try:
        ast.literal_eval(bad)
    except ValueError as e:
        print("literal_eval bad", repr(bad), type(e).__name__, str(e).split(":")[0])
try:
    ast.literal_eval("1 +")
except SyntaxError as e:
    print("literal_eval syntax", type(e).__name__, e.msg)

for bad_src in ["def f(:", "x = = 1", "if x\n  pass", "return 5", "  x = 1", "f(**a, *b)"]:
    try:
        ast.parse(bad_src)
        print("parsed ok", repr(bad_src))
    except SyntaxError as e:
        print("parse error", repr(bad_src), type(e).__name__, e.msg, e.lineno, e.offset)


class Renamer(ast.NodeTransformer):
    def visit_Name(self, node):
        if node.id == "x":
            return ast.copy_location(ast.Name(id="renamed", ctx=node.ctx), node)
        return node


class Counter(ast.NodeVisitor):
    def __init__(self):
        self.names = []

    def visit_Name(self, node):
        self.names.append(node.id)
        self.generic_visit(node)


t = ast.parse("x = x + y * x")
counter = Counter()
counter.visit(t)
print("visitor names", counter.names)
t2 = ast.fix_missing_locations(Renamer().visit(t))
print("transformed", ast.unparse(t2))
print("walk", sorted({type(n).__name__ for n in ast.walk(t2)}))
print("iter_fields", [name for name, _ in ast.iter_fields(t2.body[0])])
print("iter_child_nodes", [type(n).__name__ for n in ast.iter_child_nodes(t2.body[0])])
code = compile(t2, "<ast>", "exec")
ns = {"renamed": 3, "y": 2}
exec(code, ns)
print("exec transformed", ns["renamed"])
built = ast.Expression(ast.BinOp(ast.Constant(6), ast.Mult(), ast.Constant(7)))
print("built", eval(compile(ast.fix_missing_locations(built), "<b>", "eval")), ast.unparse(built))
print("increment_lineno", ast.increment_lineno(ast.parse("a"), 5).body[0].lineno)
print("type_comments", ast.dump(ast.parse("x = 1  # type: int", type_comments=True).body[0]))
print("feature_version", ast.dump(ast.parse("a = 1", feature_version=(3, 8))))
print("compare", ast.compare(ast.parse("a+1"), ast.parse("a + 1")), ast.compare(ast.parse("a"), ast.parse("b")))
print("ast repr", repr(ast.parse("1", mode="eval")))
