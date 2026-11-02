"""L0 golden: realistic JSON workload: config merge, records, JSON lines, custom codecs."""
import dataclasses
import io
import json
from collections import Counter, defaultdict
from datetime import date
from decimal import Decimal

DEFAULTS = {"server": {"host": "localhost", "port": 8080, "tls": False}, "log": {"level": "info"}, "features": []}
USER = '{"server": {"port": 9090, "tls": true}, "features": ["beta"], "log": {"level": "debug", "file": null}}'


def deep_merge(base, override):
    """Return a new dict where *override* recursively wins over *base*."""
    merged = dict(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(merged.get(key), dict):
            merged[key] = deep_merge(merged[key], value)
        else:
            merged[key] = value
    return merged


config = deep_merge(DEFAULTS, json.loads(USER))
print("merged config")
print(json.dumps(config, indent=2, sort_keys=True))
print("defaults untouched", DEFAULTS["server"]["port"])


@dataclasses.dataclass
class Order:
    id: int
    customer: str
    items: list
    placed: date
    total: Decimal


def encode(obj):
    """JSON default hook for dataclasses, dates and decimals."""
    if dataclasses.is_dataclass(obj):
        return {"__type__": type(obj).__name__, **dataclasses.asdict(obj)}
    if isinstance(obj, date):
        return {"__date__": obj.isoformat()}
    if isinstance(obj, Decimal):
        return {"__decimal__": str(obj)}
    raise TypeError(f"Object of type {type(obj).__name__} is not JSON serializable")


def decode(d):
    """JSON object hook reversing :func:`encode`."""
    if "__date__" in d:
        return date.fromisoformat(d["__date__"])
    if "__decimal__" in d:
        return Decimal(d["__decimal__"])
    if d.get("__type__") == "Order":
        fields = {k: v for k, v in d.items() if k != "__type__"}
        return Order(**fields)
    return d


orders = [
    Order(1, "ann", [{"sku": "A1", "qty": 2}], date(2024, 1, 15), Decimal("19.98")),
    Order(2, "bob", [{"sku": "B7", "qty": 1}, {"sku": "A1", "qty": 1}], date(2024, 2, 1), Decimal("42.00")),
    Order(3, "ann", [], date(2024, 2, 3), Decimal("0.00")),
]
payload = json.dumps(orders, default=encode, separators=(",", ":"))
print("payload length", len(payload))
print("payload head", payload[:80])
restored = json.loads(payload, object_hook=decode)
print("restored equal", restored == orders)
print("restored types", type(restored[0]).__name__, type(restored[0].placed).__name__, type(restored[0].total).__name__)

# JSON lines
buf = io.StringIO()
for o in orders:
    buf.write(json.dumps({"id": o.id, "customer": o.customer, "n": len(o.items)}) + "\n")
buf.write("not json\n")
buf.write("\n")
buf.seek(0)
good, bad = [], 0
for lineno, line in enumerate(buf, 1):
    if not line.strip():
        continue
    try:
        good.append(json.loads(line))
    except json.JSONDecodeError as e:
        bad += 1
        print("jsonl error", lineno, e.msg, e.colno)
print("jsonl good", good, "bad", bad)

# aggregation
by_customer = defaultdict(Decimal)
skus = Counter()
for o in restored:
    by_customer[o.customer] += o.total
    for item in o.items:
        skus[item["sku"]] += item["qty"]
print("totals", json.dumps({k: str(v) for k, v in sorted(by_customer.items())}))
print("skus", json.dumps(dict(skus.most_common())))

# nested access with defaults and path queries
doc = json.loads('{"a": {"b": [{"c": 1}, {"c": 2, "d": {"e": "deep"}}]}}')


def get_path(obj, path, default=None):
    """Follow a dotted *path* (list indices as integers) through *obj*."""
    for part in path.split("."):
        try:
            obj = obj[int(part)] if isinstance(obj, list) else obj[part]
        except (KeyError, IndexError, ValueError, TypeError):
            return default
    return obj


for p in ["a.b.0.c", "a.b.1.d.e", "a.b.5.c", "a.x", "a.b.one", "a.b.1.c.z"]:
    print("path", p, repr(get_path(doc, p, "<missing>")))

print("pretty compact list", json.dumps({"rows": [[1, 2], [3, 4]]}, indent=None, separators=(", ", ": ")))
print("stable keys", json.dumps({"b": 1, "a": {"d": 2, "c": 3}}, sort_keys=True) == json.dumps({"a": {"c": 3, "d": 2}, "b": 1}, sort_keys=True))
