import html_to_markdown as h
html = "<h1>Title</h1><p>Some <b>bold</b> and <i>italic</i> text with <a href='http://x.org'>link</a>.</p><ul><li>one</li><li>two</li></ul><table><tr><th>a</th><th>b</th></tr><tr><td>1</td><td>2</td></tr></table><pre><code>x = 1</code></pre>"
r = h.convert(html)
print(r.content)
print(len(r.tables), r.warnings)
print(h.convert("<p>caf&eacute; &lt;tag&gt;</p>").content)
