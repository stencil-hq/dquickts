/*
 * dquickts Python: PEP 484/695 typing objects (object.h section (k)).
 *
 * Derived from CPython 3.15.2 Objects/typevarobject.c,
 * Objects/clinic/typevarobject.c.h, Modules/_typingmodule.c and the typing
 * intrinsics of Python/intrinsics.c (PSF-2.0).
 *
 * typing.TypeVar, ParamSpec, TypeVarTuple, ParamSpecArgs, ParamSpecKwargs,
 * TypeAliasType, Generic, NoDefaultType (+ the NoDefault singleton) and
 * _typing._ConstEvaluator are builtin types whose fixed fields live in
 * QPInstance slots of JS_CLASS_PY_OBJECT instances (QP_NULL = CPython NULL;
 * bool fields hold JS bools so they can be exposed as read-only members).
 * Everything CPython delegates to typing.py (_type_check, _typevar_subst,
 * _GenericAlias, Unpack, _generic_class_getitem, ...) is called through
 * `import typing` exactly where CPython does.
 */

/* Defined in genericaliasobject.c (same translation unit). */
static JSValue qp_union_from_tuple(JSContext *ctx, JSValueConst args);

/* ---- docstrings (CPython PyDoc_STRVAR texts) ---------------------------- */

static const char qp_nodefault_doc[] =
"NoDefaultType()\n"
"--\n\n"
"The type of the NoDefault singleton.";

static const char qp_constevaluator_doc[] =
"_ConstEvaluator()\n"
"--\n\n"
"Internal type for implementing evaluation functions.";

static const char qp_typevar_doc[] =
"Type variable.\n\
\n\
The preferred way to construct a type variable is via the dedicated\n\
syntax for generic functions, classes, and type aliases::\n\
\n\
    class Sequence[T]:  # T is a TypeVar\n\
        ...\n\
\n\
This syntax can also be used to create bound and constrained type\n\
variables::\n\
\n\
    # S is a TypeVar bound to str\n\
    class StrSequence[S: str]:\n\
        ...\n\
\n\
    # A is a TypeVar constrained to str or bytes\n\
    class StrOrBytesSequence[A: (str, bytes)]:\n\
        ...\n\
\n\
Type variables can also have defaults:\n\
\n\
    class IntDefault[T = int]:\n\
        ...\n\
\n\
However, if desired, reusable type variables can also be constructed\n\
manually, like so::\n\
\n\
   T = TypeVar('T')  # Can be anything\n\
   S = TypeVar('S', bound=str)  # Can be any subtype of str\n\
   A = TypeVar('A', str, bytes)  # Must be exactly str or bytes\n\
   D = TypeVar('D', default=int)  # Defaults to int\n\
\n\
Type variables exist primarily for the benefit of static type\n\
checkers.  They serve as the parameters for generic types as well\n\
as for generic function and type alias definitions.\n\
\n\
The variance of type variables is inferred by type checkers when they\n\
are created through the type parameter syntax and when\n\
``infer_variance=True`` is passed. Manually created type variables may\n\
be explicitly marked covariant or contravariant by passing\n\
``covariant=True`` or ``contravariant=True``. By default, manually\n\
created type variables are invariant. See PEP 484 and PEP 695 for more\n\
details.\n\
";

static const char qp_paramspecargs_doc[] =
"The args for a ParamSpec object.\n\
\n\
Given a ParamSpec object P, P.args is an instance of ParamSpecArgs.\n\
\n\
ParamSpecArgs objects have a reference back to their ParamSpec::\n\
\n\
    >>> P = ParamSpec(\"P\")\n\
    >>> P.args.__origin__ is P\n\
    True\n\
\n\
This type is meant for runtime introspection and has no special meaning\n\
to static type checkers.\n\
";

static const char qp_paramspeckwargs_doc[] =
"The kwargs for a ParamSpec object.\n\
\n\
Given a ParamSpec object P, P.kwargs is an instance of ParamSpecKwargs.\n\
\n\
ParamSpecKwargs objects have a reference back to their ParamSpec::\n\
\n\
    >>> P = ParamSpec(\"P\")\n\
    >>> P.kwargs.__origin__ is P\n\
    True\n\
\n\
This type is meant for runtime introspection and has no special meaning\n\
to static type checkers.\n\
";

static const char qp_paramspec_doc[] =
"Parameter specification variable.\n\
\n\
The preferred way to construct a parameter specification is via the\n\
dedicated syntax for generic functions, classes, and type aliases,\n\
where the use of '**' creates a parameter specification::\n\
\n\
    type IntFunc[**P] = Callable[P, int]\n\
\n\
The following syntax creates a parameter specification that defaults\n\
to a callable accepting two positional-only arguments of types int\n\
and str:\n\
\n\
    type IntFuncDefault[**P = [int, str]] = Callable[P, int]\n\
\n\
For compatibility with Python 3.11 and earlier, ParamSpec objects\n\
can also be created as follows::\n\
\n\
    P = ParamSpec('P')\n\
    DefaultP = ParamSpec('DefaultP', default=[int, str])\n\
\n\
Parameter specification variables exist primarily for the benefit of\n\
static type checkers.  They are used to forward the parameter types of\n\
one callable to another callable, a pattern commonly found in\n\
higher-order functions and decorators.  They are only valid when used\n\
in ``Concatenate``, or as the first argument to ``Callable``, or as\n\
parameters for user-defined Generics. See class Generic for more\n\
information on generic types.\n\
\n\
An example for annotating a decorator::\n\
\n\
    def add_logging[**P, T](f: Callable[P, T]) -> Callable[P, T]:\n\
        '''A type-safe decorator to add logging to a function.'''\n\
        def inner(*args: P.args, **kwargs: P.kwargs) -> T:\n\
            logging.info(f'{f.__name__} was called')\n\
            return f(*args, **kwargs)\n\
        return inner\n\
\n\
    @add_logging\n\
    def add_two(x: float, y: float) -> float:\n\
        '''Add two numbers together.'''\n\
        return x + y\n\
\n\
Parameter specification variables can be introspected. e.g.::\n\
\n\
    >>> P = ParamSpec(\"P\")\n\
    >>> P.__name__\n\
    'P'\n\
\n\
Note that only parameter specification variables defined in the global\n\
scope can be pickled.\n\
";

static const char qp_typevartuple_doc[] =
"Type variable tuple. A specialized form of type variable that enables\n\
variadic generics.\n\
\n\
The preferred way to construct a type variable tuple is via the\n\
dedicated syntax for generic functions, classes, and type aliases,\n\
where a single '*' indicates a type variable tuple::\n\
\n\
    def move_first_element_to_last[T, *Ts](tup: tuple[T, *Ts]) -> tuple[*Ts, T]:\n\
        return (*tup[1:], tup[0])\n\
\n\
Type variables tuples can have default values:\n\
\n\
    type AliasWithDefault[*Ts = (str, int)] = tuple[*Ts]\n\
\n\
For compatibility with Python 3.11 and earlier, TypeVarTuple objects\n\
can also be created as follows::\n\
\n\
    Ts = TypeVarTuple('Ts')  # Can be given any name\n\
    DefaultTs = TypeVarTuple('Ts', default=(str, int))\n\
\n\
Just as a TypeVar (type variable) is a placeholder for a single type,\n\
a TypeVarTuple is a placeholder for an *arbitrary* number of types. For\n\
example, if we define a generic class using a TypeVarTuple::\n\
\n\
    class C[*Ts]: ...\n\
\n\
Then we can parameterize that class with an arbitrary number of type\n\
arguments::\n\
\n\
    C[int]       # Fine\n\
    C[int, str]  # Also fine\n\
    C[()]        # Even this is fine\n\
\n\
For more details, see PEP 646.\n\
\n\
Note that only TypeVarTuples defined in the global scope can be\n\
pickled.\n\
";

static const char qp_typealias_doc[] =
"Type alias.\n\
\n\
Type aliases are created through the type statement::\n\
\n\
    type Alias = int\n\
\n\
In this example, Alias and int will be treated equivalently by static\n\
type checkers.\n\
\n\
At runtime, Alias is an instance of TypeAliasType. The __name__\n\
attribute holds the name of the type alias. The value of the type alias\n\
is stored in the __value__ attribute. It is evaluated lazily, so the\n\
value is computed only if the attribute is accessed. The __module__\n\
attribute holds the name of the module in which the type alias was\n\
defined; it can be assigned to.\n\
\n\
Type aliases can also be generic::\n\
\n\
    type ListOrSet[T] = list[T] | set[T]\n\
\n\
In this case, the type parameters of the alias are stored in the\n\
__type_params__ attribute.\n\
\n\
See PEP 695 for more information.\n\
";

static const char qp_generic_doc[] =
"Abstract base class for generic types.\n\
\n\
On Python 3.12 and newer, generic classes implicitly inherit from\n\
Generic when they declare a parameter list after the class's name::\n\
\n\
    class Mapping[KT, VT]:\n\
        def __getitem__(self, key: KT) -> VT:\n\
            ...\n\
        # Etc.\n\
\n\
On older versions of Python, however, generic classes have to\n\
explicitly inherit from Generic.\n\
\n\
After a class has been declared to be generic, it can then be used as\n\
follows::\n\
\n\
    def lookup_name[KT, VT](mapping: Mapping[KT, VT], key: KT, default: VT) -> VT:\n\
        try:\n\
            return mapping[key]\n\
        except KeyError:\n\
            return default\n\
";

static const char qp_generic_class_getitem_doc[] =
"Parameterizes a generic class.\n\
\n\
At least, parameterizing a generic class is the *main* thing this\n\
method does.  For example, for some generic class `Foo`, this is\n\
called when we do `Foo[int]` - there, with `cls=Foo` and\n\
`params=int`.\n\
\n\
However, note that this method is also called when defining generic\n\
classes in the first place with `class Foo[T]: ...`.\n\
";

/* ---- instance layouts (QPInstance slots) -------------------------------- */

enum { /* TypeVar */
    QP_TV_NAME,
    QP_TV_BOUND,
    QP_TV_EVALUATE_BOUND,
    QP_TV_CONSTRAINTS,
    QP_TV_EVALUATE_CONSTRAINTS,
    QP_TV_DEFAULT,
    QP_TV_EVALUATE_DEFAULT,
    QP_TV_COVARIANT,            /* JS bools: covariant, contravariant, infer_variance */
    QP_TV_CONTRAVARIANT,
    QP_TV_INFER_VARIANCE,
    QP_TV_NSLOTS
};

enum { /* ParamSpec and TypeVarTuple (same fields in CPython) */
    QP_PS_NAME,
    QP_PS_BOUND,
    QP_PS_DEFAULT,
    QP_PS_EVALUATE_DEFAULT,
    QP_PS_COVARIANT,
    QP_PS_CONTRAVARIANT,
    QP_PS_INFER_VARIANCE,
    QP_PS_NSLOTS
};

enum { /* TypeAliasType */
    QP_TA_NAME,
    QP_TA_QUALNAME,
    QP_TA_TYPE_PARAMS,          /* QP_NULL = no type parameters */
    QP_TA_COMPUTE_VALUE,
    QP_TA_VALUE,
    QP_TA_MODULE,
    QP_TA_NSLOTS
};

/* ParamSpecArgs / ParamSpecKwargs: slot 0 = __origin__.
   _typing._ConstEvaluator: slot 0 = the constant value. */

#define QP_ANNOTATE_FORMAT_STRING 4 /* annotationlib.Format.STRING */

/* magic of the lazy getters: value slot, evaluate_* slot, result when both
   are NULL */
enum { QP_LAZY_NONE, QP_LAZY_NODEFAULT, QP_LAZY_EMPTY_TUPLE };
#define QP_LAZY(vslot, eslot, kind) ((vslot) | ((eslot) << 8) | ((kind) << 16))
#define QP_EVAL(vslot, eslot) ((vslot) | ((eslot) << 8))

/* ---- helpers ------------------------------------------------------------ */

static BOOL qp_typing_is_none(JSValueConst v)
{
    return JS_IsUndefined(v) || JS_IsNull(v);
}

static BOOL qp_is_nodefault(JSContext *ctx, JSValueConst v)
{
    return !QP_IS_NULL(v) && qp_is(v, QP_STATE(ctx)->typing_nodefault);
}

static Py_ssize_t qp_typing_kwcount(JSValueConst kwnames)
{
    return JS_VALUE_GET_TAG(kwnames) == JS_TAG_OBJECT ? QP_TUPLE_SIZE(kwnames) : 0;
}

/* tp_name of a type as a str (heap types: __name__). */
static JSValue qp_typing_type_name(JSContext *ctx, QPType *t)
{
    if (t->tp_name)
        return qp_str_from_cstr(ctx, t->tp_name);
    return qp_str_from_atom(ctx, t->name);
}

/* _PyArg_BadArgument: "%s() %s must be %s, not %s". */
static JSValue qp_typing_bad_argument(JSContext *ctx, const char *fname,
                                      const char *displayname, const char *expected,
                                      JSValueConst arg)
{
    JSValue tn;

    if (qp_typing_is_none(arg))
        tn = qp_str_from_cstr(ctx, "None");
    else
        tn = qp_typing_type_name(ctx, qp_type_of(ctx, arg));
    if (JS_IsException(tn))
        return tn;
    qp_raise_type_error(ctx, "%.200s() %.200s must be %.50s, not %U",
                        fname, displayname, expected, tn);
    JS_FreeValue(ctx, tn);
    return JS_EXCEPTION;
}

/* 1 if s is a str (or subclass) equal to the ASCII literal lit, 0 if not,
   -1 on error (_PyUnicode_EqualToASCIIString after PyUnicode_Check). */
static int qp_typing_str_is(JSContext *ctx, JSValueConst s, const char *lit)
{
    JSValue v, l;
    int eq;

    if (!qp_str_check(ctx, s))
        return 0;
    v = qp_str_value(ctx, s);
    if (JS_IsException(v))
        return -1;
    l = qp_str_from_cstr(ctx, lit);
    if (JS_IsException(l)) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    eq = qp_str_eq(ctx, v, l);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, l);
    return eq;
}

/* Append the str r (consumed; str subclass instances are unboxed). */
static int qp_typing_buffer_put(JSContext *ctx, StringBuffer *b, JSValue r)
{
    if (!qp_str_check_exact(r)) {
        JSValue v = qp_str_value(ctx, r);
        JS_FreeValue(ctx, r);
        if (JS_IsException(v))
            return -1;
        r = v;
    }
    return string_buffer_concat_value_free(b, r);
}

/* CPython caller(): __module__ of the function running the innermost Python
   frame, None when there is none. */
static JSValue qp_typing_caller(JSContext *ctx)
{
    struct JSStackFrame *sf = qp_py_frame_at(ctx, 0);
    QPFuncData *fd;

    if (!sf)
        return JS_UNDEFINED;
    fd = qp_function_data(sf->cur_func);
    if (!fd || QP_IS_NULL(fd->module))
        return JS_UNDEFINED;
    return JS_DupValue(ctx, fd->module);
}

/* getattr(typing, name) after `import typing`. */
static JSValue qp_typing_get(JSContext *ctx, const char *name)
{
    JSValue typing, res;
    JSAtom atom;

    typing = qp_import_module(ctx, "typing");
    if (JS_IsException(typing))
        return typing;
    atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) {
        JS_FreeValue(ctx, typing);
        return JS_EXCEPTION;
    }
    res = qp_getattr(ctx, typing, atom);
    JS_FreeAtom(ctx, atom);
    JS_FreeValue(ctx, typing);
    return res;
}

/* call_typing_func_object: typing.<name>(*args). */
static JSValue qp_call_typing_func(JSContext *ctx, const char *name, JSValueConst *args,
                                   size_t nargs)
{
    JSValue func, res;

    func = qp_typing_get(ctx, name);
    if (JS_IsException(func))
        return func;
    res = qp_vectorcall(ctx, func, args, nargs, QP_NULL);
    JS_FreeValue(ctx, func);
    return res;
}

/* type_check(): typing._type_check(arg, msg); None -> NoneType without
   calling typing.py (bootstrapping). */
static JSValue qp_typing_type_check(JSContext *ctx, JSValueConst arg, const char *msg)
{
    JSValueConst args[2];
    JSValue m, res;

    if (qp_typing_is_none(arg))
        return JS_DupValue(ctx, QP_TYPE_VALUE(QP_TYPE(ctx, NoneType)));
    m = qp_str_from_cstr(ctx, msg);
    if (JS_IsException(m))
        return m;
    args[0] = arg;
    args[1] = m;
    res = qp_call_typing_func(ctx, "_type_check", args, 2);
    JS_FreeValue(ctx, m);
    return res;
}

/* nb_or of TypeVar and ParamSpec: typing.Union[self, other] built directly
   (make_union), which also accepts string forward references. */
static JSValue qp_typing_make_union(JSContext *ctx, JSValueConst self, JSValueConst other)
{
    JSValueConst items[2];
    JSValue args, u;

    items[0] = self;
    items[1] = other;
    args = qp_tuple_pack(ctx, 2, items);
    if (JS_IsException(args))
        return args;
    u = qp_union_from_tuple(ctx, args);
    JS_FreeValue(ctx, args);
    return u;
}

/* typing.Unpack[self] */
static JSValue qp_typing_unpack(JSContext *ctx, JSValueConst self)
{
    JSValue unpack, r;

    unpack = qp_typing_get(ctx, "Unpack");
    if (JS_IsException(unpack))
        return unpack;
    r = qp_getitem(ctx, unpack, self);
    JS_FreeValue(ctx, unpack);
    return r;
}

/* TypeVarTuple must be unpacked when passed to Generic: a new tuple with
   every TypeVarTuple replaced by Unpack[tvt] (params itself if none). */
static JSValue qp_unpack_typevartuples(JSContext *ctx, JSValueConst params)
{
    QPType *tvt = QP_TYPE(ctx, TypeVarTuple);
    Py_ssize_t i, j, n = QP_TUPLE_SIZE(params);
    JSValue *items, res;

    for (i = 0; i < n; i++) {
        if (qp_type_of(ctx, QP_TUPLE_ITEMS(params)[i]) == tvt)
            break;
    }
    if (i == n)
        return JS_DupValue(ctx, params);
    items = js_malloc(ctx, sizeof(JSValue) * n);
    if (!items)
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        JSValueConst param = QP_TUPLE_ITEMS(params)[i];
        if (qp_type_of(ctx, param) == tvt) {
            items[i] = qp_typing_unpack(ctx, param);
            if (JS_IsException(items[i])) {
                for (j = 0; j < i; j++)
                    JS_FreeValue(ctx, items[j]);
                js_free(ctx, items);
                return JS_EXCEPTION;
            }
        } else {
            items[i] = JS_DupValue(ctx, param);
        }
    }
    res = qp_tuple_new_steal(ctx, n, items);
    js_free(ctx, items);
    return res;
}

/* iter((Unpack[self],)): tp_iter of TypeVarTuple and TypeAliasType. */
static JSValue qp_typing_unpack_iter(JSContext *ctx, JSValueConst self)
{
    JSValue unpacked, tuple, res;

    unpacked = qp_typing_unpack(ctx, self);
    if (JS_IsException(unpacked))
        return unpacked;
    tuple = qp_tuple_new_steal(ctx, 1, &unpacked);
    if (JS_IsException(tuple))
        return tuple;
    res = qp_iter(ctx, tuple);
    JS_FreeValue(ctx, tuple);
    return res;
}

/* PySequence_Index(seq, obj) (_PySequence_IterSearch). */
static Py_ssize_t qp_typing_seq_index(JSContext *ctx, JSValueConst seq, JSValueConst obj)
{
    JSValue it, item;
    Py_ssize_t n = 0;
    int cmp;

    it = qp_iter(ctx, seq);
    if (JS_IsException(it)) {
        if (qp_exc_pending_matches(ctx, QP_EXC(ctx, TypeError))) {
            JSValue tn;
            JS_FreeValue(ctx, qp_exc_fetch(ctx));
            tn = qp_typing_type_name(ctx, qp_type_of(ctx, seq));
            if (!JS_IsException(tn)) {
                qp_raise_type_error(ctx, "argument of type '%U' is not iterable", tn);
                JS_FreeValue(ctx, tn);
            }
        }
        return -1;
    }
    for (;;) {
        item = qp_next(ctx, it);
        if (JS_IsException(item))
            goto fail;
        if (QP_IS_NULL(item))
            break;
        cmp = qp_richcompare_bool(ctx, item, obj, QP_CMP_EQ);
        JS_FreeValue(ctx, item);
        if (cmp < 0)
            goto fail;
        if (cmp > 0) {
            JS_FreeValue(ctx, it);
            return n;
        }
        n++;
    }
    JS_FreeValue(ctx, it);
    qp_raise_value_error(ctx, "sequence.index(x): x not in sequence");
    return -1;
 fail:
    JS_FreeValue(ctx, it);
    return -1;
}

/* _Py_typing_type_repr: repr of a type for typing reprs ("int",
   "mod.Cls", "...", "None", repr() of generic aliases and others). */
static int qp_typing_type_repr(JSContext *ctx, StringBuffer *b, JSValueConst p)
{
    JSValue qualname = QP_NULL, module = QP_NULL, tmp = QP_NULL, r = JS_EXCEPTION;
    int rc;

    if (qp_is(p, QP_STATE(ctx)->ellipsis))
        return string_buffer_puts8(b, "...");
    if (qp_is(p, QP_TYPE_VALUE(QP_TYPE(ctx, NoneType))))
        return string_buffer_puts8(b, "None");

    rc = qp_getattr_opt(ctx, p, QP_ATOM(ctx, __origin__), &tmp);
    JS_FreeValue(ctx, tmp);
    tmp = QP_NULL;
    if (rc > 0) {
        rc = qp_getattr_opt(ctx, p, QP_ATOM(ctx, __args__), &tmp);
        JS_FreeValue(ctx, tmp);
        if (rc > 0)
            goto use_repr;      /* it looks like a GenericAlias */
    }
    if (rc < 0)
        goto done;

    if (qp_getattr_opt(ctx, p, QP_ATOM(ctx, __qualname__), &qualname) < 0)
        goto done;
    if (QP_IS_NULL(qualname))
        goto use_repr;
    if (qp_getattr_opt(ctx, p, QP_ATOM(ctx, __module__), &module) < 0)
        goto done;
    if (QP_IS_NULL(module) || qp_typing_is_none(module))
        goto use_repr;

    /* looks like a class */
    rc = qp_typing_str_is(ctx, module, "builtins");
    if (rc < 0)
        goto done;
    if (rc)
        r = qp_str(ctx, qualname);     /* builtins don't need a module name */
    else
        r = qp_str_from_format(ctx, "%S.%S", module, qualname);
    goto done;

 use_repr:
    r = qp_repr(ctx, p);
 done:
    JS_FreeValue(ctx, qualname);
    JS_FreeValue(ctx, module);
    if (JS_IsException(r))
        return -1;
    return qp_typing_buffer_put(ctx, b, r);
}

/* ---- NoDefault ---------------------------------------------------------- */

static JSValue qp_nodefault_repr(JSContext *ctx, JSValueConst self)
{
    return qp_str_from_cstr(ctx, "typing.NoDefault");
}

static JSValue qp_nodefault_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    return qp_str_from_cstr(ctx, "NoDefault");
}

static JSValue qp_nodefault_new(JSContext *ctx, QPType *type, JSValueConst *args,
                                size_t nargsf, JSValueConst kwnames)
{
    if (qp_nargs(nargsf) || qp_typing_kwcount(kwnames))
        return qp_raise_type_error(ctx, "NoDefaultType takes no arguments");
    return JS_DupValue(ctx, QP_STATE(ctx)->typing_nodefault);
}

static const QPMethodDef qp_nodefault_methods[] = {
    { "__reduce__", qp_nodefault_reduce, QP_METH_NOARGS, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_nodefault_spec = {
    .name = "NoDefaultType",
    .doc = qp_nodefault_doc,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .slots = {
        .tp_repr = qp_nodefault_repr,
        .tp_new = qp_nodefault_new,
    },
    .methods = qp_nodefault_methods,
};

/* ---- _ConstEvaluator ---------------------------------------------------- */

static JSValue qp_constevaluator_alloc(JSContext *ctx, JSValueConst value)
{
    JSValue ce;

    ce = qp_object_new_instance(ctx, QP_TYPE(ctx, ConstEvaluator), JS_CLASS_PY_OBJECT);
    if (JS_IsException(ce))
        return ce;
    qp_instance_slots(ce)[0] = JS_DupValue(ctx, value);
    return ce;
}

static JSValue qp_constevaluator_repr(JSContext *ctx, JSValueConst self)
{
    return qp_str_from_format(ctx, "<constevaluator %R>", qp_instance_slots(self)[0]);
}

/* evaluator(format): the value, or its source-like repr for
   Format.STRING. */
static JSValue qp_constevaluator_call(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                      size_t nargsf, JSValueConst kwnames)
{
    Py_ssize_t i, n, nargs = qp_nargs(nargsf);
    JSValueConst value;
    StringBuffer b;
    int format;

    if (qp_arg_no_keywords(ctx, "constevaluator.__call__", kwnames) < 0)
        return JS_EXCEPTION;
    if (nargs != 1)
        return qp_raise_type_error(ctx, "constevaluator.__call__() takes exactly 1 argument (%zd given)",
                                   nargs);
    if (qp_arg_int(ctx, args[0], &format) < 0)
        return JS_EXCEPTION;
    value = qp_instance_slots(self)[0];
    if (format != QP_ANNOTATE_FORMAT_STRING)
        return JS_DupValue(ctx, value);

    string_buffer_init(ctx, &b, 5);
    if (qp_tuple_check(ctx, value)) {
        if (string_buffer_putc8(&b, '('))
            goto fail;
        n = QP_TUPLE_SIZE(value);
        for (i = 0; i < n; i++) {
            if (i > 0 && string_buffer_puts8(&b, ", "))
                goto fail;
            if (qp_typing_type_repr(ctx, &b, QP_TUPLE_ITEMS(value)[i]) < 0)
                goto fail;
        }
        if (string_buffer_putc8(&b, ')'))
            goto fail;
    } else {
        if (qp_typing_type_repr(ctx, &b, value) < 0)
            goto fail;
    }
    return string_buffer_end(&b);
 fail:
    string_buffer_free(&b);
    return JS_EXCEPTION;
}

static const QPTypeSpec qp_constevaluator_spec = {
    .name = "_typing._ConstEvaluator",
    .doc = qp_constevaluator_doc,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = 1,
    .slots = {
        .tp_repr = qp_constevaluator_repr,
        .tp_call = qp_constevaluator_call,
    },
};

/* ---- shared TypeVar / ParamSpec / TypeVarTuple / TypeAliasType parts ---- */

/* Lazily evaluated attribute (typevar_bound, typevar_default, ...): the
   cached value, else the evaluate_* function called once and cached, else
   None / NoDefault / () (magic = QP_LAZY(...)). */
static JSValue qp_typing_get_lazy(JSContext *ctx, JSValueConst self, int magic)
{
    int vslot = magic & 0xff, eslot = (magic >> 8) & 0xff, kind = magic >> 16;
    JSValue *s = qp_instance_slots(self);
    JSValue ev, v;

    if (!QP_IS_NULL(s[vslot]))
        return JS_DupValue(ctx, s[vslot]);
    if (QP_IS_NULL(s[eslot])) {
        switch (kind) {
        case QP_LAZY_NODEFAULT:
            return JS_DupValue(ctx, QP_STATE(ctx)->typing_nodefault);
        case QP_LAZY_EMPTY_TUPLE:
            return qp_tuple_new(ctx, 0);
        default:
            return JS_UNDEFINED;
        }
    }
    ev = JS_DupValue(ctx, s[eslot]);
    v = qp_call0(ctx, ev);
    JS_FreeValue(ctx, ev);
    if (JS_IsException(v))
        return v;
    s = qp_instance_slots(self);
    JS_FreeValue(ctx, s[vslot]);
    s[vslot] = JS_DupValue(ctx, v);
    return v;
}

/* evaluate_bound / evaluate_constraints / evaluate_default: the evaluate_*
   function, else a _ConstEvaluator of the value, else None
   (magic = QP_EVAL(...)). */
static JSValue qp_typing_get_evaluator(JSContext *ctx, JSValueConst self, int magic)
{
    int vslot = magic & 0xff, eslot = (magic >> 8) & 0xff;
    JSValue *s = qp_instance_slots(self);

    if (!QP_IS_NULL(s[eslot]))
        return JS_DupValue(ctx, s[eslot]);
    if (!QP_IS_NULL(s[vslot]))
        return qp_constevaluator_alloc(ctx, s[vslot]);
    return JS_UNDEFINED;
}

/* has_default() (magic = QP_EVAL(default slot, evaluate_default slot)). */
static JSValue qp_typing_has_default(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    int vslot = magic & 0xff, eslot = (magic >> 8) & 0xff;
    JSValue *s = qp_instance_slots(self);

    return qp_bool_new(!QP_IS_NULL(s[eslot]) ||
                       (!QP_IS_NULL(s[vslot]) && !qp_is_nodefault(ctx, s[vslot])));
}

/* __reduce__ of TypeVar, ParamSpec, TypeVarTuple, TypeAliasType: the name
   (slot 0 everywhere), pickled as a global. */
static JSValue qp_typing_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    return JS_DupValue(ctx, qp_instance_slots(self)[0]);
}

static const char *const qp_typing_kind_names[] = {
    "TypeVar", "ParamSpecArgs", "ParamSpecKwargs", "ParamSpec", "TypeVarTuple",
};

static JSValue qp_typing_mro_entries(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    return qp_raise_type_error(ctx, "Cannot subclass an instance of %s",
                               qp_typing_kind_names[magic]);
}

/* "~T", "+T", "-T" or "T" (infer_variance); cov_slot is followed by the
   contravariant and infer_variance slots. */
static JSValue qp_typing_variance_repr(JSContext *ctx, JSValueConst self, int name_slot,
                                       int cov_slot)
{
    JSValue *s = qp_instance_slots(self);
    JSValue name, r;
    int variance;

    if (JS_VALUE_GET_BOOL(s[cov_slot + 2]))
        return JS_DupValue(ctx, s[name_slot]);
    variance = JS_VALUE_GET_BOOL(s[cov_slot]) ? '+' :
        JS_VALUE_GET_BOOL(s[cov_slot + 1]) ? '-' : '~';
    name = qp_str_value(ctx, s[name_slot]);
    if (JS_IsException(name))
        return name;
    r = qp_str_from_format(ctx, "%c%U", variance, name);
    JS_FreeValue(ctx, name);
    return r;
}

/* Optional bool keyword (clinic `bool` converter = truth value). */
static int qp_typing_opt_bool(JSContext *ctx, JSValueConst v, int *out)
{
    int r;

    if (QP_IS_NULL(v))
        return 0;
    r = qp_bool(ctx, v);
    if (r < 0)
        return -1;
    *out = r;
    return 0;
}

static int qp_typing_check_variance(JSContext *ctx, int covariant, int contravariant,
                                    int infer_variance)
{
    if (covariant && contravariant) {
        qp_raise_value_error(ctx, "Bivariant types are not supported.");
        return -1;
    }
    if (infer_variance && (covariant || contravariant)) {
        qp_raise_value_error(ctx, "Variance cannot be specified with infer_variance.");
        return -1;
    }
    return 0;
}

/* ---- TypeVar ------------------------------------------------------------ */

/* typevar_alloc: arguments may be QP_NULL; module QP_NULL = no __module__. */
static JSValue qp_typevar_alloc(JSContext *ctx, JSValueConst name, JSValueConst bound,
                                JSValueConst evaluate_bound, JSValueConst constraints,
                                JSValueConst evaluate_constraints, JSValueConst default_value,
                                BOOL covariant, BOOL contravariant, BOOL infer_variance,
                                JSValueConst module)
{
    JSValue tv;
    JSValue *s;

    tv = qp_object_new_instance(ctx, QP_TYPE(ctx, TypeVar), JS_CLASS_PY_OBJECT);
    if (JS_IsException(tv))
        return tv;
    s = qp_instance_slots(tv);
    s[QP_TV_NAME] = JS_DupValue(ctx, name);
    s[QP_TV_BOUND] = JS_DupValue(ctx, bound);
    s[QP_TV_EVALUATE_BOUND] = JS_DupValue(ctx, evaluate_bound);
    s[QP_TV_CONSTRAINTS] = JS_DupValue(ctx, constraints);
    s[QP_TV_EVALUATE_CONSTRAINTS] = JS_DupValue(ctx, evaluate_constraints);
    s[QP_TV_DEFAULT] = JS_DupValue(ctx, default_value);
    s[QP_TV_EVALUATE_DEFAULT] = QP_NULL;
    s[QP_TV_COVARIANT] = JS_NewBool(ctx, covariant);
    s[QP_TV_CONTRAVARIANT] = JS_NewBool(ctx, contravariant);
    s[QP_TV_INFER_VARIANCE] = JS_NewBool(ctx, infer_variance);
    if (!QP_IS_NULL(module) && qp_setattr(ctx, tv, QP_ATOM(ctx, __module__), module) < 0) {
        JS_FreeValue(ctx, tv);
        return JS_EXCEPTION;
    }
    return tv;
}

static const char *const qp_typevar_kwlist[] = {
    "name", "bound", "default", "covariant", "contravariant", "infer_variance", NULL
};
static const QPArgSpec qp_typevar_argspec = { "typevar", qp_typevar_kwlist, 1, 1, 1 };

/* TypeVar(name, *constraints, bound=None, default=typing.NoDefault,
           covariant=False, contravariant=False, infer_variance=False) */
static JSValue qp_typevar_new(JSContext *ctx, QPType *type, JSValueConst *args,
                              size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);
    JSValueConst fa[6], name, bound = JS_UNDEFINED, default_value;
    JSValue constraints, checked_bound = QP_NULL, module, tv = JS_EXCEPTION;
    int covariant = 0, contravariant = 0, infer_variance = 0;
    Py_ssize_t n_constraints;

    if (qp_arg_parse(ctx, &qp_typevar_argspec, args, nargs, kwnames, fa) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, fa[0]))
        return qp_typing_bad_argument(ctx, "typevar", "argument 'name'", "str", fa[0]);
    name = fa[0];
    if (!QP_IS_NULL(fa[1]))
        bound = fa[1];
    default_value = QP_IS_NULL(fa[2]) ? (JSValueConst)QP_STATE(ctx)->typing_nodefault : fa[2];
    if (qp_typing_opt_bool(ctx, fa[3], &covariant) < 0 ||
        qp_typing_opt_bool(ctx, fa[4], &contravariant) < 0 ||
        qp_typing_opt_bool(ctx, fa[5], &infer_variance) < 0)
        return JS_EXCEPTION;
    if (nargs > 1)
        constraints = qp_tuple_pack(ctx, nargs - 1, args + 1);
    else
        constraints = qp_tuple_new(ctx, 0);
    if (JS_IsException(constraints))
        return constraints;

    if (qp_typing_check_variance(ctx, covariant, contravariant, infer_variance) < 0)
        goto done;
    if (!qp_typing_is_none(bound)) {
        checked_bound = qp_typing_type_check(ctx, bound, "Bound must be a type.");
        if (JS_IsException(checked_bound)) {
            checked_bound = QP_NULL;
            goto done;
        }
    }
    n_constraints = QP_TUPLE_SIZE(constraints);
    if (n_constraints == 1) {
        qp_raise_type_error(ctx, "A single constraint is not allowed");
        goto done;
    } else if (n_constraints > 1 && !QP_IS_NULL(checked_bound)) {
        qp_raise_type_error(ctx, "Constraints cannot be combined with bound=...");
        goto done;
    }
    module = qp_typing_caller(ctx);
    tv = qp_typevar_alloc(ctx, name, checked_bound, QP_NULL,
                          n_constraints ? (JSValueConst)constraints : QP_NULL, QP_NULL,
                          default_value, covariant, contravariant, infer_variance, module);
    JS_FreeValue(ctx, module);
 done:
    JS_FreeValue(ctx, checked_bound);
    JS_FreeValue(ctx, constraints);
    return tv;
}

static JSValue qp_typevar_repr(JSContext *ctx, JSValueConst self)
{
    return qp_typing_variance_repr(ctx, self, QP_TV_NAME, QP_TV_COVARIANT);
}

static JSValue qp_typevar_typing_subst(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                       int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst a[2];

    a[0] = self;
    a[1] = args[0];
    return qp_call_typing_func(ctx, "_typevar_subst", a, 2);
}

/* __typing_prepare_subst__(alias, args): fill in the default of a TypeVar
   missing from the substitution arguments. */
static JSValue qp_typevar_typing_prepare_subst(JSContext *ctx, JSValueConst self,
                                               JSValueConst *args, int nargs,
                                               JSValueConst kwnames, int magic)
{
    JSValueConst alias, targs;
    JSValue params, dflt, new_args, result;
    Py_ssize_t i, args_len;

    if (qp_arg_check_positional(ctx, "__typing_prepare_subst__", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    alias = args[0];
    targs = args[1];
    params = qp_getattr(ctx, alias, QP_ATOM(ctx, __parameters__));
    if (JS_IsException(params))
        return params;
    i = qp_typing_seq_index(ctx, params, self);
    JS_FreeValue(ctx, params);
    if (i < 0)
        return JS_EXCEPTION;
    args_len = qp_len(ctx, targs);
    if (args_len < 0)
        return JS_EXCEPTION;
    if (i < args_len) {
        /* we already have a value for our TypeVar */
        return JS_DupValue(ctx, targs);
    } else if (i == args_len) {
        /* if the TypeVar has a default, use it */
        dflt = qp_typing_get_lazy(ctx, self,
                                  QP_LAZY(QP_TV_DEFAULT, QP_TV_EVALUATE_DEFAULT, QP_LAZY_NODEFAULT));
        if (JS_IsException(dflt))
            return dflt;
        if (!qp_is_nodefault(ctx, dflt)) {
            new_args = qp_tuple_new_steal(ctx, 1, &dflt);
            if (JS_IsException(new_args))
                return new_args;
            result = qp_binop(ctx, targs, new_args, QP_NB_ADD);
            JS_FreeValue(ctx, new_args);
            return result;
        }
        JS_FreeValue(ctx, dflt);
    }
    return qp_raise_type_error(ctx, "Too few arguments for %S; actual %zd, expected at least %zd",
                               alias, args_len, i + 1);
}

static const QPMemberDef qp_typevar_members[] = {
    { "__name__", QP_TV_NAME, QP_MEMBER_READONLY | QP_MEMBER_NONE_IF_NULL, NULL },
    { "__covariant__", QP_TV_COVARIANT, QP_MEMBER_READONLY, NULL },
    { "__contravariant__", QP_TV_CONTRAVARIANT, QP_MEMBER_READONLY, NULL },
    { "__infer_variance__", QP_TV_INFER_VARIANCE, QP_MEMBER_READONLY, NULL },
    { NULL }
};

static const QPGetSetDef qp_typevar_getsets[] = {
    { "__bound__", qp_typing_get_lazy, NULL,
      QP_LAZY(QP_TV_BOUND, QP_TV_EVALUATE_BOUND, QP_LAZY_NONE), NULL },
    { "__constraints__", qp_typing_get_lazy, NULL,
      QP_LAZY(QP_TV_CONSTRAINTS, QP_TV_EVALUATE_CONSTRAINTS, QP_LAZY_EMPTY_TUPLE), NULL },
    { "__default__", qp_typing_get_lazy, NULL,
      QP_LAZY(QP_TV_DEFAULT, QP_TV_EVALUATE_DEFAULT, QP_LAZY_NODEFAULT), NULL },
    { "evaluate_bound", qp_typing_get_evaluator, NULL,
      QP_EVAL(QP_TV_BOUND, QP_TV_EVALUATE_BOUND), NULL },
    { "evaluate_constraints", qp_typing_get_evaluator, NULL,
      QP_EVAL(QP_TV_CONSTRAINTS, QP_TV_EVALUATE_CONSTRAINTS), NULL },
    { "evaluate_default", qp_typing_get_evaluator, NULL,
      QP_EVAL(QP_TV_DEFAULT, QP_TV_EVALUATE_DEFAULT), NULL },
    { NULL }
};

static const QPMethodDef qp_typevar_methods[] = {
    { "__typing_subst__", qp_typevar_typing_subst, QP_METH_O, 0,
      "__typing_subst__($self, arg, /)\n--\n\n" },
    { "__typing_prepare_subst__", qp_typevar_typing_prepare_subst, QP_METH_FASTCALL, 0,
      "__typing_prepare_subst__($self, alias, args, /)\n--\n\n" },
    { "__reduce__", qp_typing_reduce, QP_METH_NOARGS, 0, "__reduce__($self, /)\n--\n\n" },
    { "has_default", qp_typing_has_default, QP_METH_NOARGS,
      QP_EVAL(QP_TV_DEFAULT, QP_TV_EVALUATE_DEFAULT), "has_default($self, /)\n--\n\n" },
    { "__mro_entries__", qp_typing_mro_entries, QP_METH_O, 0, NULL },
    { NULL }
};

static const QPTypeSpec qp_typevar_spec = {
    .name = "typing.TypeVar",
    .doc = qp_typevar_doc,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_MANAGED_DICT | QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_TV_NSLOTS,
    .slots = {
        .tp_repr = qp_typevar_repr,
        .tp_new = qp_typevar_new,
        .nb = {
            .nb_or = qp_typing_make_union,
        },
    },
    .methods = qp_typevar_methods,
    .getsets = qp_typevar_getsets,
    .members = qp_typevar_members,
};

/* ---- ParamSpecArgs / ParamSpecKwargs ------------------------------------ */

static JSValue qp_paramspecattr_alloc(JSContext *ctx, QPType *tp, JSValueConst origin)
{
    JSValue psa;

    psa = qp_object_new_instance(ctx, tp, JS_CLASS_PY_OBJECT);
    if (JS_IsException(psa))
        return psa;
    qp_instance_slots(psa)[0] = JS_DupValue(ctx, origin);
    return psa;
}

static JSValue qp_paramspecattr_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b,
                                            int op)
{
    if (qp_type_of(ctx, a) != qp_type_of(ctx, b))
        return qp_not_implemented(ctx);
    if (op != QP_CMP_EQ && op != QP_CMP_NE)
        return qp_not_implemented(ctx);
    return qp_richcompare(ctx, qp_instance_slots(a)[0], qp_instance_slots(b)[0], op);
}

/* "P.args" for a ParamSpec origin, else "<repr(origin)>.args". */
static JSValue qp_paramspecattr_repr(JSContext *ctx, JSValueConst self, const char *suffix)
{
    JSValueConst origin = qp_instance_slots(self)[0];
    JSValue name, r;

    if (qp_type_of(ctx, origin) == QP_TYPE(ctx, ParamSpec)) {
        name = qp_str_value(ctx, qp_instance_slots(origin)[QP_PS_NAME]);
        if (JS_IsException(name))
            return name;
        r = qp_str_from_format(ctx, "%U.%s", name, suffix);
        JS_FreeValue(ctx, name);
        return r;
    }
    return qp_str_from_format(ctx, "%R.%s", origin, suffix);
}

static JSValue qp_paramspecargs_repr(JSContext *ctx, JSValueConst self)
{
    return qp_paramspecattr_repr(ctx, self, "args");
}

static JSValue qp_paramspeckwargs_repr(JSContext *ctx, JSValueConst self)
{
    return qp_paramspecattr_repr(ctx, self, "kwargs");
}

static const char *const qp_paramspecattr_kwlist[] = { "origin", NULL };
static const QPArgSpec qp_paramspecargs_argspec = { "paramspecargs", qp_paramspecattr_kwlist, 1, 1, 0 };
static const QPArgSpec qp_paramspeckwargs_argspec = { "paramspeckwargs", qp_paramspecattr_kwlist, 1, 1, 0 };

/* ParamSpecArgs(origin) */
static JSValue qp_paramspecargs_new(JSContext *ctx, QPType *type, JSValueConst *args,
                                    size_t nargsf, JSValueConst kwnames)
{
    JSValueConst fa[1];

    if (qp_arg_parse(ctx, &qp_paramspecargs_argspec, args, (int)qp_nargs(nargsf), kwnames, fa) < 0)
        return JS_EXCEPTION;
    return qp_paramspecattr_alloc(ctx, type, fa[0]);
}

/* ParamSpecKwargs(origin) */
static JSValue qp_paramspeckwargs_new(JSContext *ctx, QPType *type, JSValueConst *args,
                                      size_t nargsf, JSValueConst kwnames)
{
    JSValueConst fa[1];

    if (qp_arg_parse(ctx, &qp_paramspeckwargs_argspec, args, (int)qp_nargs(nargsf), kwnames, fa) < 0)
        return JS_EXCEPTION;
    return qp_paramspecattr_alloc(ctx, type, fa[0]);
}

static const QPMemberDef qp_paramspecattr_members[] = {
    { "__origin__", 0, QP_MEMBER_READONLY | QP_MEMBER_NONE_IF_NULL, NULL },
    { NULL }
};

static const QPMethodDef qp_paramspecargs_methods[] = {
    { "__mro_entries__", qp_typing_mro_entries, QP_METH_O, 1, NULL },
    { NULL }
};

static const QPMethodDef qp_paramspeckwargs_methods[] = {
    { "__mro_entries__", qp_typing_mro_entries, QP_METH_O, 2, NULL },
    { NULL }
};

static const QPTypeSpec qp_paramspecargs_spec = {
    .name = "typing.ParamSpecArgs",
    .doc = qp_paramspecargs_doc,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = 1,
    .slots = {
        .tp_repr = qp_paramspecargs_repr,
        .tp_richcompare = qp_paramspecattr_richcompare,
        .tp_new = qp_paramspecargs_new,
    },
    .methods = qp_paramspecargs_methods,
    .members = qp_paramspecattr_members,
};

static const QPTypeSpec qp_paramspeckwargs_spec = {
    .name = "typing.ParamSpecKwargs",
    .doc = qp_paramspeckwargs_doc,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = 1,
    .slots = {
        .tp_repr = qp_paramspeckwargs_repr,
        .tp_richcompare = qp_paramspecattr_richcompare,
        .tp_new = qp_paramspeckwargs_new,
    },
    .methods = qp_paramspeckwargs_methods,
    .members = qp_paramspecattr_members,
};

/* ---- ParamSpec and TypeVarTuple ----------------------------------------- */

/* paramspec_alloc / typevartuple_alloc (tp = ParamSpec or TypeVarTuple). */
static JSValue qp_paramspec_alloc(JSContext *ctx, QPType *tp, JSValueConst name,
                                  JSValueConst bound, JSValueConst default_value,
                                  BOOL covariant, BOOL contravariant, BOOL infer_variance,
                                  JSValueConst module)
{
    JSValue ps;
    JSValue *s;

    ps = qp_object_new_instance(ctx, tp, JS_CLASS_PY_OBJECT);
    if (JS_IsException(ps))
        return ps;
    s = qp_instance_slots(ps);
    s[QP_PS_NAME] = JS_DupValue(ctx, name);
    s[QP_PS_BOUND] = JS_DupValue(ctx, bound);
    s[QP_PS_DEFAULT] = JS_DupValue(ctx, default_value);
    s[QP_PS_EVALUATE_DEFAULT] = QP_NULL;
    s[QP_PS_COVARIANT] = JS_NewBool(ctx, covariant);
    s[QP_PS_CONTRAVARIANT] = JS_NewBool(ctx, contravariant);
    s[QP_PS_INFER_VARIANCE] = JS_NewBool(ctx, infer_variance);
    if (!QP_IS_NULL(module) && qp_setattr(ctx, ps, QP_ATOM(ctx, __module__), module) < 0) {
        JS_FreeValue(ctx, ps);
        return JS_EXCEPTION;
    }
    return ps;
}

/* paramspec_new_impl / typevartuple_impl after argument parsing. */
static JSValue qp_paramspec_new_impl(JSContext *ctx, QPType *tp, JSValueConst name,
                                     JSValueConst bound, JSValueConst default_value,
                                     int covariant, int contravariant, int infer_variance)
{
    JSValue module, ps;

    if (qp_typing_check_variance(ctx, covariant, contravariant, infer_variance) < 0)
        return JS_EXCEPTION;
    module = qp_typing_caller(ctx);
    ps = qp_paramspec_alloc(ctx, tp, name, bound, default_value, covariant, contravariant,
                            infer_variance, module);
    JS_FreeValue(ctx, module);
    return ps;
}

static const char *const qp_paramspec_kwlist[] = {
    "name", "bound", "default", "covariant", "contravariant", "infer_variance", NULL
};
static const QPArgSpec qp_paramspec_argspec = { "paramspec", qp_paramspec_kwlist, 1, 1, 0 };

/* ParamSpec(name, *, bound=None, default=typing.NoDefault,
             covariant=False, contravariant=False, infer_variance=False) */
static JSValue qp_paramspec_new(JSContext *ctx, QPType *type, JSValueConst *args,
                                size_t nargsf, JSValueConst kwnames)
{
    JSValueConst fa[6], bound, default_value;
    int covariant = 0, contravariant = 0, infer_variance = 0;

    if (qp_arg_parse(ctx, &qp_paramspec_argspec, args, (int)qp_nargs(nargsf), kwnames, fa) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, fa[0]))
        return qp_typing_bad_argument(ctx, "paramspec", "argument 'name'", "str", fa[0]);
    bound = QP_IS_NULL(fa[1]) ? JS_UNDEFINED : fa[1];
    default_value = QP_IS_NULL(fa[2]) ? (JSValueConst)QP_STATE(ctx)->typing_nodefault : fa[2];
    if (qp_typing_opt_bool(ctx, fa[3], &covariant) < 0 ||
        qp_typing_opt_bool(ctx, fa[4], &contravariant) < 0 ||
        qp_typing_opt_bool(ctx, fa[5], &infer_variance) < 0)
        return JS_EXCEPTION;
    return qp_paramspec_new_impl(ctx, QP_TYPE(ctx, ParamSpec), fa[0], bound, default_value,
                                 covariant, contravariant, infer_variance);
}

static const char *const qp_typevartuple_kwlist[] = {
    "name", "bound", "covariant", "contravariant", "infer_variance", "default", NULL
};
static const QPArgSpec qp_typevartuple_argspec = { "typevartuple", qp_typevartuple_kwlist, 1, 1, 0 };

/* TypeVarTuple(name, *, bound=None, covariant=False, contravariant=False,
                infer_variance=False, default=typing.NoDefault) */
static JSValue qp_typevartuple_new(JSContext *ctx, QPType *type, JSValueConst *args,
                                   size_t nargsf, JSValueConst kwnames)
{
    JSValueConst fa[6], bound, default_value;
    int covariant = 0, contravariant = 0, infer_variance = 0;

    if (qp_arg_parse(ctx, &qp_typevartuple_argspec, args, (int)qp_nargs(nargsf), kwnames, fa) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, fa[0]))
        return qp_typing_bad_argument(ctx, "typevartuple", "argument 'name'", "str", fa[0]);
    bound = QP_IS_NULL(fa[1]) ? JS_UNDEFINED : fa[1];
    if (qp_typing_opt_bool(ctx, fa[2], &covariant) < 0 ||
        qp_typing_opt_bool(ctx, fa[3], &contravariant) < 0 ||
        qp_typing_opt_bool(ctx, fa[4], &infer_variance) < 0)
        return JS_EXCEPTION;
    default_value = QP_IS_NULL(fa[5]) ? (JSValueConst)QP_STATE(ctx)->typing_nodefault : fa[5];
    return qp_paramspec_new_impl(ctx, QP_TYPE(ctx, TypeVarTuple), fa[0], bound, default_value,
                                 covariant, contravariant, infer_variance);
}

static JSValue qp_paramspec_repr(JSContext *ctx, JSValueConst self)
{
    return qp_typing_variance_repr(ctx, self, QP_PS_NAME, QP_PS_COVARIANT);
}

/* P.args (magic 0) / P.kwargs (magic 1) */
static JSValue qp_paramspec_get_attr(JSContext *ctx, JSValueConst self, int magic)
{
    return qp_paramspecattr_alloc(ctx, magic ? QP_TYPE(ctx, ParamSpecKwargs) : QP_TYPE(ctx, ParamSpecArgs),
                                  self);
}

static JSValue qp_paramspec_typing_subst(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                         int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst a[2];

    a[0] = self;
    a[1] = args[0];
    return qp_call_typing_func(ctx, "_paramspec_subst", a, 2);
}

/* __typing_prepare_subst__ of ParamSpec (magic 0) and TypeVarTuple
   (magic 1): typing._paramspec_prepare_subst /
   _typevartuple_prepare_subst(self, alias, args). */
static JSValue qp_paramspec_typing_prepare_subst(JSContext *ctx, JSValueConst self,
                                                 JSValueConst *args, int nargs,
                                                 JSValueConst kwnames, int magic)
{
    JSValueConst a[3];

    if (qp_arg_check_positional(ctx, "__typing_prepare_subst__", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    a[0] = self;
    a[1] = args[0];
    a[2] = args[1];
    return qp_call_typing_func(ctx, magic ? "_typevartuple_prepare_subst" : "_paramspec_prepare_subst",
                               a, 3);
}

static JSValue qp_typevartuple_typing_subst(JSContext *ctx, JSValueConst self,
                                            JSValueConst *args, int nargs,
                                            JSValueConst kwnames, int magic)
{
    return qp_raise_type_error(ctx, "Substitution of bare TypeVarTuple is not supported");
}

static const QPMemberDef qp_paramspec_members[] = {
    { "__name__", QP_PS_NAME, QP_MEMBER_READONLY | QP_MEMBER_NONE_IF_NULL, NULL },
    { "__bound__", QP_PS_BOUND, QP_MEMBER_READONLY | QP_MEMBER_NONE_IF_NULL, NULL },
    { "__covariant__", QP_PS_COVARIANT, QP_MEMBER_READONLY, NULL },
    { "__contravariant__", QP_PS_CONTRAVARIANT, QP_MEMBER_READONLY, NULL },
    { "__infer_variance__", QP_PS_INFER_VARIANCE, QP_MEMBER_READONLY, NULL },
    { NULL }
};

static const QPGetSetDef qp_paramspec_getsets[] = {
    { "args", qp_paramspec_get_attr, NULL, 0, "Represents positional arguments." },
    { "kwargs", qp_paramspec_get_attr, NULL, 1, "Represents keyword arguments." },
    { "__default__", qp_typing_get_lazy, NULL,
      QP_LAZY(QP_PS_DEFAULT, QP_PS_EVALUATE_DEFAULT, QP_LAZY_NODEFAULT),
      "The default value for this ParamSpec." },
    { "evaluate_default", qp_typing_get_evaluator, NULL,
      QP_EVAL(QP_PS_DEFAULT, QP_PS_EVALUATE_DEFAULT), NULL },
    { NULL }
};

static const QPMethodDef qp_paramspec_methods[] = {
    { "__typing_subst__", qp_paramspec_typing_subst, QP_METH_O, 0,
      "__typing_subst__($self, arg, /)\n--\n\n" },
    { "__typing_prepare_subst__", qp_paramspec_typing_prepare_subst, QP_METH_FASTCALL, 0,
      "__typing_prepare_subst__($self, alias, args, /)\n--\n\n" },
    { "has_default", qp_typing_has_default, QP_METH_NOARGS,
      QP_EVAL(QP_PS_DEFAULT, QP_PS_EVALUATE_DEFAULT), "has_default($self, /)\n--\n\n" },
    { "__reduce__", qp_typing_reduce, QP_METH_NOARGS, 0, "__reduce__($self, /)\n--\n\n" },
    { "__mro_entries__", qp_typing_mro_entries, QP_METH_O, 3, NULL },
    { NULL }
};

static const QPTypeSpec qp_paramspec_spec = {
    .name = "typing.ParamSpec",
    .doc = qp_paramspec_doc,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_MANAGED_DICT | QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_PS_NSLOTS,
    .slots = {
        .tp_repr = qp_paramspec_repr,
        .tp_new = qp_paramspec_new,
        /* Unions of ParamSpecs have no defined meaning, but they were
           allowed by the Python implementation, so they are allowed here
           too. */
        .nb = {
            .nb_or = qp_typing_make_union,
        },
    },
    .methods = qp_paramspec_methods,
    .getsets = qp_paramspec_getsets,
    .members = qp_paramspec_members,
};

static const QPGetSetDef qp_typevartuple_getsets[] = {
    { "__default__", qp_typing_get_lazy, NULL,
      QP_LAZY(QP_PS_DEFAULT, QP_PS_EVALUATE_DEFAULT, QP_LAZY_NODEFAULT),
      "The default value for this TypeVarTuple." },
    { "evaluate_default", qp_typing_get_evaluator, NULL,
      QP_EVAL(QP_PS_DEFAULT, QP_PS_EVALUATE_DEFAULT), NULL },
    { NULL }
};

static const QPMethodDef qp_typevartuple_methods[] = {
    { "__typing_subst__", qp_typevartuple_typing_subst, QP_METH_O, 0,
      "__typing_subst__($self, arg, /)\n--\n\n" },
    { "__typing_prepare_subst__", qp_paramspec_typing_prepare_subst, QP_METH_FASTCALL, 1,
      "__typing_prepare_subst__($self, alias, args, /)\n--\n\n" },
    { "__reduce__", qp_typing_reduce, QP_METH_NOARGS, 0, "__reduce__($self, /)\n--\n\n" },
    { "has_default", qp_typing_has_default, QP_METH_NOARGS,
      QP_EVAL(QP_PS_DEFAULT, QP_PS_EVALUATE_DEFAULT), "has_default($self, /)\n--\n\n" },
    { "__mro_entries__", qp_typing_mro_entries, QP_METH_O, 4, NULL },
    { NULL }
};

static const QPTypeSpec qp_typevartuple_spec = {
    .name = "typing.TypeVarTuple",
    .doc = qp_typevartuple_doc,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_MANAGED_DICT | QP_TPFLAGS_WEAKREF,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_PS_NSLOTS,
    .slots = {
        .tp_repr = qp_paramspec_repr,
        .tp_iter = qp_typing_unpack_iter,
        .tp_new = qp_typevartuple_new,
    },
    .methods = qp_typevartuple_methods,
    .getsets = qp_typevartuple_getsets,
    .members = qp_paramspec_members,
};

/* Default of a type parameter (get_type_param_default), new reference. */
static JSValue qp_get_type_param_default(JSContext *ctx, JSValueConst typeparam)
{
    QPType *t = qp_type_of(ctx, typeparam);

    if (t == QP_TYPE(ctx, TypeVar))
        return qp_typing_get_lazy(ctx, typeparam,
                                  QP_LAZY(QP_TV_DEFAULT, QP_TV_EVALUATE_DEFAULT, QP_LAZY_NODEFAULT));
    if (t == QP_TYPE(ctx, ParamSpec) || t == QP_TYPE(ctx, TypeVarTuple))
        return qp_typing_get_lazy(ctx, typeparam,
                                  QP_LAZY(QP_PS_DEFAULT, QP_PS_EVALUATE_DEFAULT, QP_LAZY_NODEFAULT));
    return qp_raise_type_error(ctx, "Expected a type param, got %R", typeparam);
}

/* _Py_set_typeparam_default (INTRINSIC_SET_TYPEPARAM_DEFAULT). */
static JSValue qp_set_typeparam_default(JSContext *ctx, JSValueConst typeparam,
                                        JSValueConst evaluate_default)
{
    QPType *t = qp_type_of(ctx, typeparam);
    JSValue *s;
    int slot;

    if (t == QP_TYPE(ctx, TypeVar))
        slot = QP_TV_EVALUATE_DEFAULT;
    else if (t == QP_TYPE(ctx, ParamSpec) || t == QP_TYPE(ctx, TypeVarTuple))
        slot = QP_PS_EVALUATE_DEFAULT;
    else
        return qp_raise_type_error(ctx, "Expected a type param, got %R", typeparam);
    s = qp_instance_slots(typeparam);
    JS_FreeValue(ctx, s[slot]);
    s[slot] = JS_DupValue(ctx, evaluate_default);
    return JS_DupValue(ctx, typeparam);
}

/* ---- TypeAliasType ------------------------------------------------------ */

static JSValue qp_typealias_alloc(JSContext *ctx, JSValueConst name, JSValueConst qualname,
                                  JSValueConst type_params, JSValueConst compute_value,
                                  JSValueConst value, JSValueConst module)
{
    JSValue ta;
    JSValue *s;

    ta = qp_object_new_instance(ctx, QP_TYPE(ctx, TypeAliasType), JS_CLASS_PY_OBJECT);
    if (JS_IsException(ta))
        return ta;
    s = qp_instance_slots(ta);
    s[QP_TA_NAME] = JS_DupValue(ctx, name);
    s[QP_TA_QUALNAME] = JS_DupValue(ctx, qualname);
    s[QP_TA_TYPE_PARAMS] = JS_DupValue(ctx, type_params);
    s[QP_TA_COMPUTE_VALUE] = JS_DupValue(ctx, compute_value);
    s[QP_TA_VALUE] = JS_DupValue(ctx, value);
    s[QP_TA_MODULE] = JS_DupValue(ctx, module);
    return ta;
}

/* __value__: computed once by calling the lazy evaluation function. */
static JSValue qp_typealias_value(JSContext *ctx, JSValueConst self, int magic)
{
    JSValue *s = qp_instance_slots(self);
    JSValue fn, r;

    if (!QP_IS_NULL(s[QP_TA_VALUE]))
        return JS_DupValue(ctx, s[QP_TA_VALUE]);
    fn = JS_DupValue(ctx, s[QP_TA_COMPUTE_VALUE]);
    r = qp_call0(ctx, fn);
    JS_FreeValue(ctx, fn);
    if (JS_IsException(r))
        return r;
    s = qp_instance_slots(self);
    JS_FreeValue(ctx, s[QP_TA_VALUE]);
    s[QP_TA_VALUE] = JS_DupValue(ctx, r);
    return r;
}

static JSValue qp_typealias_evaluate_value(JSContext *ctx, JSValueConst self, int magic)
{
    JSValue *s = qp_instance_slots(self);

    if (!QP_IS_NULL(s[QP_TA_COMPUTE_VALUE]))
        return JS_DupValue(ctx, s[QP_TA_COMPUTE_VALUE]);
    return qp_constevaluator_alloc(ctx, s[QP_TA_VALUE]);
}

static JSValue qp_typealias_parameters(JSContext *ctx, JSValueConst self, int magic)
{
    JSValueConst tp = qp_instance_slots(self)[QP_TA_TYPE_PARAMS];

    if (QP_IS_NULL(tp))
        return qp_tuple_new(ctx, 0);
    return qp_unpack_typevartuples(ctx, tp);
}

static JSValue qp_typealias_type_params(JSContext *ctx, JSValueConst self, int magic)
{
    JSValueConst tp = qp_instance_slots(self)[QP_TA_TYPE_PARAMS];

    if (QP_IS_NULL(tp))
        return qp_tuple_new(ctx, 0);
    return JS_DupValue(ctx, tp);
}

/* __module__: the assigned value, else the module of the evaluation
   function, else None. */
static JSValue qp_typealias_module(JSContext *ctx, JSValueConst self, int magic)
{
    JSValue *s = qp_instance_slots(self);
    QPFuncData *fd;

    if (!QP_IS_NULL(s[QP_TA_MODULE]))
        return JS_DupValue(ctx, s[QP_TA_MODULE]);
    if (!QP_IS_NULL(s[QP_TA_COMPUTE_VALUE])) {
        /* may be unset, e.g. for functions defined in an exec()'ed block */
        fd = qp_function_data(s[QP_TA_COMPUTE_VALUE]);
        if (fd && !QP_IS_NULL(fd->module))
            return JS_DupValue(ctx, fd->module);
    }
    return JS_UNDEFINED;
}

static int qp_typealias_set_module(JSContext *ctx, JSValueConst self, JSValueConst value,
                                   int magic)
{
    JSValue *s = qp_instance_slots(self);
    JSValue old = s[QP_TA_MODULE];

    s[QP_TA_MODULE] = JS_DupValue(ctx, value);
    JS_FreeValue(ctx, old);
    return 0;
}

static JSValue qp_typealias_repr(JSContext *ctx, JSValueConst self)
{
    return JS_DupValue(ctx, qp_instance_slots(self)[QP_TA_QUALNAME]);
}

/* X[args] -> types.GenericAlias(X, args) for generic aliases. */
static JSValue qp_typealias_subscript(JSContext *ctx, JSValueConst self, JSValueConst args)
{
    if (QP_IS_NULL(qp_instance_slots(self)[QP_TA_TYPE_PARAMS]))
        return qp_raise_type_error(ctx, "Only generic type aliases are subscriptable");
    return qp_generic_alias_new(ctx, self, args);
}

/* typealias_check_type_params: *pchecked = type_params (borrowed) or
   QP_NULL for none/empty; TypeError when a parameter without default
   follows one with a default.  0 or -1. */
static int qp_typealias_check_type_params(JSContext *ctx, JSValueConst type_params,
                                          JSValueConst *pchecked)
{
    Py_ssize_t i, n;
    JSValue dflt;
    BOOL default_seen = FALSE;

    *pchecked = QP_NULL;
    if (QP_IS_NULL(type_params))
        return 0;
    n = QP_TUPLE_SIZE(type_params);
    if (n == 0)     /* 0-length tuples are the same as NULL */
        return 0;
    for (i = 0; i < n; i++) {
        JSValueConst type_param = QP_TUPLE_ITEMS(type_params)[i];
        dflt = qp_get_type_param_default(ctx, type_param);
        if (JS_IsException(dflt))
            return -1;
        if (qp_is_nodefault(ctx, dflt)) {
            JS_FreeValue(ctx, dflt);
            if (default_seen) {
                qp_raise_type_error(ctx, "non-default type parameter '%R' "
                                    "follows default type parameter", type_param);
                return -1;
            }
        } else {
            default_seen = TRUE;
            JS_FreeValue(ctx, dflt);
        }
    }
    *pchecked = type_params;
    return 0;
}

static const char *const qp_typealias_kwlist[] = {
    "name", "value", "type_params", "qualname", NULL
};
static const QPArgSpec qp_typealias_argspec = { "typealias", qp_typealias_kwlist, 2, 2, 0 };

/* TypeAliasType(name, value, *, type_params=(), qualname=None) */
static JSValue qp_typealias_new(JSContext *ctx, QPType *type, JSValueConst *args,
                                size_t nargsf, JSValueConst kwnames)
{
    JSValueConst fa[4], name, value, type_params, qualname, checked_params;
    JSValue module, ta;

    if (qp_arg_parse(ctx, &qp_typealias_argspec, args, (int)qp_nargs(nargsf), kwnames, fa) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, fa[0]))
        return qp_typing_bad_argument(ctx, "typealias", "argument 'name'", "str", fa[0]);
    name = fa[0];
    value = fa[1];
    type_params = fa[2];
    qualname = fa[3];

    if (!QP_IS_NULL(type_params) && !qp_tuple_check(ctx, type_params))
        return qp_raise_type_error(ctx, "type_params must be a tuple");
    if (qp_typealias_check_type_params(ctx, type_params, &checked_params) < 0)
        return JS_EXCEPTION;
    if (QP_IS_NULL(qualname) || qp_typing_is_none(qualname)) {
        /* if qualname was not set directly, use name instead */
        qualname = name;
    } else if (!qp_str_check(ctx, qualname)) {
        return qp_raise_type_error(ctx, "qualname must be a string");
    }
    module = qp_typing_caller(ctx);
    ta = qp_typealias_alloc(ctx, name, qualname, checked_params, QP_NULL, value, module);
    JS_FreeValue(ctx, module);
    return ta;
}

/* _Py_make_typealias (INTRINSIC_TYPEALIAS): args = (name, type_params or
   None, compute_value function); __qualname__ is the co_qualname of the
   evaluation function. */
static JSValue qp_make_typealias(JSContext *ctx, JSValueConst args)
{
    JSValueConst name, type_params, compute_value;
    struct JSFunctionBytecode *b;
    QPCodeInfo *ci;
    JSValue qualname, ta;

    if (!qp_tuple_check(ctx, args) || QP_TUPLE_SIZE(args) != 3)
        return qp_raise_system_error(ctx, "bad argument to internal function");
    name = QP_TUPLE_ITEMS(args)[0];
    type_params = QP_TUPLE_ITEMS(args)[1];
    compute_value = QP_TUPLE_ITEMS(args)[2];
    /* typealias_convert_type_params */
    if (qp_typing_is_none(type_params) ||
        (qp_tuple_check(ctx, type_params) && QP_TUPLE_SIZE(type_params) == 0))
        type_params = QP_NULL;

    b = qp_code_bytecode(compute_value);
    ci = b ? qp_code_info(b) : NULL;
    if (ci && ci->qualname != JS_ATOM_NULL)
        qualname = qp_str_from_atom(ctx, ci->qualname);
    else
        qualname = qp_getattr(ctx, compute_value, QP_ATOM(ctx, __qualname__));
    if (JS_IsException(qualname))
        return qualname;
    ta = qp_typealias_alloc(ctx, name, qualname, type_params, compute_value, QP_NULL, QP_NULL);
    JS_FreeValue(ctx, qualname);
    return ta;
}

static const QPMemberDef qp_typealias_members[] = {
    { "__name__", QP_TA_NAME, QP_MEMBER_READONLY | QP_MEMBER_NONE_IF_NULL, NULL },
    { "__qualname__", QP_TA_QUALNAME, QP_MEMBER_READONLY | QP_MEMBER_NONE_IF_NULL, NULL },
    { NULL }
};

static const QPGetSetDef qp_typealias_getsets[] = {
    { "__parameters__", qp_typealias_parameters, NULL, 0, NULL },
    { "__type_params__", qp_typealias_type_params, NULL, 0, NULL },
    { "__value__", qp_typealias_value, NULL, 0, NULL },
    { "evaluate_value", qp_typealias_evaluate_value, NULL, 0, NULL },
    { "__module__", qp_typealias_module, qp_typealias_set_module, 0, NULL },
    { NULL }
};

static const QPMethodDef qp_typealias_methods[] = {
    { "__reduce__", qp_typing_reduce, QP_METH_NOARGS, 0, "__reduce__($self, /)\n--\n\n" },
    { NULL }
};

static const QPTypeSpec qp_typealias_spec = {
    .name = "typing.TypeAliasType",
    .doc = qp_typealias_doc,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .nslots = QP_TA_NSLOTS,
    .slots = {
        .tp_repr = qp_typealias_repr,
        .tp_iter = qp_typing_unpack_iter,
        .tp_new = qp_typealias_new,
        .nb = {
            .nb_or = qp_union_new,
        },
        .mp = {
            .mp_subscript = qp_typealias_subscript,
        },
    },
    .methods = qp_typealias_methods,
    .getsets = qp_typealias_getsets,
    .members = qp_typealias_members,
};

/* ---- Generic ------------------------------------------------------------ */

/* Generic.__class_getitem__ (magic 0) and Generic.__init_subclass__
   (magic 1): typing._generic_class_getitem / _generic_init_subclass(cls,
   *args, **kwargs) (call_typing_args_kwargs). */
static JSValue qp_generic_classmeth(JSContext *ctx, JSValueConst cls, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    Py_ssize_t i, nkw = qp_typing_kwcount(kwnames);
    JSValueConst *argv;
    JSValue func, res;

    func = qp_typing_get(ctx, magic ? "_generic_init_subclass" : "_generic_class_getitem");
    if (JS_IsException(func))
        return func;
    argv = js_malloc(ctx, sizeof(JSValueConst) * (1 + nargs + nkw));
    if (!argv) {
        JS_FreeValue(ctx, func);
        return JS_EXCEPTION;
    }
    argv[0] = cls;
    for (i = 0; i < nargs + nkw; i++)
        argv[1 + i] = args[i];
    res = qp_vectorcall(ctx, func, argv, 1 + nargs, nkw ? kwnames : QP_NULL);
    js_free(ctx, argv);
    JS_FreeValue(ctx, func);
    return res;
}

static const QPMethodDef qp_generic_methods[] = {
    { "__class_getitem__", qp_generic_classmeth,
      QP_METH_FASTCALL | QP_METH_KEYWORDS | QP_METH_CLASS, 0, qp_generic_class_getitem_doc },
    { "__init_subclass__", qp_generic_classmeth,
      QP_METH_FASTCALL | QP_METH_KEYWORDS | QP_METH_CLASS, 1, "Function to initialize subclasses." },
    { NULL }
};

static const QPTypeSpec qp_generic_spec = {
    .name = "typing.Generic",
    .doc = qp_generic_doc,
    .flags = QP_TPFLAGS_DEFAULT | QP_TPFLAGS_BASETYPE,
    .base = -1,
    .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT,
    .methods = qp_generic_methods,
};

/* _Py_subscript_generic (INTRINSIC_SUBSCRIPT_GENERIC): the implicit
   Generic[*params] base of `class C[T]: ...`. */
static JSValue qp_subscript_generic(JSContext *ctx, JSValueConst params)
{
    JSValueConst a[2];
    JSValue p, res;

    if (!qp_tuple_check(ctx, params))
        return qp_raise_system_error(ctx, "bad argument to internal function");
    p = qp_unpack_typevartuples(ctx, params);
    if (JS_IsException(p))
        return p;
    a[0] = QP_TYPE_VALUE(QP_TYPE(ctx, Generic));
    a[1] = p;
    res = qp_call_typing_func(ctx, "_GenericAlias", a, 2);
    JS_FreeValue(ctx, p);
    return res;
}

/* ---- intrinsics (Python/intrinsics.c) ----------------------------------- */

static JSValue qp_typing_intrinsic1(JSContext *ctx, int id, JSValueConst v)
{
    switch (id) {
    case QP_INTRINSIC1_TYPEVAR:
        /* _Py_make_typevar(name, NULL, NULL) */
        return qp_typevar_alloc(ctx, v, QP_NULL, QP_NULL, QP_NULL, QP_NULL, QP_NULL,
                                FALSE, FALSE, TRUE, QP_NULL);
    case QP_INTRINSIC1_PARAMSPEC:
        return qp_paramspec_alloc(ctx, QP_TYPE(ctx, ParamSpec), v, QP_NULL, QP_NULL,
                                  FALSE, FALSE, TRUE, QP_NULL);
    case QP_INTRINSIC1_TYPEVARTUPLE:
        return qp_paramspec_alloc(ctx, QP_TYPE(ctx, TypeVarTuple), v, QP_NULL, QP_NULL,
                                  FALSE, FALSE, TRUE, QP_NULL);
    case QP_INTRINSIC1_SUBSCRIPT_GENERIC:
        return qp_subscript_generic(ctx, v);
    case QP_INTRINSIC1_TYPEALIAS:
        return qp_make_typealias(ctx, v);
    default:
        return qp_raise_system_error(ctx, "invalid intrinsic function");
    }
}

static JSValue qp_typing_intrinsic2(JSContext *ctx, int id, JSValueConst a, JSValueConst b)
{
    switch (id) {
    case QP_INTRINSIC2_TYPEVAR_WITH_BOUND:
        /* _Py_make_typevar(name, evaluate_bound, NULL) */
        return qp_typevar_alloc(ctx, a, QP_NULL, b, QP_NULL, QP_NULL, QP_NULL,
                                FALSE, FALSE, TRUE, QP_NULL);
    case QP_INTRINSIC2_TYPEVAR_WITH_CONSTRAINTS:
        /* _Py_make_typevar(name, NULL, evaluate_constraints) */
        return qp_typevar_alloc(ctx, a, QP_NULL, QP_NULL, QP_NULL, b, QP_NULL,
                                FALSE, FALSE, TRUE, QP_NULL);
    case QP_INTRINSIC2_SET_TYPEPARAM_DEFAULT:
        return qp_set_typeparam_default(ctx, a, b);
    default:
        return qp_raise_system_error(ctx, "invalid intrinsic function");
    }
}

/* ---- initialization and the _typing module ------------------------------ */

/* Ready the typing types and create typing.NoDefault (init step 13). */
static int qp_init_typevar(JSContext *ctx)
{
    JSValue nodefault;

    if (qp_type_ready(ctx, QP_TYPE(ctx, NoDefaultType), &qp_nodefault_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, Generic), &qp_generic_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, TypeVar), &qp_typevar_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, TypeVarTuple), &qp_typevartuple_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, ParamSpec), &qp_paramspec_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, ParamSpecArgs), &qp_paramspecargs_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, ParamSpecKwargs), &qp_paramspeckwargs_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, ConstEvaluator), &qp_constevaluator_spec) < 0 ||
        qp_type_ready(ctx, QP_TYPE(ctx, TypeAliasType), &qp_typealias_spec) < 0)
        return -1;
    /* CPython creates Generic with PyType_FromSpec (a heap type), so
       type_ready_set_new inherits object.__new__ instead of disallowing
       instantiation as for a static type without tp_new. */
    QP_TYPE(ctx, Generic)->flags &= ~QP_TPFLAGS_DISALLOW_INSTANTIATION;
    QP_TYPE(ctx, Generic)->slots.tp_new = QP_TYPE(ctx, object)->slots.tp_new;
    {
        /* PyType_FromModuleAndSpec heap types (_PyTypeAlias_Type and
           NoDefault are static): Py_TPFLAGS_HEAPTYPE, __module__ in the
           dict.  copyreg._reduce_ex walks past them (pickling a Generic
           subclass with protocol < 2). */
        static const int heap_ids[] = {
            QP_T_Generic, QP_T_TypeVar, QP_T_TypeVarTuple, QP_T_ParamSpec,
            QP_T_ParamSpecArgs, QP_T_ParamSpecKwargs,
        };
        JSValue mod = qp_str_from_cstr(ctx, "typing");
        size_t i;
        int r = 0;

        if (JS_IsException(mod))
            return -1;
        for (i = 0; i < countof(heap_ids) && r >= 0; i++) {
            QPType *t = QP_STATE(ctx)->types[heap_ids[i]];
            t->flags |= QP_TPFLAGS_HEAPTYPE;
            r = qp_dict_set_item_atom(ctx, t->dict, QP_ATOM(ctx, __module__), mod);
        }
        JS_FreeValue(ctx, mod);
        if (r < 0)
            return -1;
    }
    nodefault = qp_object_new_instance(ctx, QP_TYPE(ctx, NoDefaultType), JS_CLASS_PY_OBJECT);
    if (JS_IsException(nodefault))
        return -1;
    JS_FreeValue(ctx, QP_STATE(ctx)->typing_nodefault);
    QP_STATE(ctx)->typing_nodefault = nodefault;
    return 0;
}

/* _typing._idfunc(x, /): helper making typing.NewType.__call__ faster. */
static JSValue qp_typing_idfunc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    return JS_DupValue(ctx, args[0]);
}

static const QPMethodDef qp_typing_methods[] = {
    { "_idfunc", qp_typing_idfunc, QP_METH_O, 0, "_idfunc($module, x, /)\n--\n\n" },
    { NULL }
};

static int qp_typing_module_add(JSContext *ctx, JSValueConst dict, const char *name,
                                JSValueConst value)
{
    JSValue key;
    int r;

    key = qp_str_from_cstr(ctx, name);
    if (JS_IsException(key))
        return -1;
    r = qp_dict_set_item(ctx, dict, key, value);
    JS_FreeValue(ctx, key);
    return r;
}

/* Modules/_typingmodule.c _typing_exec: export the typing types and
   NoDefault (the module def adds __doc__ and _idfunc). */
static int qp_typing_module_exec(JSContext *ctx, JSValueConst module)
{
    static const struct {
        const char *name;
        int type_id;
    } exported[] = {
        { "TypeVar", QP_T_TypeVar },
        { "TypeVarTuple", QP_T_TypeVarTuple },
        { "ParamSpec", QP_T_ParamSpec },
        { "ParamSpecArgs", QP_T_ParamSpecArgs },
        { "ParamSpecKwargs", QP_T_ParamSpecKwargs },
        { "Generic", QP_T_Generic },
        { "TypeAliasType", QP_T_TypeAliasType },
        { "Union", QP_T_Union },
    };
    JSValueConst dict = qp_module_get_dict(module);
    size_t i;

    for (i = 0; i < countof(exported); i++) {
        if (qp_typing_module_add(ctx, dict, exported[i].name,
                                 QP_TYPE_VALUE(QP_STATE(ctx)->types[exported[i].type_id])) < 0)
            return -1;
    }
    return qp_typing_module_add(ctx, dict, "NoDefault", QP_STATE(ctx)->typing_nodefault);
}

static const QPModuleDef qp_moddef__typing = {
    "_typing",
    "Primitives and accelerators for the typing module.\n",
    qp_typing_methods,
    qp_typing_module_exec,
};
