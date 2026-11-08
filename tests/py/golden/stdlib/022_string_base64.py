"""L0 golden: string constants/Template/Formatter, base64 and binascii."""
import base64
import binascii
import string

print("ascii_letters", string.ascii_letters)
print("lowercase/uppercase", string.ascii_lowercase, string.ascii_uppercase)
print("digits", string.digits, string.hexdigits, string.octdigits)
print("punctuation", string.punctuation)
print("whitespace", repr(string.whitespace))
print("printable len", len(string.printable), repr(string.printable[-6:]))
print("capwords", string.capwords("hello   big world"), string.capwords("a-b c-d", "-"))

t = string.Template("$who likes ${what}s, costs $$5")
print("substitute", t.substitute(who="tim", what="kung pao"))
print("substitute mapping", t.substitute({"who": "ann", "what": "taco"}))
print("substitute mapping+kw", t.substitute({"who": "x", "what": "y"}, who="kw"))
try:
    t.substitute(who="tim")
except KeyError as e:
    print("missing", type(e).__name__, e)
print("safe_substitute", t.safe_substitute(who="tim"))
print("safe bad placeholder", string.Template("$ok $ and ${").safe_substitute(ok=1))
try:
    string.Template("cost: $").substitute()
except ValueError as e:
    print("invalid", type(e).__name__, e)
print("identifiers", sorted(t.get_identifiers()), t.is_valid())
print("template attr", t.template)


class Percent(string.Template):
    delimiter = "%"


print("custom delimiter", Percent("%name is 100%% done").substitute(name="job"))

fmt = string.Formatter()
print("Formatter", fmt.format("{0}-{name}-{1:>4}", "a", "b", name="n"))
print("Formatter parse", list(fmt.parse("x{0!r:>5}y{name}z")))

# base64
raw = b"Many hands make light work."
enc = base64.b64encode(raw)
print("b64encode", enc)
print("b64decode", base64.b64decode(enc))
for n in range(5):
    print("padding", n, base64.b64encode(b"abcd"[:n]))
binary = bytes([0xFB, 0xFF, 0xBF, 0x00, 0x3E])
print("standard", base64.b64encode(binary))
print("urlsafe", base64.urlsafe_b64encode(binary))
print("urlsafe decode", base64.urlsafe_b64decode(base64.urlsafe_b64encode(binary)) == binary)
print("altchars", base64.b64encode(binary, altchars=b"._"))
print("decode str", base64.b64decode("aGk="))
print("decode nonalphabet skipped", base64.b64decode(b"aG k=\n"))
try:
    base64.b64decode(b"aG k=", validate=True)
except binascii.Error as e:
    print("validate", type(e).__name__, e)
try:
    base64.b64decode(b"abc")
except binascii.Error as e:
    print("bad padding", type(e).__name__, e)
print("b32", base64.b32encode(b"hello"), base64.b32decode(b"NBSWY3DP"))
print("b32 lower", base64.b32decode(b"nbswy3dp", casefold=True))
print("b32hex", base64.b32hexencode(b"hello"))
print("b16", base64.b16encode(b"\x01\xab\xff"), base64.b16decode(b"01ABFF"))
try:
    base64.b16decode(b"01abff")
except binascii.Error as e:
    print("b16 lowercase", type(e).__name__, e)
print("b16 casefold", base64.b16decode(b"01abff", casefold=True))
print("b85", base64.b85encode(b"hello"), base64.b85decode(base64.b85encode(b"hello")))
print("a85", base64.a85encode(b"hello"), base64.a85encode(b"\0\0\0\0"))
print("encodebytes", base64.encodebytes(b"x" * 60))
print("standard_b64", base64.standard_b64encode(b"\xff"), base64.standard_b64decode(b"/w=="))

# binascii
print("hexlify", binascii.hexlify(b"\x00\x7f\x80\xff"))
print("hexlify sep", binascii.hexlify(b"\x01\x02\x03", "-"), binascii.hexlify(b"\x01\x02\x03\x04", b":", 2))
print("unhexlify", binascii.unhexlify(b"deadBEEF"), binascii.a2b_hex("00ff"))
try:
    binascii.unhexlify(b"abc")
except binascii.Error as e:
    print("odd length", type(e).__name__, e)
try:
    binascii.unhexlify(b"zz")
except binascii.Error as e:
    print("non-hex", type(e).__name__, e)
print("crc32", binascii.crc32(b"hello world"), hex(binascii.crc32(b"")))
print("crc32 incremental", binascii.crc32(b" world", binascii.crc32(b"hello")))
print("crc_hqx", binascii.crc_hqx(b"hello", 0))
print("b2a_base64", binascii.b2a_base64(b"hi"), binascii.b2a_base64(b"hi", newline=False))
print("a2b_base64", binascii.a2b_base64(b"aGk=\n"))
print("bytes.hex", b"\xab\xcd".hex(), b"\xab\xcd\xef".hex(" ", -2), bytes.fromhex("ab cd"))
