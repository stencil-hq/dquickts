import css_inline
html = """<html><head><style>h1 { color: blue; } p.x { margin: 0 }</style></head>
<body><h1>Title</h1><p class="x">para</p></body></html>"""
print(css_inline.inline(html))
inl = css_inline.CSSInliner(keep_style_tags=True)
print(inl.inline(html))
print(css_inline.inline_fragment("<p>hi</p>", "p { color: red; }"))
print(css_inline.inline_many([html, "<p>x</p>"])[1])
try:
    css_inline.CSSInliner(base_url="not a url")
except ValueError as e:
    print("ValueError", type(e).__name__)
