# Dict-heavy: word counting with dict.get, setdefault, items() iteration,
# dict comprehensions, membership tests, deletion.
import sys
import time

SYLLABLES = ["ka", "lo", "mi", "ne", "su", "ta", "ri", "po", "ze", "fu", "ya", "do"]


def make_words(n):
    seed = 12345
    words = []
    for _ in range(n):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        k = 1 + (seed >> 8) % 3
        w = ""
        s = seed
        for _ in range(k):
            w += SYLLABLES[s % 12]
            s //= 12
        words.append(w)
    return words


def run(words):
    counts = {}
    for w in words:
        counts[w] = counts.get(w, 0) + 1
    by_first = {}
    for w in words:
        by_first.setdefault(w[:2], []).append(len(w))
    total = 0
    for k, v in counts.items():
        total += len(k) * v
    first_sums = {k: sum(v) for k, v in by_first.items()}
    hits = 0
    for w in words:
        if w in counts and w[:4] in counts:
            hits += 1
    inverted = {}
    for k, v in counts.items():
        inverted.setdefault(v, []).append(k)
    for k in list(counts):
        if counts[k] == 1:
            del counts[k]
    top = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[:3]
    merged = {**first_sums, "extra": 1} | {"zz": 2}
    return f"{len(counts)} {total} {hits} {len(inverted)} {top} {len(merged)}"


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 200000
    words = make_words(n)
    run(words[: n // 10])
    t0 = time.perf_counter()
    for _ in range(3):
        result = run(words)
    t1 = time.perf_counter()
    print(f"time {t1 - t0:.6f}")
    print(f"check {result}")


main()
