import bcrypt
salt = b"$2b$04$" + b"abcdefghijklmnopqrstuu"
h = bcrypt.hashpw(b"correct horse", salt)
print(h)
print(bcrypt.checkpw(b"correct horse", h), bcrypt.checkpw(b"wrong", h))
print(bcrypt.kdf(b"password", b"salt", 32, 4).hex())
try:
    bcrypt.hashpw("str", salt)
except TypeError as e:
    print("TypeError", e)
