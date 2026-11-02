"""L0 golden: difflib diffs, SequenceMatcher and get_close_matches."""
import difflib

a = ["one\n", "two\n", "three\n", "four\n", "five\n", "six\n"]
b = ["zero\n", "one\n", "three\n", "FOUR\n", "five\n", "six\n", "seven\n"]

print("unified:")
print("".join(difflib.unified_diff(a, b, fromfile="a.txt", tofile="b.txt")), end="")
print("unified n=0:")
print("".join(difflib.unified_diff(a, b, "old", "new", n=0)), end="")
print("unified lineterm:")
for line in difflib.unified_diff(["x", "y"], ["x", "z"], "l", "r", lineterm=""):
    print(" ", repr(line))
print("unified identical", list(difflib.unified_diff(a, a)))

print("context:")
print("".join(difflib.context_diff(a[:3], b[:3], "a", "b")), end="")

print("ndiff:")
print("".join(difflib.ndiff(["apple\n", "banana\n", "cherry\n"], ["apple\n", "bandana\n", "cherry pie\n"])), end="")
diff = list(difflib.ndiff(["abc\n"], ["abd\n"]))
print("ndiff list", diff)
print("restore 1", "".join(difflib.restore(diff, 1)), end="")
print("restore 2", "".join(difflib.restore(diff, 2)), end="")

sm = difflib.SequenceMatcher(None, "abcdefg", "abxdefgh")
print("ratio", round(sm.ratio(), 6))
print("quick_ratio", round(sm.quick_ratio(), 6), round(sm.real_quick_ratio(), 6))
print("opcodes", sm.get_opcodes())
print("matching blocks", sm.get_matching_blocks())
print("longest match", sm.find_longest_match(0, 7, 0, 8))
print("longest match defaults", sm.find_longest_match())
for tag, i1, i2, j1, j2 in sm.get_opcodes():
    print(f"  {tag:7} a[{i1}:{i2}]={sm.a[i1:i2]!r:8} b[{j1}:{j2}]={sm.b[j1:j2]!r}")
print("grouped", list(difflib.SequenceMatcher(None, "a" * 10 + "b", "a" * 10 + "c").get_grouped_opcodes(2)))

print("ratio identical", difflib.SequenceMatcher(None, "same", "same").ratio())
print("ratio disjoint", difflib.SequenceMatcher(None, "abc", "xyz").ratio())
print("ratio empty", difflib.SequenceMatcher(None, "", "").ratio())
print("ratio lists", round(difflib.SequenceMatcher(None, [1, 2, 3, 4], [1, 3, 4, 5]).ratio(), 6))
junk = difflib.SequenceMatcher(lambda c: c == " ", "a b c", "a  b c")
print("isjunk", round(junk.ratio(), 6), junk.get_opcodes())
sm2 = difflib.SequenceMatcher()
sm2.set_seqs("kitten", "sitting")
print("set_seqs", round(sm2.ratio(), 6), sm2.get_opcodes())
sm2.set_seq2("knitting")
print("set_seq2", round(sm2.ratio(), 6))

words = ["apple", "apply", "ape", "maple", "pineapple", "applet", "peach"]
print("close", difflib.get_close_matches("appel", words))
print("close n=1", difflib.get_close_matches("appel", words, n=1))
print("close cutoff", difflib.get_close_matches("appel", words, n=5, cutoff=0.4))
print("close none", difflib.get_close_matches("zzz", words))
print("close keywords", difflib.get_close_matches("wheel", ["while", "with", "where", "whale"]))
try:
    difflib.get_close_matches("x", words, n=0)
except ValueError as e:
    print("close n=0", type(e).__name__, e)
try:
    difflib.get_close_matches("x", words, cutoff=2)
except ValueError as e:
    print("close cutoff", type(e).__name__, e)

d = difflib.Differ()
print("Differ:")
print("".join(d.compare(["one\n", "two\n"], ["one\n", "tree\n", "emu\n"])), end="")
print("IS_CHARACTER_JUNK", difflib.IS_CHARACTER_JUNK(" "), difflib.IS_CHARACTER_JUNK("x"))
print("IS_LINE_JUNK", difflib.IS_LINE_JUNK("  #  \n"), difflib.IS_LINE_JUNK("code\n"))
print("diff_bytes:")
for line in difflib.diff_bytes(difflib.unified_diff, [b"a\n", b"b\n"], [b"a\n", b"c\n"], b"x", b"y"):
    print(" ", line)
