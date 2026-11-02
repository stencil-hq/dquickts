# String ops: split/join/replace/strip/find, f-strings, format specs,
# %-formatting, str methods, slicing.
import sys
import time

TEXT = ("The quick brown fox jumps over the lazy dog; "
        "Pack my box with five dozen liquor jugs. "
        "How vexingly quick daft zebras jump!  ")


def run(n):
    acc = 0
    out = []
    for i in range(n):
        line = TEXT + str(i)
        words = line.split()
        joined = "-".join(words)
        rep = joined.replace("quick", "slow").replace("-", " ")
        up = rep.upper()
        acc += len(up) + up.count("O") + rep.find("zebras")
        acc += len(line.strip().lower().title())
        a, sep, b = line.partition(";")
        acc += len(a) - len(b) + len(sep)
        s1 = f"{i:>8d}|{i * 0.37:10.3f}|{i:08x}|{i:,}|{words[i % 5]!r:>12}"
        s2 = "{0:<6}{1:^9}{2:+.2e}".format(i % 97, words[i % 3], i / 7.0)
        s3 = "%5d %-8s %.4f" % (i, words[(i + 1) % 4], i / 3.0)
        acc += len(s1) + len(s2) + len(s3)
        if line.startswith("The") and line.endswith(str(i)):
            acc += 1
        acc += sum(1 for c in words[i % 7] if c.isalpha())
        if i % 1000 == 0:
            out.append(s1 + s2 + s3)
    return f"{acc} {len(out)} {out[-1] if out else ''}"


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 30000
    run(n // 10)
    t0 = time.perf_counter()
    result = run(n)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {result}")


main()
