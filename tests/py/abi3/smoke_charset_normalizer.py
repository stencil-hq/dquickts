from charset_normalizer import from_bytes, detect, is_binary
import charset_normalizer.md as md, charset_normalizer.cd as cd
print(md.__file__.endswith('.so'), cd.__file__.endswith('.so'))
samples = [
    "Bonjour, ceci est un texte français avec des accents: é è à ç ù.".encode("cp1252"),
    "Привет, это русский текст для проверки кодировки.".encode("cp1251"),
    "Ünïcödé text in UTF-8 with emoji 🎉 and symbols ∑".encode("utf-8"),
    "UTF-16 encoded text sample".encode("utf-16"),
    "Hello plain ascii text".encode("ascii"),
    "Ελληνικά κείμενα για δοκιμή".encode("iso-8859-7"),
]
for b in samples:
    r = from_bytes(b).best()
    print(r.encoding if r else None, str(r)[:30] if r else None, detect(b)["encoding"])
print(is_binary(b"\x00\x01\x02\xff" * 50))
print(cd.encoding_languages("cp1251")[:3], md.mess_ratio("Hello world, normal text."))
