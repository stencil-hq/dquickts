import jsonschema_rs
schema = {"type": "object", "properties": {"a": {"type": "integer", "minimum": 0}, "b": {"type": "string"}}, "required": ["a"]}
v = jsonschema_rs.validator_for(schema)
print(v.is_valid({"a": 1}), v.is_valid({"a": -1}), v.is_valid({"b": "x"}))
for err in sorted(v.iter_errors({"a": -1, "b": 3}), key=lambda e: str(e)):
    print(err.message, list(err.instance_path), list(err.schema_path))
try:
    jsonschema_rs.validate(schema, {"a": "no"})
except jsonschema_rs.ValidationError as e:
    print("ValidationError", e.message)
print(jsonschema_rs.is_valid({"type": "array", "items": {"type": "number"}}, [1, 2.5, 3]))
print(jsonschema_rs.Draft7Validator({"enum": [1, None, "x"]}).is_valid(None))
