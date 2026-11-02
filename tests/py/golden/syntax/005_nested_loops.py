# Nested loops: matrices, early exit via flags/functions/exceptions, mixed for/while.

matrix = [[1, 2, 3], [4, 5, 6], [7, 8, 9]]
for row in matrix:
    line = ""
    for v in row:
        line += str(v) + " "
    print("row", line.strip())

# transpose with nested loops
t = []
for c in range(3):
    col = []
    for r in range(3):
        col.append(matrix[r][c])
    t.append(col)
print("transpose", t)

# multiplication table
for i in range(1, 4):
    print("table", i, [i * j for j in range(1, 4)])

# exit both loops with a flag
target = 5
found = None
for r, row in enumerate(matrix):
    for c, v in enumerate(row):
        if v == target:
            found = (r, c)
            break
    if found:
        break
print("flag exit", found)

# exit both loops with for/else/continue/break idiom
for r, row in enumerate(matrix):
    for c, v in enumerate(row):
        if v == 8:
            break
    else:
        continue
    break
print("else-continue idiom", r, c)

# exit via function return
def locate(m, x):
    for r, row in enumerate(m):
        for c, v in enumerate(row):
            if v == x:
                return r, c
    return None

print("locate 6", locate(matrix, 6))
print("locate 10", locate(matrix, 10))

# exit via exception
class Found(Exception):
    pass

try:
    for a in range(10):
        for b in range(10):
            if a * b == 12 and a < b:
                raise Found(a, b)
except Found as e:
    print("exception exit", e.args)

# triple nesting
count = 0
triples = []
for a in range(1, 15):
    for b in range(a, 15):
        for c in range(b, 15):
            count += 1
            if a * a + b * b == c * c:
                triples.append((a, b, c))
print("pythagorean", triples, "iterations", count)

# while inside for
out = []
for n in [5, 8, 1]:
    steps = 0
    while n > 1:
        n //= 2
        steps += 1
    out.append(steps)
print("while in for", out)

# for inside while
i = 0
acc = []
while i < 3:
    for ch in "ab":
        acc.append(ch + str(i))
    i += 1
print("for in while", acc)

# inner loop depends on outer variable
tri = []
for i in range(4):
    tri.append([j for j in range(i)])
print("triangle", tri)

# nested loops over strings
for a in "ab":
    for b in "xy":
        print("pair", a + b, end=" ")
print()

# continue in outer from inside inner via flag
res = []
for word in ["apple", "sky", "tree", "rhythm"]:
    has_vowel = False
    for ch in word:
        if ch in "aeiou":
            has_vowel = True
            break
    if not has_vowel:
        continue
    res.append(word)
print("words with vowels", res)

# flatten
nested = [[1, [2, 3]], [4], [], [[5]]]
flat = []
def walk(x):
    for item in x:
        if isinstance(item, list):
            walk(item)
        else:
            flat.append(item)

walk(nested)
print("flatten", flat)

# nested loop modifying a shared accumulator
total = 0
for i in range(5):
    for j in range(5):
        if (i + j) % 2:
            total += i * j
print("odd-sum products", total)
