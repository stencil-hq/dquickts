import nh3
print(nh3.clean("<b>bold</b><script>alert(1)</script><a href='http://x' onclick='y'>l</a>"))
print(nh3.clean_text("<p>a & b</p>"))
print(nh3.is_html("<p>x</p>"), nh3.is_html("plain"))
print(nh3.clean("<p class='c'>x</p>", tags={"p"}, attributes={"p": {"class"}}))
