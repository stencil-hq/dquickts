"""L0 golden: typing constructs observable at runtime."""
import typing
from typing import (Any, Callable, ClassVar, Generic, Literal, NamedTuple, Optional, Protocol, TypedDict, TypeVar,
                    Union, cast, get_args, get_origin, get_type_hints, overload, runtime_checkable)


class Employee(NamedTuple):
    name: str
    dept: str = "eng"
    level: int = 1

    def badge(self):
        return f"{self.name}@{self.dept}"


e = Employee("ann")
print("NamedTuple", e, e.badge(), e._fields, e._field_defaults)
print("NamedTuple tuple", isinstance(e, tuple), e._replace(level=3), e._asdict())
print("NamedTuple annotations", get_type_hints(Employee))
try:
    Employee()
except TypeError as err:
    print("NamedTuple missing", type(err).__name__, err)

Pair = NamedTuple("Pair", [("left", int), ("right", int)])
print("functional NamedTuple", Pair(1, 2))


class Movie(TypedDict):
    title: str
    year: int


class MovieExtra(Movie, total=False):
    rating: float


m: Movie = {"title": "Blade Runner", "year": 1982}
print("TypedDict is dict", type(m).__name__, Movie(title="x", year=1))
print("required keys", sorted(Movie.__required_keys__), sorted(MovieExtra.__optional_keys__))
print("total", Movie.__total__, MovieExtra.__total__)
print("TypedDict hints", get_type_hints(MovieExtra))
try:
    isinstance(m, Movie)
except TypeError as err:
    print("TypedDict isinstance", type(err).__name__, err)


def greet(name: str, times: int = 1, *, loud: Optional[bool] = None) -> list[str]:
    return [name] * times


print("hints", get_type_hints(greet))
print("hints raw", greet.__annotations__ == get_type_hints(greet))


class Node:
    value: int
    next: "Optional[Node]"
    count: ClassVar[int] = 0


print("forward ref hints", get_type_hints(Node))
print("Optional repr", repr(Optional[int]), repr(Union[int, str]), repr(Union[int, None, str]))
print("Optional eq", Optional[int] == Union[int, None], Union[int, int] is int or Union[int, int] == int)
print("pipe union", repr(int | None), (int | None) == Optional[int])
print("get_origin", get_origin(list[int]), get_origin(Optional[int]) is Union, get_origin(int))
print("get_args", get_args(dict[str, list[int]]), get_args(Optional[int]), get_args(Callable[[int, str], bool]))
print("Literal", repr(Literal["a", 1]), get_args(Literal["a", 1]))
print("generic alias", repr(list[int]), repr(dict[str, int]), repr(typing.List[int]))
print("Callable", repr(Callable[..., int]), repr(Callable[[int], str]))
print("Any", repr(Any), isinstance(3, object))

T = TypeVar("T")
B = TypeVar("B", bound=int)


class Box(Generic[T]):
    def __init__(self, item: T):
        self.item = item

    def get(self) -> T:
        return self.item


print("TypeVar", repr(T), T.__name__, B.__bound__)
print("Generic", Box[int], Box(3).get(), Box.__parameters__)


class Stack[U]:
    def __init__(self):
        self.items: list[U] = []


print("PEP 695 class", Stack.__type_params__, Stack[int])
type Alias = list[int]
print("type alias", Alias, Alias.__value__, type(Alias).__name__)


@runtime_checkable
class Closable(Protocol):
    def close(self) -> None: ...


class File:
    def close(self):
        pass


print("Protocol", isinstance(File(), Closable), isinstance(3, Closable))
print("cast", cast(int, "not really"), cast("list[str]", 1))


@overload
def ident(x: int) -> int: ...
@overload
def ident(x: str) -> str: ...
def ident(x):
    return x


print("overload", ident(5), ident("s"))
print("TYPE_CHECKING", typing.TYPE_CHECKING)
print("assert_never exists", callable(typing.assert_never), typing.reveal_type.__name__)
try:
    typing.assert_never(1)
except AssertionError as err:
    print("assert_never", type(err).__name__, err)
print("Final/Annotated", get_args(typing.Annotated[int, "meta"]), repr(typing.Final[int]))
