"""numpy + matplotlib (Agg) demo, run by tests/py/mpl/run.sh under qpy and
CPython 3.15; everything printed must be identical between the two.

usage: demo.py OUTDIR

Writes OUTDIR/demo.{png,svg,pdf} and OUTDIR/layout.png and prints numeric
summaries of the data, the layout and the rendered output.
"""
import base64
import hashlib
import io
import os
import re
import sys
import zlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

out = sys.argv[1] if len(sys.argv) > 1 else "mpl-out"
os.makedirs(out, exist_ok=True)

# reproducible SVG ids
plt.rcParams["svg.hashsalt"] = "qpy-demo"


def r(a, nd=6):
    return (np.round(np.asarray(a, dtype=float), nd) + 0.0).tolist()   # no -0.0


def sha(data):
    return hashlib.sha256(data).hexdigest()[:16]


print("versions", matplotlib.__version__, np.__version__)
print("backend", matplotlib.get_backend())

rng = np.random.default_rng(0)

# --- data -----------------------------------------------------------------
x = np.linspace(0.0, 2.0 * np.pi, 200)
s, c = np.sin(x), np.cos(x)
print("trig", r([s.sum(), c.sum(), (s * s + c * c).mean(), np.abs(s).max()]))

pts = rng.normal(size=(300, 2))
radius = np.hypot(pts[:, 0], pts[:, 1])
print("scatter", r([pts.mean(), pts.std(), radius.min(), radius.max(), np.median(radius)]))

data = rng.normal(loc=1.0, scale=0.5, size=5000)
print("hist data", r([data.mean(), data.std(), np.percentile(data, 5), np.percentile(data, 95)]))

yy, xx = np.mgrid[-3:3:121j, -3:3:121j]
z = np.exp(-(xx ** 2 + 0.5 * yy ** 2)) + 0.6 * np.exp(-((xx - 1.5) ** 2 + (yy + 1.0) ** 2) * 2.0)
print("gauss", z.shape, r([z.min(), z.max(), z.mean(), z.sum()]))

# --- 2x2 figure -----------------------------------------------------------
fig, axs = plt.subplots(2, 2, figsize=(10, 8), dpi=100)

ax = axs[0, 0]
ax.plot(x, s, label=r"$\sin x$")
ax.plot(x, c, "--", label=r"$\cos x$")
ax.set_xlabel("x [rad]")
ax.set_title("sin / cos")
ax.legend(loc="upper right")
ax.grid(True)

ax = axs[0, 1]
sc = ax.scatter(pts[:, 0], pts[:, 1], c=radius, cmap="viridis", s=14, edgecolors="none")
fig.colorbar(sc, ax=ax, label="radius")
ax.set_aspect("equal")
ax.set_title("scatter")

ax = axs[1, 0]
counts, edges, patches = ax.hist(data, bins=40, color="tab:orange", edgecolor="k", linewidth=0.5)
ax.axvline(data.mean(), color="k", linestyle=":")
ax.set_title(r"histogram, $\mu \approx %.3f$" % data.mean())
print("hist", int(counts.sum()), int(counts.argmax()), int(counts.max()), r(edges[[0, 20, 40]]),
      len(patches))

ax = axs[1, 1]
im = ax.imshow(z, extent=(-3, 3, -3, 3), origin="lower", cmap="magma")
cs = ax.contour(xx, yy, z, levels=6, colors="w", linewidths=0.8)
labels = ax.clabel(cs, fmt="%.2f", fontsize=7)
fig.colorbar(im, ax=ax)
ax.set_title("imshow + contour")
print("contour levels", r(cs.levels))
print("contour vertices", [len(p.vertices) for p in cs.get_paths()])
print("clabels", len(labels), sorted(t.get_text() for t in labels))

fig.suptitle(r"qpy demo: $\alpha^2 + \sqrt{\beta}$, $\int_0^\infty e^{-x^2}\,dx = \frac{\sqrt{\pi}}{2}$")
fig.tight_layout()

print("size_inches", r(fig.get_size_inches()), "dpi", fig.dpi)
print("artists", len(fig.findobj()), "axes", len(fig.axes),
      "children", [len(a.get_children()) for a in fig.axes])
for a in fig.axes:
    print("axes", a.get_label() or "-", r(a.get_position().bounds, 4), r(a.get_xlim(), 4),
          r(a.get_ylim(), 4))

fig.canvas.draw()
renderer = fig.canvas.get_renderer()
title = fig._suptitle
print("suptitle extent", r(title.get_window_extent(renderer).bounds, 3))
print("tick labels", [t.get_text() for t in axs[0, 0].get_xticklabels()])

rgba = np.asarray(fig.canvas.buffer_rgba())
print("canvas", rgba.shape, rgba.dtype, r(rgba.reshape(-1, 4).mean(axis=0), 4),
      int((rgba[..., :3] < 128).any(axis=-1).sum()))

png = os.path.join(out, "demo.png")
svg = os.path.join(out, "demo.svg")
pdf = os.path.join(out, "demo.pdf")
fig.savefig(png)
fig.savefig(svg, metadata={"Date": None})
fig.savefig(pdf, metadata={"CreationDate": None, "ModDate": None})
# The embedded PNGs (SVG) and the Flate streams (PDF, PNG) depend on the
# zlib build, not on the interpreter: hash the decoded images and the
# decompressed streams, plus the rest of each file.
from PIL import Image

svg_bytes = open(svg, "rb").read()
svg_images = []


def svg_image(m):
    data = base64.b64decode(m.group(1))
    svg_images.append(np.asarray(Image.open(io.BytesIO(data))))
    return b"data:image/png;base64,IMAGE"


svg_text = re.sub(rb"data:image/png;base64,\s*([A-Za-z0-9+/=\s]+)", svg_image, svg_bytes)
svg_text = re.sub(rb'id="image[0-9a-f]+"', b'id="image"', svg_text)
print("svg", round(len(svg_bytes), -3), svg_bytes.count(b"<path"), svg_bytes.count(b"<use"),
      sha(svg_text), [(i.shape, sha(i.tobytes())) for i in svg_images])
pdf_bytes = open(pdf, "rb").read()
pdf_streams = re.findall(rb"stream\n(.*?)\nendstream", pdf_bytes, re.S)
pdf_flate = [zlib.decompress(m) for m in pdf_streams if m[:1] == b"x"]
print("pdf", pdf_bytes.count(b" obj\n"), pdf_bytes[:8], len(pdf_streams), len(pdf_flate),
      sha(b"".join(pdf_flate)))
print("canvas sha", sha(rgba.tobytes()))

# PNG round trip through matplotlib.image (Pillow)
img = plt.imread(png)
print("png", img.shape, img.dtype, r(img.mean(axis=(0, 1)), 4))
plt.close(fig)

# --- constrained layout (kiwisolver) --------------------------------------
fig2, axs2 = plt.subplots(2, 3, figsize=(7, 4), layout="constrained")
for i, a in enumerate(axs2.flat):
    a.plot(np.arange(10), np.arange(10) ** (i / 2.0))
    a.set_title("panel %d" % i)
    a.set_ylabel(r"$y^{%d/2}$" % i)
fig2.colorbar(plt.cm.ScalarMappable(cmap="coolwarm"), ax=axs2[:, -1])
fig2.canvas.draw()
for a in axs2.flat:
    print("constrained", r(a.get_position().bounds, 4))
fig2.savefig(os.path.join(out, "layout.png"))
plt.close(fig2)

print("done")
