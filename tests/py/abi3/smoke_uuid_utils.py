import uuid, uuid_utils
u = uuid_utils.UUID("12345678-1234-5678-1234-567812345678")
print(u, u.hex, u.version, u.int, u.bytes, u.urn, str(u.fields))
print(uuid_utils.uuid3(uuid_utils.NAMESPACE_DNS, "python.org"), uuid_utils.uuid5(uuid_utils.NAMESPACE_URL, "x"))
a = uuid_utils.uuid4(); print(a.version, len(str(a)), isinstance(a, uuid_utils.UUID))
b = uuid_utils.uuid7(); print(b.version, b.variant)
print(uuid_utils.UUID(int=5) < uuid_utils.UUID(int=6), hash(uuid_utils.UUID(int=5)) == hash(uuid_utils.UUID(int=5)))
print(uuid_utils.UUID(bytes=b"\x01"*16), repr(uuid_utils.UUID(int=255)))
import uuid_utils.compat as c
print(type(c.uuid4()) is uuid.UUID, c.uuid5(uuid.NAMESPACE_DNS, "a"))
try:
    uuid_utils.UUID("nope")
except ValueError as e:
    print("ValueError", e)
print(sorted([uuid_utils.UUID(int=3), uuid_utils.UUID(int=1)]))
