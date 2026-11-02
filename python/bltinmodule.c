/*
 * dquickts Python: the builtins module (object.h section (k)).
 *
 * Derived from CPython Python/bltinmodule.c, Objects/enumobject.c,
 * Objects/sentinelobject.c, the anext_awaitable part of
 * Objects/iterobject.c and Python/pythonrun.c (_Py_SourceAsString)
 * (PSF-2.0).
 *
 * Builtin functions are QPMethodDef entries turned into
 * builtin_function_or_method objects bound to the module.  The iterator
 * types map, filter, zip, enumerate, reversed (and sentinel,
 * anext_awaitable) keep their fields in QPInstance slots so that heap
 * subclasses inherit the layout.
 */

/* Doc strings (Argument Clinic text of CPython 3.15). */
static const char qp_bi_doc_builtin___import__[] =
    "__import__($module, /, name, globals=None, locals=None, fromlist=(),\n"
    "           level=0)\n"
    "--\n"
    "\n"
    "Import a module.\n"
    "\n"
    "Because this function is meant for use by the Python\n"
    "interpreter and not for general use, it is better to use\n"
    "importlib.import_module() to programmatically import a module.\n"
    "\n"
    "The globals argument is only used to determine the context;\n"
    "they are not modified.  The locals argument is unused.  The fromlist\n"
    "should be a list of names to emulate ``from name import ...``, or an\n"
    "empty list to emulate ``import name``.\n"
    "When importing a module from a package, note that __import__('A.B', ...)\n"
    "returns package A when fromlist is empty, but its submodule B when\n"
    "fromlist is not empty.  The level argument is used to determine whether\n"
    "to perform absolute or relative imports: 0 is absolute, while a positive\n"
    "number is the number of parent directories to search relative to the\n"
    "current module.";

static const char qp_bi_doc_builtin___lazy_import__[] =
    "__lazy_import__($module, /, name, globals=None, locals=None,\n"
    "                fromlist=(), level=0)\n"
    "--\n"
    "\n"
    "Lazily imports a module.\n"
    "\n"
    "Returns either the module to be imported or a imp.lazy_module object\n"
    "which indicates the module to be lazily imported.";

static const char qp_bi_doc_builtin_abs[] =
    "abs($module, number, /)\n"
    "--\n"
    "\n"
    "Return the absolute value of the argument.";

static const char qp_bi_doc_builtin_all[] =
    "all($module, iterable, /)\n"
    "--\n"
    "\n"
    "Return True if bool(x) is True for all values x in the iterable.\n"
    "\n"
    "If the iterable is empty, return True.";

static const char qp_bi_doc_builtin_any[] =
    "any($module, iterable, /)\n"
    "--\n"
    "\n"
    "Return True if bool(x) is True for any x in the iterable.\n"
    "\n"
    "If the iterable is empty, return False.";

static const char qp_bi_doc_builtin_ascii[] =
    "ascii($module, obj, /)\n"
    "--\n"
    "\n"
    "Return an ASCII-only representation of an object.\n"
    "\n"
    "As repr(), return a string containing a printable representation of an\n"
    "object, but escape the non-ASCII characters in the string returned by\n"
    "repr() using \\\\x, \\\\u or \\\\U escapes. This generates a string similar\n"
    "to that returned by repr() in Python 2.";

static const char qp_bi_doc_builtin_bin[] =
    "bin($module, integer, /)\n"
    "--\n"
    "\n"
    "Return the binary representation of an integer.\n"
    "\n"
    "   >>> bin(2796202)\n"
    "   '0b1010101010101010101010'";

static const char qp_bi_doc_builtin_callable[] =
    "callable($module, obj, /)\n"
    "--\n"
    "\n"
    "Return whether the object is callable (i.e., some kind of function).\n"
    "\n"
    "Note that classes are callable, as are instances of classes with a\n"
    "__call__() method.";

static const char qp_bi_doc_builtin_format[] =
    "format($module, value, format_spec='', /)\n"
    "--\n"
    "\n"
    "Return type(value).__format__(value, format_spec)\n"
    "\n"
    "Many built-in types implement format_spec according to the\n"
    "Format Specification Mini-language. See help('FORMATTING').\n"
    "\n"
    "If type(value) does not supply a method named __format__\n"
    "and format_spec is empty, then str(value) is returned.\n"
    "See also help('SPECIALMETHODS').";

static const char qp_bi_doc_builtin_chr[] =
    "chr($module, i, /)\n"
    "--\n"
    "\n"
    "Return a Unicode string of one character with ordinal i; 0 <= i <= 0x10ffff.";

static const char qp_bi_doc_builtin_compile[] =
    "compile($module, /, source, filename, mode, flags=0,\n"
    "        dont_inherit=False, optimize=-1, *, module=None,\n"
    "        _feature_version=-1)\n"
    "--\n"
    "\n"
    "Compile source into a code object that can be executed by exec() or eval().\n"
    "\n"
    "The source code may represent a Python module, statement or\n"
    "expression.\n"
    "The filename will be used for run-time error messages.\n"
    "The mode must be 'exec' to compile a module, 'single' to compile a\n"
    "single (interactive) statement, or 'eval' to compile an expression.\n"
    "The flags argument, if present, controls which future statements\n"
    "influence the compilation of the code.\n"
    "The dont_inherit argument, if true, stops the compilation inheriting\n"
    "the effects of any future statements in effect in the code calling\n"
    "compile; if absent or false these statements do influence the\n"
    "compilation, in addition to any features explicitly specified.";

static const char qp_bi_doc_builtin_divmod[] =
    "divmod($module, x, y, /)\n"
    "--\n"
    "\n"
    "Return the tuple (x//y, x%y).  Invariant: div*y + mod == x.";

static const char qp_bi_doc_builtin_eval[] =
    "eval($module, source, /, globals=None, locals=None)\n"
    "--\n"
    "\n"
    "Evaluate the given source in the context of globals and locals.\n"
    "\n"
    "The source may be a string representing a Python expression\n"
    "or a code object as returned by compile().\n"
    "The globals must be a dictionary and locals can be any mapping,\n"
    "defaulting to the current globals and locals.\n"
    "If only globals is given, locals defaults to it.";

static const char qp_bi_doc_builtin_exec[] =
    "exec($module, source, /, globals=None, locals=None, *, closure=None)\n"
    "--\n"
    "\n"
    "Execute the given source in the context of globals and locals.\n"
    "\n"
    "The source may be a string representing one or more Python statements\n"
    "or a code object as returned by compile().\n"
    "The globals must be a dictionary and locals can be any mapping,\n"
    "defaulting to the current globals and locals.\n"
    "If only globals is given, locals defaults to it.\n"
    "The closure must be a tuple of cellvars, and can only be used\n"
    "when source is a code object requiring exactly that many cellvars.";

static const char qp_bi_doc_builtin_globals[] =
    "globals($module, /)\n"
    "--\n"
    "\n"
    "Return the dictionary containing the current scope's global variables.\n"
    "\n"
    "NOTE: Updates to this dictionary *will* affect name lookups in the\n"
    "current global scope and vice-versa.";

static const char qp_bi_doc_builtin_hasattr[] =
    "hasattr($module, obj, name, /)\n"
    "--\n"
    "\n"
    "Return whether the object has an attribute with the given name.\n"
    "\n"
    "This is done by calling getattr(obj, name) and catching AttributeError.";

static const char qp_bi_doc_builtin_id[] =
    "id($module, obj, /)\n"
    "--\n"
    "\n"
    "Return the identity of an object.\n"
    "\n"
    "This is guaranteed to be unique among simultaneously existing objects.\n"
    "(CPython uses the object's memory address.)";

static const char qp_bi_doc_builtin_setattr[] =
    "setattr($module, obj, name, value, /)\n"
    "--\n"
    "\n"
    "Sets the named attribute on the given object to the specified value.\n"
    "\n"
    "setattr(x, 'y', v) is equivalent to ``x.y = v``";

static const char qp_bi_doc_builtin_delattr[] =
    "delattr($module, obj, name, /)\n"
    "--\n"
    "\n"
    "Deletes the named attribute from the given object.\n"
    "\n"
    "delattr(x, 'y') is equivalent to ``del x.y``";

static const char qp_bi_doc_builtin_hash[] =
    "hash($module, obj, /)\n"
    "--\n"
    "\n"
    "Return the integer hash value for the given object.\n"
    "\n"
    "Two objects that compare equal must also have the same hash value, but\n"
    "the reverse is not necessarily true.  Hash values may differ between\n"
    "Python processes.  Not all objects are hashable; calling hash() on an\n"
    "unhashable object raises TypeError.";

static const char qp_bi_doc_builtin_hex[] =
    "hex($module, integer, /)\n"
    "--\n"
    "\n"
    "Return the hexadecimal representation of an integer.\n"
    "\n"
    "   >>> hex(12648430)\n"
    "   '0xc0ffee'";

static const char qp_bi_doc_builtin_aiter[] =
    "aiter($module, async_iterable, /)\n"
    "--\n"
    "\n"
    "Return an AsyncIterator for an AsyncIterable object.";

static const char qp_bi_doc_builtin_anext[] =
    "anext($module, async_iterator, default=<unrepresentable>, /)\n"
    "--\n"
    "\n"
    "Return the next item from the async iterator.\n"
    "\n"
    "If default is given and the async iterator is exhausted,\n"
    "it is returned instead of raising StopAsyncIteration.";

static const char qp_bi_doc_builtin_len[] =
    "len($module, obj, /)\n"
    "--\n"
    "\n"
    "Return the number of items in a container.";

static const char qp_bi_doc_builtin_locals[] =
    "locals($module, /)\n"
    "--\n"
    "\n"
    "Return a dictionary containing the current scope's local variables.\n"
    "\n"
    "NOTE: Whether or not updates to this dictionary will affect name\n"
    "lookups in the local scope and vice-versa is *implementation\n"
    "dependent* and not covered by any backwards compatibility\n"
    "guarantees.";

static const char qp_bi_doc_builtin_oct[] =
    "oct($module, integer, /)\n"
    "--\n"
    "\n"
    "Return the octal representation of an integer.\n"
    "\n"
    "   >>> oct(342391)\n"
    "   '0o1234567'";

static const char qp_bi_doc_builtin_ord[] =
    "ord($module, character, /)\n"
    "--\n"
    "\n"
    "Return the ordinal value of a character.\n"
    "\n"
    "If the argument is a one-character string, return the Unicode code\n"
    "point of that character.\n"
    "\n"
    "If the argument is a bytes or bytearray object of length 1, return its\n"
    "single byte value.";

static const char qp_bi_doc_builtin_pow[] =
    "pow($module, /, base, exp, mod=None)\n"
    "--\n"
    "\n"
    "Equivalent to base**exp with 2 arguments or base**exp % mod with 3 arguments\n"
    "\n"
    "Some types, such as ints, are able to use a more efficient algorithm\n"
    "when invoked using the three argument form.";

static const char qp_bi_doc_builtin_print[] =
    "print($module, /, *objects, sep=' ', end='\\n', file=None, flush=False)\n"
    "--\n"
    "\n"
    "Prints the values to a stream, or to sys.stdout by default.\n"
    "\n"
    "  sep\n"
    "    string inserted between values, default a space.\n"
    "  end\n"
    "    string appended after the last value, default a newline.\n"
    "  file\n"
    "    a file-like object (stream); defaults to the current sys.stdout.\n"
    "  flush\n"
    "    whether to forcibly flush the stream.";

static const char qp_bi_doc_builtin_input[] =
    "input($module, prompt='', /)\n"
    "--\n"
    "\n"
    "Read a string from standard input.  The trailing newline is stripped.\n"
    "\n"
    "The prompt string, if given, is printed to standard output without a\n"
    "trailing newline before reading input.\n"
    "\n"
    "If the user hits EOF (*nix: Ctrl-D, Windows: Ctrl-Z+Return), raise\n"
    "EOFError.\n"
    "On *nix systems, readline is used if available.";

static const char qp_bi_doc_builtin_repr[] =
    "repr($module, obj, /)\n"
    "--\n"
    "\n"
    "Return the canonical string representation of the object.\n"
    "\n"
    "For many object types, including most builtins, eval(repr(obj)) == obj.";

static const char qp_bi_doc_builtin_round[] =
    "round($module, /, number, ndigits=None)\n"
    "--\n"
    "\n"
    "Round a number to a given precision in decimal digits.\n"
    "\n"
    "The return value is an integer if ndigits is omitted or None.\n"
    "Otherwise the return value has the same type as the number.  ndigits\n"
    "may be negative.";

static const char qp_bi_doc_builtin_sum[] =
    "sum($module, iterable, /, start=0)\n"
    "--\n"
    "\n"
    "Return the sum of a 'start' value (default: 0) plus an iterable of numbers\n"
    "\n"
    "When the iterable is empty, return the start value.\n"
    "This function is intended specifically for use with numeric values and\n"
    "may reject non-numeric types.";

static const char qp_bi_doc_builtin_isinstance[] =
    "isinstance($module, obj, class_or_tuple, /)\n"
    "--\n"
    "\n"
    "Return whether an object is an instance of a class or of a subclass thereof.\n"
    "\n"
    "A tuple, as in ``isinstance(x, (A, B, ...))``, may be given as the\n"
    "target to check against.  This is equivalent to ``isinstance(x, A) or\n"
    "isinstance(x, B) or ...`` etc.";

static const char qp_bi_doc_builtin_issubclass[] =
    "issubclass($module, cls, class_or_tuple, /)\n"
    "--\n"
    "\n"
    "Return whether 'cls' is derived from another class or is the same class.\n"
    "\n"
    "A tuple, as in ``issubclass(x, (A, B, ...))``, may be given as the\n"
    "target to check against.  This is equivalent to ``issubclass(x, A) or\n"
    "issubclass(x, B) or ...``.";

static const char qp_bi_doc_build_class[] =
    "__build_class__(func, name, /, *bases, [metaclass], **kwds) -> class\n"
    "\n"
    "Internal helper function used by the class statement.";

static const char qp_bi_doc_breakpoint[] =
    "breakpoint($module, /, *args, **kws)\n"
    "--\n"
    "\n"
    "Call sys.breakpointhook(*args, **kws).  sys.breakpointhook() must accept\n"
    "whatever arguments are passed.\n"
    "\n"
    "By default, this drops you into the pdb debugger.";

static const char qp_bi_doc_reduce[] =
    "Return state information for pickling.";

static const char qp_bi_doc_filter[] =
    "filter(function, iterable, /)\n"
    "--\n"
    "\n"
    "Return an iterator yielding those items of iterable for which\n"
    "function(item) is true.  If function is None, return the items that\n"
    "are true.";

static const char qp_bi_doc_dir[] =
    "dir([object]) -> list of strings\n"
    "\n"
    "If called without an argument, return the names in the current scope.\n"
    "Else, return an alphabetized list of names comprising (some of) the\n"
    "attributes of the given object, and of attributes reachable from it.\n"
    "If the object supplies a method named __dir__, it will be used;\n"
    "otherwise the default dir() logic is used and returns:\n"
    "  for a module object: the module's attributes.\n"
    "  for a class object:  its attributes, and recursively the attributes\n"
    "    of its bases.\n"
    "  for any other object: its attributes, its class's attributes, and\n"
    "    recursively the attributes of its class's base classes.";

static const char qp_bi_doc_getattr[] =
    "getattr(object, name[, default]) -> value\n"
    "\n"
    "Get a named attribute from an object.\n"
    "\n"
    "getattr(x, 'y') is equivalent to x.y.\n"
    "When a default argument is given, it is returned when the attribute\n"
    "doesn't exist; without it, an exception is raised in that case.";

static const char qp_bi_doc_setstate[] =
    "Set state information for unpickling.";

static const char qp_bi_doc_map[] =
    "map(function, iterable, /, *iterables, strict=False)\n"
    "--\n"
    "\n"
    "Make an iterator that computes the function using arguments from\n"
    "each of the iterables.  Stops when the shortest iterable is exhausted.\n"
    "\n"
    "If strict is true and one of the arguments is exhausted before the\n"
    "others, raise a ValueError.";

static const char qp_bi_doc_next[] =
    "next(iterator[, default])\n"
    "\n"
    "Return the next item from the iterator.  If default is given and the\n"
    "iterator is exhausted, it is returned instead of raising StopIteration.";

static const char qp_bi_doc_iter[] =
    "iter(iterable) -> iterator\n"
    "iter(callable, sentinel) -> iterator\n"
    "\n"
    "Get an iterator from an object.  In the first form, the argument must\n"
    "supply its own iterator, or be a sequence.\n"
    "In the second form, the callable is called until it returns the\n"
    "sentinel.";

static const char qp_bi_doc_min[] =
    "min(iterable, *[, default=obj, key=func]) -> value\n"
    "min(arg1, arg2, *args, *[, key=func]) -> value\n"
    "\n"
    "With a single iterable argument, return its smallest item. The\n"
    "default keyword-only argument specifies an object to return if\n"
    "the provided iterable is empty.\n"
    "With two or more positional arguments, return the smallest argument.";

static const char qp_bi_doc_max[] =
    "max(iterable, *[, default=obj, key=func]) -> value\n"
    "max(arg1, arg2, *args, *[, key=func]) -> value\n"
    "\n"
    "With a single iterable argument, return its biggest item. The\n"
    "default keyword-only argument specifies an object to return if\n"
    "the provided iterable is empty.\n"
    "With two or more positional arguments, return the largest argument.";

static const char qp_bi_doc_builtin_sorted[] =
    "sorted($module, iterable, /, *, key=None, reverse=False)\n"
    "--\n"
    "\n"
    "Return a new list containing all items from the iterable in ascending order.\n"
    "\n"
    "A custom key function can be supplied to customize the sort order, and the\n"
    "reverse flag can be set to request the result in descending order.";

static const char qp_bi_doc_vars[] =
    "vars([object]) -> dictionary\n"
    "\n"
    "Without arguments, equivalent to locals().\n"
    "With an argument, equivalent to object.__dict__.";

static const char qp_bi_doc_zip[] =
    "zip(*iterables, strict=False)\n"
    "--\n"
    "\n"
    "The zip object yields n-length tuples, where n is the number of\n"
    "iterables passed as positional arguments to zip().  The i-th element\n"
    "in every tuple comes from the i-th iterable argument to zip().  This\n"
    "continues until the shortest argument is exhausted.\n"
    "\n"
    "If strict is true and one of the arguments is exhausted before the\n"
    "others, raise a ValueError.\n"
    "\n"
    "   >>> list(zip('abcdefg', range(3), range(4)))\n"
    "   [('a', 0, 0), ('b', 1, 1), ('c', 2, 2)]";

static const char qp_bi_doc_builtin[] =
    "Built-in functions, types, exceptions, and other objects.\n"
    "\n"
    "This module provides direct access to all 'built-in'\n"
    "identifiers of Python; for example, builtins.len is\n"
    "the full name for the built-in function len().\n"
    "\n"
    "This module is not normally accessed explicitly by most\n"
    "applications, but can be useful in modules that provide\n"
    "objects with the same name as a built-in value, but in\n"
    "which the built-in of that name is also needed.";

static const char qp_bi_doc_enum_enum_new[] =
    "enumerate(iterable, start=0)\n"
    "--\n"
    "\n"
    "Return an enumerate object.\n"
    "\n"
    "  iterable\n"
    "    an object supporting iteration\n"
    "\n"
    "The enumerate object yields pairs containing a count (from start, which\n"
    "defaults to zero) and a value yielded by the iterable argument.\n"
    "\n"
    "enumerate is useful for obtaining an indexed list:\n"
    "    (0, seq[0]), (1, seq[1]), (2, seq[2]), ...";

static const char qp_bi_doc_enum_reversed_new[] =
    "reversed(object, /)\n"
    "--\n"
    "\n"
    "Return a reverse iterator over the values of the given sequence.";

static const char qp_bi_doc_enumobj_reduce[] =
    "Return state information for pickling.";

static const char qp_bi_doc_enumobj_length_hint[] =
    "Private method returning an estimate of len(list(it)).";

static const char qp_bi_doc_enumobj_setstate[] =
    "Set state information for unpickling.";

static const char qp_bi_doc_sentinel[] =
    "sentinel(name, /, *, repr=None)\n"
    "--\n"
    "\n"
    "Create a unique sentinel object with the given name.";

/* Implemented by sysmodule.c (same unit): readies the sys-related and
   typing types, creates the sys module (QPState.sys_module/sys_dict) and
   registers it in sys.modules.  Called last by qp_init_builtins. */
static int qp_init_sys(JSContext *ctx);
/* Implemented by import.c (unit l): PyImport_ImportModuleLevelObject and
   _PyImport_LazyImportModuleLevelObject without the builtins.__import__
   indirection.  globals/locals/fromlist may be QP_NULL. */
static JSValue qp_import_module_level(JSContext *ctx, JSValueConst name, JSValueConst globals,
                                      JSValueConst locals, JSValueConst fromlist, int level);
static JSValue qp_lazy_import_module_level(JSContext *ctx, JSValueConst name,
                                           JSValueConst builtins_dict, JSValueConst globals,
                                           JSValueConst locals, JSValueConst fromlist, int level);
/* Implemented by ceval.c (unit a): PyEval_EvalCodeEx with a closure tuple
   of cells (QP_NULL = none). */
static JSValue qp_eval_code_closure(JSContext *ctx, JSValueConst code, JSValueConst globals,
                                    JSValueConst locals, JSValueConst closure);

/* PyCF_* (Include/cpython/compile.h) */
#define QP_BI_PyCF_MASK (CO_FUTURE_DIVISION | CO_FUTURE_ABSOLUTE_IMPORT | \
                         CO_FUTURE_WITH_STATEMENT | CO_FUTURE_PRINT_FUNCTION | \
                         CO_FUTURE_UNICODE_LITERALS | CO_FUTURE_BARRY_AS_BDFL | \
                         CO_FUTURE_GENERATOR_STOP | CO_FUTURE_ANNOTATIONS)
#define QP_BI_PyCF_MASK_OBSOLETE CO_NESTED
#define QP_BI_PyCF_ONLY_AST 0x0400
#define QP_BI_PyCF_OPTIMIZED_AST (0x8000 | QP_BI_PyCF_ONLY_AST)
#define QP_BI_PyCF_COMPILE_MASK (QP_BI_PyCF_ONLY_AST | QP_PF_TOP_LEVEL_AWAIT | \
                                 QP_PF_TYPE_COMMENTS | QP_PF_DONT_IMPLY_DEDENT | \
                                 QP_PF_ALLOW_INCOMPLETE_INPUT | QP_BI_PyCF_OPTIMIZED_AST)

/* ========================================================================= */
/* Small helpers                                                              */
/* ========================================================================= */

static inline BOOL qp_bi_is_none(JSValueConst v)
{
    return JS_IsUndefined(v) || JS_IsNull(v);
}

static inline BOOL qp_bi_absent(JSValueConst v)
{
    return QP_IS_NULL(v);
}

static inline int qp_bi_nkw(JSValueConst kwnames)
{
    if (QP_IS_NULL(kwnames) || JS_IsUndefined(kwnames))
        return 0;
    return (int)QP_TUPLE_SIZE(kwnames);
}

static inline BOOL qp_bi_has_class(JSValueConst v, JSClassID id)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT && JS_VALUE_GET_OBJ(v)->class_id == id;
}

static inline BOOL qp_bi_same(JSValueConst a, JSValueConst b)
{
    return JS_VALUE_GET_TAG(a) == JS_VALUE_GET_TAG(b) &&
        JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}

/* Py_TYPE(o)->tp_name as a str: the spec name of builtin types, __name__
   of heap types. */
static JSValue qp_bi_tp_name_of_type(JSContext *ctx, QPType *t)
{
    if (t->tp_name)
        return qp_str_from_cstr(ctx, t->tp_name);
    return qp_str_from_atom(ctx, t->name);
}

static JSValue qp_bi_tp_name(JSContext *ctx, JSValueConst v)
{
    return qp_bi_tp_name_of_type(ctx, qp_type_of(ctx, v));
}

/* Raise exc_type with fmt containing exactly one %U, filled with
   Py_TYPE(v)->tp_name. */
static JSValue qp_bi_raise_tp(JSContext *ctx, QPType *exc_type, const char *fmt, JSValueConst v)
{
    JSValue tn = qp_bi_tp_name(ctx, v);
    if (JS_IsException(tn))
        return JS_EXCEPTION;
    qp_raise(ctx, exc_type, fmt, tn);
    JS_FreeValue(ctx, tn);
    return JS_EXCEPTION;
}

/* _PyArg_BadArgument: "%s() %s must be %s, not %s". */
static JSValue qp_bi_bad_argument(JSContext *ctx, const char *fname, const char *displayname,
                                  const char *expected, JSValueConst arg)
{
    JSValue tn;
    if (qp_bi_is_none(arg))
        tn = qp_str_from_cstr(ctx, "None");
    else
        tn = qp_bi_tp_name(ctx, arg);
    if (JS_IsException(tn))
        return JS_EXCEPTION;
    qp_raise_type_error(ctx, "%.200s() %.200s must be %.50s, not %.50U", fname, displayname,
                        expected, tn);
    JS_FreeValue(ctx, tn);
    return JS_EXCEPTION;
}

/* Clear the pending exception. */
static void qp_bi_clear_exc(JSContext *ctx)
{
    JS_FreeValue(ctx, qp_exc_fetch(ctx));
}

/* PyMapping_Check */
static BOOL qp_bi_mapping_check(JSContext *ctx, JSValueConst v)
{
    return qp_type_of(ctx, v)->slots.mp.mp_subscript != NULL;
}

/* PyIter_Check */
static BOOL qp_bi_iter_check(JSContext *ctx, JSValueConst v)
{
    return qp_type_of(ctx, v)->slots.tp_iternext != NULL;
}

/* PyCallable_Check */
static BOOL qp_bi_callable(JSContext *ctx, JSValueConst v)
{
    if (qp_type_of(ctx, v)->slots.tp_call != NULL)
        return TRUE;
    return JS_IsFunction(ctx, v);
}

/* PySys_GetAttr: sys.<name> or RuntimeError "lost sys.<name>". */
static JSValue qp_bi_sys_attr(JSContext *ctx, JSAtom name)
{
    JSValue v = qp_sys_get(ctx, name);
    if (QP_IS_NULL(v)) {
        JSValue s = qp_str_from_atom(ctx, name);
        if (JS_IsException(s))
            return JS_EXCEPTION;
        qp_raise_runtime_error(ctx, "lost sys.%U", s);
        JS_FreeValue(ctx, s);
        return JS_EXCEPTION;
    }
    return v;
}

/* Attribute name argument of getattr & co. */
static JSAtom qp_bi_attr_name(JSContext *ctx, JSValueConst name)
{
    JSValue s;
    JSAtom atom;
    if (!qp_str_check(ctx, name)) {
        qp_bi_raise_tp(ctx, QP_EXC(ctx, TypeError),
                       "attribute name must be string, not '%.200U'", name);
        return JS_ATOM_NULL;
    }
    s = qp_str_value(ctx, name);
    if (JS_IsException(s))
        return JS_ATOM_NULL;
    atom = qp_str_to_atom(ctx, s);
    JS_FreeValue(ctx, s);
    return atom;
}

/* PyFile_WriteObject(v, f, Py_PRINT_RAW) */
static int qp_bi_file_write(JSContext *ctx, JSValueConst v, JSValueConst file)
{
    JSValue writer, value, r;
    writer = qp_getattr(ctx, file, QP_ATOM(ctx, write));
    if (JS_IsException(writer))
        return -1;
    value = qp_str(ctx, v);
    if (JS_IsException(value)) {
        JS_FreeValue(ctx, writer);
        return -1;
    }
    r = qp_call1(ctx, writer, value);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, writer);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

/* PyFile_WriteString */
static int qp_bi_file_write_cstr(JSContext *ctx, const char *s, JSValueConst file)
{
    JSValue v = qp_str_from_cstr(ctx, s);
    int r;
    if (JS_IsException(v))
        return -1;
    r = qp_bi_file_write(ctx, v, file);
    JS_FreeValue(ctx, v);
    return r;
}

/* _PyFile_Flush */
static int qp_bi_file_flush(JSContext *ctx, JSValueConst file)
{
    JSValue r = qp_call_method(ctx, file, QP_ATOM(ctx, flush), NULL, 0, QP_NULL);
    if (JS_IsException(r))
        return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

/* Python frame of the caller of the running builtin, or NULL. */
static JSStackFrame *qp_bi_caller_frame(JSContext *ctx)
{
    return qp_py_frame_at(ctx, 0);
}

/* ========================================================================= */
/* __build_class__                                                            */
/* ========================================================================= */

/* update_bases(): apply __mro_entries__ (PEP 560).  Returns a new
   reference; the same tuple as orig_bases when nothing changed. */
static JSValue qp_bi_update_bases(JSContext *ctx, JSValueConst orig_bases,
                                  JSValueConst *args, int nargs)
{
    JSValue new_bases = QP_NULL, meth, new_base, result;
    int i, j;

    for (i = 0; i < nargs; i++) {
        JSValueConst base = args[i];
        int r;
        if (qp_type_from_value(base)) {
            if (!QP_IS_NULL(new_bases)) {
                /* If we already have made a replacement, then we append
                   every normal base, otherwise just skip it. */
                if (qp_list_append(ctx, new_bases, base) < 0)
                    goto error;
            }
            continue;
        }
        r = qp_getattr_opt(ctx, base, QP_ATOM(ctx, __mro_entries__), &meth);
        if (r < 0)
            goto error;
        if (r == 0) {
            if (!QP_IS_NULL(new_bases)) {
                if (qp_list_append(ctx, new_bases, base) < 0)
                    goto error;
            }
            continue;
        }
        new_base = qp_call1(ctx, meth, orig_bases);
        JS_FreeValue(ctx, meth);
        if (JS_IsException(new_base))
            goto error;
        if (!qp_tuple_check(ctx, new_base)) {
            qp_raise_type_error(ctx, "__mro_entries__ must return a tuple");
            JS_FreeValue(ctx, new_base);
            goto error;
        }
        if (QP_IS_NULL(new_bases)) {
            /* If this is a first successful replacement, create new_bases
               list and copy previously encountered bases. */
            new_bases = qp_list_new(ctx, 0);
            if (JS_IsException(new_bases)) {
                new_bases = QP_NULL;
                JS_FreeValue(ctx, new_base);
                goto error;
            }
            for (j = 0; j < i; j++) {
                if (qp_list_append(ctx, new_bases, args[j]) < 0) {
                    JS_FreeValue(ctx, new_base);
                    goto error;
                }
            }
        }
        if (qp_list_extend(ctx, new_bases, new_base) < 0) {
            JS_FreeValue(ctx, new_base);
            goto error;
        }
        JS_FreeValue(ctx, new_base);
    }
    if (QP_IS_NULL(new_bases))
        return JS_DupValue(ctx, orig_bases);
    result = qp_list_as_tuple(ctx, new_bases);
    JS_FreeValue(ctx, new_bases);
    return result;

error:
    JS_FreeValue(ctx, new_bases);
    return JS_EXCEPTION;
}

/* _PyType_CalculateMetaclass */
static QPType *qp_bi_calculate_metaclass(JSContext *ctx, QPType *metatype, JSValueConst bases)
{
    QPType *winner = metatype;
    Py_ssize_t i, n = QP_TUPLE_SIZE(bases);

    for (i = 0; i < n; i++) {
        QPType *tmptype = qp_type_of(ctx, QP_TUPLE_ITEMS(bases)[i]);
        if (qp_type_is_subtype(winner, tmptype))
            continue;
        if (qp_type_is_subtype(tmptype, winner)) {
            winner = tmptype;
            continue;
        }
        qp_raise_type_error(ctx, "metaclass conflict: "
                            "the metaclass of a derived class "
                            "must be a (non-strict) subclass "
                            "of the metaclasses of all its bases");
        return NULL;
    }
    return winner;
}

/* PyObject_VectorcallDict(callable, args, nargs, kwdict) */
static JSValue qp_bi_call_dict(JSContext *ctx, JSValueConst callable, int nargs,
                               JSValueConst *args, JSValueConst kwdict)
{
    JSValue tup, r;
    if (QP_IS_NULL(kwdict) || qp_dict_size(kwdict) == 0)
        return qp_vectorcall(ctx, callable, args, nargs, QP_NULL);
    tup = qp_tuple_pack(ctx, nargs, args);
    if (JS_IsException(tup))
        return JS_EXCEPTION;
    r = qp_call_tuple(ctx, callable, tup, kwdict);
    JS_FreeValue(ctx, tup);
    return r;
}

static JSValue qp_bi_build_class(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst func, name;
    JSValue cls = JS_EXCEPTION, cell = JS_UNDEFINED, ns = JS_UNDEFINED, meta = JS_UNDEFINED;
    JSValue orig_bases, bases, mkw = QP_NULL, prep;
    QPType *metatype = NULL;
    int isclass = 0, nkw = qp_bi_nkw(kwnames), i, r;

    if (nargs < 2)
        return qp_raise_type_error(ctx, "__build_class__: not enough arguments");
    func = args[0];
    if (!qp_is_py_function(func))
        return qp_raise_type_error(ctx, "__build_class__: func must be a function");
    name = args[1];
    if (!qp_str_check(ctx, name))
        return qp_raise_type_error(ctx, "__build_class__: name is not a string");
    orig_bases = qp_tuple_pack(ctx, nargs - 2, args + 2);
    if (JS_IsException(orig_bases))
        return JS_EXCEPTION;
    bases = qp_bi_update_bases(ctx, orig_bases, args + 2, nargs - 2);
    if (JS_IsException(bases)) {
        JS_FreeValue(ctx, orig_bases);
        return JS_EXCEPTION;
    }

    meta = QP_NULL;
    if (nkw > 0) {
        JSValue key;
        mkw = qp_dict_new_presized(ctx, nkw);
        if (JS_IsException(mkw)) {
            mkw = QP_NULL;
            goto error;
        }
        for (i = 0; i < nkw; i++) {
            if (qp_dict_set_item(ctx, mkw, QP_TUPLE_ITEMS(kwnames)[i], args[nargs + i]) < 0)
                goto error;
        }
        key = qp_str_from_atom(ctx, QP_ATOM(ctx, metaclass));
        if (JS_IsException(key))
            goto error;
        r = qp_dict_pop(ctx, mkw, key, &meta);
        JS_FreeValue(ctx, key);
        if (r < 0) {
            meta = QP_NULL;
            goto error;
        }
        if (r > 0) {
            /* metaclass is explicitly given, check if it's indeed a class */
            isclass = qp_type_from_value(meta) != NULL;
        } else {
            meta = QP_NULL;
        }
    }
    if (QP_IS_NULL(meta)) {
        if (QP_TUPLE_SIZE(bases) == 0)
            metatype = QP_TYPE(ctx, type); /* if there are no bases, use type */
        else
            metatype = qp_type_of(ctx, QP_TUPLE_ITEMS(bases)[0]); /* type of the first base */
        meta = JS_DupValue(ctx, QP_TYPE_VALUE(metatype));
        isclass = 1; /* meta is really a class */
    }
    if (isclass) {
        /* meta is really a class, so check for a more derived metaclass,
           or possible metaclass conflicts */
        QPType *winner = qp_bi_calculate_metaclass(ctx, qp_type_from_value(meta), bases);
        if (!winner)
            goto error;
        if (winner != qp_type_from_value(meta)) {
            JS_FreeValue(ctx, meta);
            meta = JS_DupValue(ctx, QP_TYPE_VALUE(winner));
        }
        metatype = winner;
    }
    /* else: meta is not a class, so we cannot do the metaclass calculation,
       so we will use the explicitly given object as it is */
    r = qp_getattr_opt(ctx, meta, QP_ATOM(ctx, __prepare__), &prep);
    if (r < 0) {
        goto error;
    } else if (r == 0) {
        ns = qp_dict_new(ctx);
    } else {
        JSValueConst pargs[2] = { name, bases };
        ns = qp_bi_call_dict(ctx, prep, 2, pargs, mkw);
        JS_FreeValue(ctx, prep);
    }
    if (JS_IsException(ns)) {
        ns = JS_UNDEFINED;
        goto error;
    }
    if (!qp_bi_mapping_check(ctx, ns)) {
        JSValue mn = isclass ? qp_bi_tp_name_of_type(ctx, metatype)
                             : qp_str_from_cstr(ctx, "<metaclass>");
        JSValue tn;
        if (JS_IsException(mn))
            goto error;
        tn = qp_bi_tp_name(ctx, ns);
        if (!JS_IsException(tn)) {
            qp_raise_type_error(ctx, "%.200U.__prepare__() must return a mapping, not %.200U",
                                mn, tn);
            JS_FreeValue(ctx, tn);
        }
        JS_FreeValue(ctx, mn);
        goto error;
    }
    cell = qp_call_with_locals(ctx, func, ns);
    if (JS_IsException(cell)) {
        cell = JS_UNDEFINED;
        goto error;
    }
    if (!qp_bi_same(bases, orig_bases)) {
        JSValue key = qp_str_from_atom(ctx, QP_ATOM(ctx, __orig_bases__));
        if (JS_IsException(key))
            goto error;
        r = qp_setitem(ctx, ns, key, orig_bases);
        JS_FreeValue(ctx, key);
        if (r < 0)
            goto error;
    }
    {
        JSValueConst margs[3] = { name, bases, ns };
        cls = qp_bi_call_dict(ctx, meta, 3, margs, mkw);
    }
    if (!JS_IsException(cls) && qp_type_from_value(cls) && qp_bi_has_class(cell, JS_CLASS_PY_CELL)) {
        JSValue cell_cls = qp_cell_get(ctx, cell);
        if (!qp_bi_same(cell_cls, cls)) {
            if (QP_IS_NULL(cell_cls)) {
                qp_raise_runtime_error(ctx, "__class__ not set defining %.200R as %.200R. "
                                       "Was __classcell__ propagated to type.__new__?",
                                       name, cls);
            } else {
                qp_raise_type_error(ctx, "__class__ set to %.200R defining %.200R as %.200R",
                                    cell_cls, name, cls);
                JS_FreeValue(ctx, cell_cls);
            }
            JS_FreeValue(ctx, cls);
            cls = JS_EXCEPTION;
        } else {
            JS_FreeValue(ctx, cell_cls);
        }
    }
error:
    JS_FreeValue(ctx, cell);
    JS_FreeValue(ctx, ns);
    JS_FreeValue(ctx, meta);
    JS_FreeValue(ctx, mkw);
    JS_FreeValue(ctx, orig_bases);
    JS_FreeValue(ctx, bases);
    return cls;
}

/* ========================================================================= */
/* __import__, __lazy_import__, open                                          */
/* ========================================================================= */

static const char *const qp_bi_import_kw[] = { "name", "globals", "locals", "fromlist", "level", NULL };

static JSValue qp_bi_import(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    static const QPArgSpec spec = { "__import__", qp_bi_import_kw, 1, 5, 0 };
    JSValueConst a[5];
    int level = 0;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[4]) && qp_arg_int(ctx, a[4], &level) < 0)
        return JS_EXCEPTION;
    return qp_import_module_level(ctx, a[0], a[1], a[2], a[3], level);
}

static JSValue qp_bi_lazy_import(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    static const QPArgSpec spec = { "__lazy_import__", qp_bi_import_kw, 1, 5, 0 };
    JSValueConst a[5];
    JSValueConst globals, locals;
    JSValue builtins, res;
    int level = 0, r;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!QP_IS_NULL(a[4]) && qp_arg_int(ctx, a[4], &level) < 0)
        return JS_EXCEPTION;
    globals = a[1];
    if (QP_IS_NULL(globals)) {
        JSStackFrame *sf = qp_bi_caller_frame(ctx);
        globals = sf ? qp_frame_globals(sf) : QP_NULL;
        if (QP_IS_NULL(globals))
            return qp_raise_type_error(ctx, "__lazy_import__() missing globals "
                                       "when called without a frame");
    }
    locals = QP_IS_NULL(a[2]) ? globals : a[2];
    if (!qp_dict_check(ctx, globals))
        return qp_raise_type_error(ctx, "expect dict for globals, got %T", globals);
    r = qp_dict_get_item_atom(ctx, globals, QP_ATOM(ctx, __builtins__), &builtins);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return qp_raise_value_error(ctx, "unable to get builtins for lazy import");
    if (qp_module_check(ctx, builtins)) {
        JSValue d = qp_module_get_dict(builtins);
        if (QP_IS_NULL(d) || JS_IsUndefined(d)) {
            JS_FreeValue(ctx, builtins);
            return qp_raise(ctx, QP_EXC(ctx, AttributeError), "builtins module has no dict");
        }
        d = JS_DupValue(ctx, d);
        JS_FreeValue(ctx, builtins);
        builtins = d;
    }
    res = qp_lazy_import_module_level(ctx, a[0], builtins, globals, locals, a[3], level);
    JS_FreeValue(ctx, builtins);
    return res;
}

/* builtins.open is io.open (CPython installs it at startup from the io
   module).  Forwarding through the module keeps the same object
   semantics; without an io module the import error propagates. */
static JSValue qp_bi_open(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          int nargs, JSValueConst kwnames, int magic)
{
    JSValue io, f, r;
    io = qp_import_module(ctx, "io");
    if (JS_IsException(io))
        return JS_EXCEPTION;
    f = qp_getattr(ctx, io, QP_ATOM(ctx, open));
    JS_FreeValue(ctx, io);
    if (JS_IsException(f))
        return JS_EXCEPTION;
    r = qp_vectorcall(ctx, f, args, nargs, kwnames);
    JS_FreeValue(ctx, f);
    return r;
}

/* ========================================================================= */
/* Simple builtins                                                            */
/* ========================================================================= */

static JSValue qp_bi_abs(JSContext *ctx, JSValueConst self, JSValueConst *args,
                         int nargs, JSValueConst kwnames, int magic)
{
    QPType *t = qp_type_of(ctx, args[0]);
    if (t->slots.nb.nb_absolute)
        return t->slots.nb.nb_absolute(ctx, args[0]);
    return qp_bi_raise_tp(ctx, QP_EXC(ctx, TypeError), "bad operand type for abs(): '%.200U'",
                          args[0]);
}

/* all() (magic 0) and any() (magic 1) */
static JSValue qp_bi_all_any(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSValue it, item;
    int r;

    it = qp_iter(ctx, args[0]);
    if (JS_IsException(it))
        return JS_EXCEPTION;
    for (;;) {
        item = qp_next(ctx, it);
        if (JS_IsException(item)) {
            JS_FreeValue(ctx, it);
            return JS_EXCEPTION;
        }
        if (QP_IS_NULL(item))
            break;
        r = qp_bool(ctx, item);
        JS_FreeValue(ctx, item);
        if (r < 0) {
            JS_FreeValue(ctx, it);
            return JS_EXCEPTION;
        }
        if (magic == 0 && r == 0) {
            JS_FreeValue(ctx, it);
            return JS_FALSE;
        }
        if (magic == 1 && r > 0) {
            JS_FreeValue(ctx, it);
            return JS_TRUE;
        }
    }
    JS_FreeValue(ctx, it);
    return qp_bool_new(magic == 0);
}

static JSValue qp_bi_ascii(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    return qp_ascii(ctx, args[0]);
}

/* bin/oct/hex: PyNumber_ToBase(integer, magic) */
static JSValue qp_bi_to_base(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSValue idx = qp_index(ctx, args[0]), r;
    if (JS_IsException(idx))
        return JS_EXCEPTION;
    r = qp_long_to_str(ctx, idx, magic, 1);
    JS_FreeValue(ctx, idx);
    return r;
}

static JSValue qp_bi_breakpoint(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    JSValue hook = qp_bi_sys_attr(ctx, QP_ATOM(ctx, breakpointhook)), r;
    if (JS_IsException(hook))
        return JS_EXCEPTION;
    r = qp_vectorcall(ctx, hook, args, nargs, kwnames);
    JS_FreeValue(ctx, hook);
    return r;
}

static JSValue qp_bi_callable_fn(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    return qp_bool_new(qp_bi_callable(ctx, args[0]));
}

static JSValue qp_bi_chr(JSContext *ctx, JSValueConst self, JSValueConst *args,
                         int nargs, JSValueConst kwnames, int magic)
{
    JSValue idx;
    int64_t v;
    int overflow = 0;

    if (JS_VALUE_GET_TAG(args[0]) == JS_TAG_INT) {
        v = JS_VALUE_GET_INT(args[0]);
    } else {
        idx = qp_index(ctx, args[0]);
        if (JS_IsException(idx))
            return JS_EXCEPTION;
        if (qp_long_as_int64_overflow(idx, &v, &overflow) < 0) {
            JS_FreeValue(ctx, idx);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, idx);
        if (overflow)
            v = overflow < 0 ? INT_MIN : INT_MAX;
    }
    if (v < 0 || v > 0x10ffff)
        return qp_raise_value_error(ctx, "chr() arg not in range(0x110000)");
    return qp_str_from_char(ctx, (uint32_t)v);
}

static JSValue qp_bi_delattr(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSAtom name;
    int r;
    if (qp_arg_check_positional(ctx, "delattr", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    name = qp_bi_attr_name(ctx, args[1]);
    if (name == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_delattr(ctx, args[0], name);
    JS_FreeAtom(ctx, name);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* _dir_locals(): sorted keys of the caller's locals. */
static JSValue qp_bi_dir_locals(JSContext *ctx)
{
    JSStackFrame *sf = qp_bi_caller_frame(ctx);
    JSValue locals, names;

    if (!sf)
        return qp_raise_system_error(ctx, "frame does not exist");
    locals = qp_frame_locals(ctx, sf);
    if (JS_IsException(locals))
        return JS_EXCEPTION;
    if (qp_dict_check(ctx, locals)) {
        names = qp_dict_keys_list(ctx, locals);
    } else {
        JSValue keys = qp_call_method(ctx, locals, QP_ATOM(ctx, keys), NULL, 0, QP_NULL);
        if (JS_IsException(keys)) {
            names = JS_EXCEPTION;
        } else {
            names = qp_list_from_iterable(ctx, keys);
            JS_FreeValue(ctx, keys);
        }
    }
    JS_FreeValue(ctx, locals);
    if (JS_IsException(names))
        return JS_EXCEPTION;
    if (qp_list_sort(ctx, names, QP_NULL, 0) < 0) {
        JS_FreeValue(ctx, names);
        return JS_EXCEPTION;
    }
    return names;
}

static JSValue qp_bi_dir(JSContext *ctx, JSValueConst self, JSValueConst *args,
                         int nargs, JSValueConst kwnames, int magic)
{
    if (qp_arg_check_positional(ctx, "dir", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs == 0)
        return qp_bi_dir_locals(ctx);
    return qp_dir(ctx, args[0]);
}

/* PyNumber_Divmod: binary_op1 over nb_divmod. */
static JSValue qp_bi_number_divmod(JSContext *ctx, JSValueConst v, JSValueConst w)
{
    QPType *tv = qp_type_of(ctx, v), *tw = qp_type_of(ctx, w);
    QPBinaryFunc *slotv = tv->slots.nb.nb_divmod, *slotw = NULL;
    JSValue x, tnv, tnw;

    if (tw != tv) {
        slotw = tw->slots.nb.nb_divmod;
        if (slotw == slotv)
            slotw = NULL;
    }
    if (slotv) {
        if (slotw && qp_type_is_subtype(tw, tv)) {
            x = slotw(ctx, v, w);
            if (!qp_is_notimpl(ctx, x))
                return x;
            JS_FreeValue(ctx, x);
            slotw = NULL;
        }
        x = slotv(ctx, v, w);
        if (!qp_is_notimpl(ctx, x))
            return x;
        JS_FreeValue(ctx, x);
    }
    if (slotw) {
        x = slotw(ctx, v, w);
        if (!qp_is_notimpl(ctx, x))
            return x;
        JS_FreeValue(ctx, x);
    }
    tnv = qp_bi_tp_name(ctx, v);
    if (JS_IsException(tnv))
        return JS_EXCEPTION;
    tnw = qp_bi_tp_name(ctx, w);
    if (!JS_IsException(tnw)) {
        qp_raise_type_error(ctx, "unsupported operand type(s) for divmod(): '%.100U' and '%.100U'",
                            tnv, tnw);
        JS_FreeValue(ctx, tnw);
    }
    JS_FreeValue(ctx, tnv);
    return JS_EXCEPTION;
}

static JSValue qp_bi_divmod(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    if (qp_arg_check_positional(ctx, "divmod", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    return qp_bi_number_divmod(ctx, args[0], args[1]);
}

/* ========================================================================= */
/* compile / eval / exec                                                      */
/* ========================================================================= */

/* PyEval_MergeCompilerFlags: inherit the future flags of the calling
   code.  Returns the merged flags. */
static int qp_bi_merge_flags(JSContext *ctx, int flags)
{
    JSStackFrame *sf = qp_bi_caller_frame(ctx);
    struct JSFunctionBytecode *b;
    QPCodeInfo *ci;

    if (!sf)
        return flags;
    b = qp_code_bytecode(sf->cur_func);
    ci = b ? qp_code_info(b) : NULL;
    if (ci)
        flags |= (int)(ci->co_flags & QP_BI_PyCF_MASK);
    return flags;
}

/* _Py_SourceAsString: NUL-terminated copy of the source text in *pbuf
   (js_free it), QP_PF_IGNORE_COOKIE added for str sources. */
static int qp_bi_source_as_string(JSContext *ctx, JSValueConst cmd, const char *funcname,
                                  const char *what, int *flags, char **pbuf, size_t *plen)
{
    const char *str = NULL;
    size_t size = 0;
    char *buf;
    QPBuffer view;
    int have_view = 0;
    JSValue s = JS_UNDEFINED;

    *pbuf = NULL;
    if (qp_str_check(ctx, cmd)) {
        *flags |= QP_PF_IGNORE_COOKIE;
        s = qp_str_value(ctx, cmd);
        if (JS_IsException(s))
            return -1;
        str = qp_str_utf8(ctx, s, &size);
        if (!str) {
            JS_FreeValue(ctx, s);
            return -1;
        }
    } else if (qp_bytes_check(ctx, cmd) || qp_bytearray_check(ctx, cmd)) {
        Py_ssize_t n;
        str = (const char *)qp_bytes_data(cmd, &n);
        size = (size_t)n;
    } else if (qp_buffer_get(ctx, cmd, &view, QP_BUF_SIMPLE) == 0) {
        have_view = 1;
        str = (const char *)view.buf;
        size = (size_t)view.len;
    } else {
        qp_bi_clear_exc(ctx);
        qp_raise_type_error(ctx, "%s() arg 1 must be a %s object", funcname, what);
        return -1;
    }
    buf = js_malloc(ctx, size + 1);
    if (buf) {
        if (size)
            memcpy(buf, str, size);
        buf[size] = '\0';
    }
    if (!JS_IsUndefined(s)) {
        JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, s);
    }
    if (have_view)
        qp_buffer_release(ctx, &view);
    if (!buf)
        return -1;
    if (strlen(buf) != size) {
        js_free(ctx, buf);
        qp_raise(ctx, QP_EXC(ctx, SyntaxError), "source code string cannot contain null bytes");
        return -1;
    }
    *pbuf = buf;
    *plen = size;
    return 0;
}

/* Compile source text to a code object (_Py_CompileString). */
static JSValue qp_bi_compile_text(JSContext *ctx, const char *src, size_t len,
                                  const char *filename, int mode, int flags, int optimize)
{
    qp_error err;
    JSValue bc, code;

    memset(&err, 0, sizeof(err));
    bc = qp_compile_source(ctx, src, len, filename, mode, flags, optimize, &err);
    if (JS_IsException(bc)) {
        if (err.kind != QP_ERR_NONE) {
            qp_raise_qp_error(ctx, &err, filename);
            qp_error_clear(&err);
        }
        return JS_EXCEPTION;
    }
    qp_error_clear(&err);
    code = qp_code_new(ctx, bc);
    JS_FreeValue(ctx, bc);
    return code;
}

/* PyUnicode_FSDecoder for compile()'s filename: str, bytes or
   os.PathLike.  Returns a str. */
static JSValue qp_bi_fs_decode(JSContext *ctx, JSValueConst v)
{
    JSValue path = JS_UNDEFINED, r;

    if (qp_str_check(ctx, v))
        return qp_str_value(ctx, v);
    if (!qp_bytes_check(ctx, v)) {
        JSValue func = qp_lookup_special(ctx, v, QP_ATOM(ctx, __fspath__));
        if (QP_IS_NULL(func))
            return qp_raise_type_error(ctx, "expected str, bytes or os.PathLike object, not %T", v);
        path = qp_call0(ctx, func);
        JS_FreeValue(ctx, func);
        if (JS_IsException(path))
            return JS_EXCEPTION;
        if (!qp_str_check(ctx, path) && !qp_bytes_check(ctx, path)) {
            JSValue tn = qp_bi_tp_name(ctx, v), tp = qp_bi_tp_name(ctx, path);
            if (!JS_IsException(tn) && !JS_IsException(tp))
                qp_raise_type_error(ctx, "expected %.200U.__fspath__() to return str or bytes, "
                                    "not %.200U", tn, tp);
            JS_FreeValue(ctx, tn);
            JS_FreeValue(ctx, tp);
            JS_FreeValue(ctx, path);
            return JS_EXCEPTION;
        }
        if (qp_str_check(ctx, path)) {
            r = qp_str_value(ctx, path);
            JS_FreeValue(ctx, path);
            return r;
        }
        v = path;
    }
    {
        Py_ssize_t n;
        const uint8_t *data = qp_bytes_data(v, &n);
        if (memchr(data, 0, n)) {
            JS_FreeValue(ctx, path);
            return qp_raise_value_error(ctx, "embedded null byte");
        }
        r = qp_str_decode(ctx, (const char *)data, n, "utf-8", "surrogateescape");
    }
    JS_FreeValue(ctx, path);
    return r;
}

static JSValue qp_bi_compile(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "source", "filename", "mode", "flags", "dont_inherit",
                                      "optimize", "module", "_feature_version", NULL };
    static const QPArgSpec spec = { "compile", kw, 3, 6, 0 };
    JSValueConst a[8];
    JSValue filename = JS_UNDEFINED, mode_s = JS_UNDEFINED, result = JS_EXCEPTION;
    const char *fname_utf8 = NULL, *mode = NULL;
    size_t mode_len;
    int flags = 0, dont_inherit = 0, optimize = -1, feature_version = -1, compile_mode;
    char *src = NULL;
    size_t srclen;
    int cf_flags;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    filename = qp_bi_fs_decode(ctx, a[1]);
    if (JS_IsException(filename))
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, a[2])) {
        qp_bi_bad_argument(ctx, "compile", "argument 'mode'", "str", a[2]);
        goto done;
    }
    mode_s = qp_str_value(ctx, a[2]);
    if (JS_IsException(mode_s))
        goto done;
    mode = qp_str_utf8(ctx, mode_s, &mode_len);
    if (!mode)
        goto done;
    if (strlen(mode) != mode_len) {
        qp_raise_value_error(ctx, "embedded null character");
        goto done;
    }
    if (!QP_IS_NULL(a[3]) && qp_arg_int(ctx, a[3], &flags) < 0)
        goto done;
    if (!QP_IS_NULL(a[4]) && qp_arg_bool(ctx, a[4], &dont_inherit) < 0)
        goto done;
    if (!QP_IS_NULL(a[5]) && qp_arg_int(ctx, a[5], &optimize) < 0)
        goto done;
    if (!QP_IS_NULL(a[7]) && qp_arg_int(ctx, a[7], &feature_version) < 0)
        goto done;
    (void)feature_version; /* only meaningful with PyCF_ONLY_AST */

    cf_flags = flags;
    if (flags & ~(QP_BI_PyCF_MASK | QP_BI_PyCF_MASK_OBSOLETE | QP_BI_PyCF_COMPILE_MASK)) {
        qp_raise_value_error(ctx, "compile(): unrecognised flags");
        goto done;
    }
    if (optimize < -1 || optimize > 2) {
        qp_raise_value_error(ctx, "compile(): invalid optimize value");
        goto done;
    }
    if (!QP_IS_NULL(a[6]) && !qp_bi_is_none(a[6]) && !qp_str_check(ctx, a[6])) {
        qp_raise_type_error(ctx, "compile() argument 'module' must be str or None, not %T", a[6]);
        goto done;
    }
    if (!dont_inherit)
        cf_flags = qp_bi_merge_flags(ctx, cf_flags);

    if (strcmp(mode, "exec") == 0) {
        compile_mode = QP_INPUT_FILE;
    } else if (strcmp(mode, "eval") == 0) {
        compile_mode = QP_INPUT_EVAL;
    } else if (strcmp(mode, "single") == 0) {
        compile_mode = QP_INPUT_SINGLE;
    } else if (strcmp(mode, "func_type") == 0) {
        if (!(flags & QP_BI_PyCF_ONLY_AST)) {
            qp_raise_value_error(ctx, "compile() mode 'func_type' requires flag PyCF_ONLY_AST");
            goto done;
        }
        compile_mode = QP_INPUT_FUNC_TYPE;
    } else {
        if (flags & QP_BI_PyCF_ONLY_AST)
            qp_raise_value_error(ctx, "compile() mode must be 'exec', 'eval', 'single' or 'func_type'");
        else
            qp_raise_value_error(ctx, "compile() mode must be 'exec', 'eval' or 'single'");
        goto done;
    }

    /* AST objects (PyAST_Check) only exist once the _ast module is
       available (M3); without it every non-text source is rejected here
       exactly like a non-AST object. */
    if (qp_bi_source_as_string(ctx, a[0], "compile", "string, bytes or AST", &cf_flags,
                               &src, &srclen) < 0)
        goto done;
    if (flags & QP_BI_PyCF_ONLY_AST) {
        /* PyAST_mod2obj needs the _ast module state: import it so a
           missing module raises like CPython would without it. */
        JSValue m = qp_import_module(ctx, "_ast");
        if (JS_IsException(m))
            goto done;
        JS_FreeValue(ctx, m);
        qp_raise_system_error(ctx, "compile(): PyCF_ONLY_AST requires the _ast object converter");
        goto done;
    }
    fname_utf8 = qp_str_utf8(ctx, filename, NULL);
    if (!fname_utf8)
        goto done;
    result = qp_bi_compile_text(ctx, src, srclen, fname_utf8, compile_mode,
                                cf_flags & ~QP_BI_PyCF_ONLY_AST, optimize);
done:
    if (src)
        js_free(ctx, src);
    if (fname_utf8)
        JS_FreeCString(ctx, fname_utf8);
    if (mode)
        JS_FreeCString(ctx, mode);
    JS_FreeValue(ctx, mode_s);
    JS_FreeValue(ctx, filename);
    return result;
}

static inline BOOL qp_bi_code_check(JSValueConst v)
{
    return qp_bi_has_class(v, JS_CLASS_PY_CODE);
}

/* Number of free variables of a code object (PyCode_GetNumFree). */
static int qp_bi_code_nfree(JSValueConst code)
{
    struct JSFunctionBytecode *b = qp_code_bytecode(code);
    return b ? b->closure_var_count : 0;
}

/* Compile `source` text (str/bytes/buffer) for eval/exec and run it. */
static JSValue qp_bi_run_source(JSContext *ctx, JSValueConst source, const char *funcname,
                                const char *what, int mode, JSValueConst globals,
                                JSValueConst locals)
{
    int flags = 0;
    char *src;
    size_t len;
    const char *p;
    JSValue code, r;

    if (qp_bi_source_as_string(ctx, source, funcname, what, &flags, &src, &len) < 0)
        return JS_EXCEPTION;
    p = src;
    if (mode == QP_INPUT_EVAL) {
        while (*p == ' ' || *p == '\t')
            p++;
    }
    flags = qp_bi_merge_flags(ctx, flags);
    code = qp_bi_compile_text(ctx, p, len - (size_t)(p - src), "<string>", mode, flags, -1);
    js_free(ctx, src);
    if (JS_IsException(code))
        return JS_EXCEPTION;
    r = qp_eval_code(ctx, code, globals, locals);
    JS_FreeValue(ctx, code);
    return r;
}

static JSValue qp_bi_eval(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "", "globals", "locals", NULL };
    static const QPArgSpec spec = { "eval", kw, 1, 3, 0 };
    JSValueConst a[3];
    JSValueConst source;
    JSValue globals, locals, result;
    JSStackFrame *sf = NULL;
    int fromframe = 0;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    source = a[0];
    if (!QP_IS_NULL(a[2]) && !qp_bi_is_none(a[2]) && !qp_bi_mapping_check(ctx, a[2]))
        return qp_raise_type_error(ctx, "locals must be a mapping");
    if (!QP_IS_NULL(a[1]) && !qp_bi_is_none(a[1]) && !qp_anydict_check(ctx, a[1])) {
        return qp_raise_type_error(ctx, qp_bi_mapping_check(ctx, a[1])
                                   ? "globals must be a real dict or a frozendict; "
                                     "try eval(expr, {}, mapping)"
                                   : "globals must be a dict or a frozendict");
    }
    if (!QP_IS_NULL(a[1]) && !qp_bi_is_none(a[1])) {
        globals = JS_DupValue(ctx, a[1]);
    } else {
        sf = qp_bi_caller_frame(ctx);
        globals = sf ? qp_frame_globals(sf) : QP_NULL;
        if (QP_IS_NULL(globals))
            return qp_raise_type_error(ctx, "eval must be given globals and locals "
                                       "when called without a frame");
        fromframe = 1;
        globals = JS_DupValue(ctx, globals);
    }
    if (!QP_IS_NULL(a[2]) && !qp_bi_is_none(a[2])) {
        locals = JS_DupValue(ctx, a[2]);
    } else if (fromframe) {
        locals = qp_frame_locals(ctx, sf);
        if (JS_IsException(locals)) {
            JS_FreeValue(ctx, globals);
            return JS_EXCEPTION;
        }
    } else {
        locals = JS_DupValue(ctx, globals);
    }

    if (qp_bi_code_check(source)) {
        if (qp_bi_code_nfree(source) > 0) {
            qp_raise_type_error(ctx, "code object passed to eval() may not contain free variables");
            result = JS_EXCEPTION;
        } else {
            result = qp_eval_code(ctx, source, globals, locals);
        }
    } else {
        result = qp_bi_run_source(ctx, source, "eval", "string, bytes or code", QP_INPUT_EVAL,
                                  globals, locals);
    }
    JS_FreeValue(ctx, globals);
    JS_FreeValue(ctx, locals);
    return result;
}

static JSValue qp_bi_exec(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "", "globals", "locals", "closure", NULL };
    static const QPArgSpec spec = { "exec", kw, 1, 3, 0 };
    JSValueConst a[4];
    JSValueConst source, closure;
    JSValue globals = JS_UNDEFINED, locals = JS_UNDEFINED, v = JS_EXCEPTION;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    source = a[0];
    if (QP_IS_NULL(a[1]) || qp_bi_is_none(a[1])) {
        JSStackFrame *sf = qp_bi_caller_frame(ctx);
        JSValue g = sf ? qp_frame_globals(sf) : QP_NULL;
        if (QP_IS_NULL(g))
            return qp_raise_system_error(ctx, "globals and locals cannot be NULL");
        globals = JS_DupValue(ctx, g);
        if (QP_IS_NULL(a[2]) || qp_bi_is_none(a[2])) {
            locals = qp_frame_locals(ctx, sf);
            if (JS_IsException(locals)) {
                JS_FreeValue(ctx, globals);
                return JS_EXCEPTION;
            }
        } else {
            locals = JS_DupValue(ctx, a[2]);
        }
    } else {
        globals = JS_DupValue(ctx, a[1]);
        if (QP_IS_NULL(a[2]) || qp_bi_is_none(a[2]))
            locals = JS_DupValue(ctx, globals);
        else
            locals = JS_DupValue(ctx, a[2]);
    }

    if (!qp_anydict_check(ctx, globals)) {
        qp_raise_type_error(ctx, "exec() globals must be a dict or a frozendict, not %T", globals);
        goto error;
    }
    if (!qp_bi_mapping_check(ctx, locals)) {
        qp_bi_raise_tp(ctx, QP_EXC(ctx, TypeError),
                       "locals must be a mapping or None, not %.100U", locals);
        goto error;
    }
    closure = a[3];
    if (qp_bi_is_none(closure))
        closure = QP_NULL;

    if (qp_bi_code_check(source)) {
        int num_free = qp_bi_code_nfree(source);
        if (num_free == 0) {
            if (!QP_IS_NULL(closure)) {
                qp_raise_type_error(ctx, "cannot use a closure with this code object");
                goto error;
            }
        } else {
            BOOL closure_is_ok = !QP_IS_NULL(closure) && qp_tuple_check_exact(ctx, closure) &&
                QP_TUPLE_SIZE(closure) == num_free;
            if (closure_is_ok) {
                int i;
                for (i = 0; i < num_free; i++) {
                    if (!qp_bi_has_class(QP_TUPLE_ITEMS(closure)[i], JS_CLASS_PY_CELL)) {
                        closure_is_ok = FALSE;
                        break;
                    }
                }
            }
            if (!closure_is_ok) {
                qp_raise_type_error(ctx, "code object requires a closure of exactly length %d",
                                    num_free);
                goto error;
            }
        }
        if (QP_IS_NULL(closure))
            v = qp_eval_code(ctx, source, globals, locals);
        else
            v = qp_eval_code_closure(ctx, source, globals, locals, closure);
    } else {
        if (!QP_IS_NULL(closure)) {
            qp_raise_type_error(ctx, "closure can only be used when source is a code object");
            goto error;
        }
        v = qp_bi_run_source(ctx, source, "exec", "string, bytes or code", QP_INPUT_FILE,
                             globals, locals);
    }
    if (JS_IsException(v))
        goto error;
    JS_FreeValue(ctx, v);
    v = JS_UNDEFINED;
error:
    JS_FreeValue(ctx, globals);
    JS_FreeValue(ctx, locals);
    return v;
}

/* ========================================================================= */
/* Attributes, identity, misc                                                 */
/* ========================================================================= */

static JSValue qp_bi_format(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    if (qp_arg_check_positional(ctx, "format", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    if (nargs < 2)
        return qp_format(ctx, args[0], QP_NULL);
    if (!qp_str_check(ctx, args[1]))
        return qp_bi_bad_argument(ctx, "format", "argument 2", "str", args[1]);
    return qp_format(ctx, args[0], args[1]);
}

static JSValue qp_bi_getattr(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSAtom name;
    JSValue result;

    if (qp_arg_check_positional(ctx, "getattr", nargs, 2, 3) < 0)
        return JS_EXCEPTION;
    name = qp_bi_attr_name(ctx, args[1]);
    if (name == JS_ATOM_NULL)
        return JS_EXCEPTION;
    if (nargs > 2) {
        int r = qp_getattr_opt(ctx, args[0], name, &result);
        JS_FreeAtom(ctx, name);
        if (r < 0)
            return JS_EXCEPTION;
        if (r == 0)
            return JS_DupValue(ctx, args[2]);
        return result;
    }
    result = qp_getattr(ctx, args[0], name);
    JS_FreeAtom(ctx, name);
    return result;
}

static JSValue qp_bi_globals(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSStackFrame *sf = qp_bi_caller_frame(ctx);
    JSValue g = sf ? qp_frame_globals(sf) : QP_NULL;
    if (QP_IS_NULL(g))
        return JS_UNDEFINED;
    return JS_DupValue(ctx, g);
}

static JSValue qp_bi_hasattr(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSAtom name;
    JSValue v;
    int r;

    if (qp_arg_check_positional(ctx, "hasattr", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    name = qp_bi_attr_name(ctx, args[1]);
    if (name == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_getattr_opt(ctx, args[0], name, &v);
    JS_FreeAtom(ctx, name);
    if (r < 0)
        return JS_EXCEPTION;
    JS_FreeValue(ctx, v);
    return qp_bool_new(r > 0);
}

static JSValue qp_bi_hash(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          int nargs, JSValueConst kwnames, int magic)
{
    Py_hash_t h = qp_hash(ctx, args[0]);
    if (h == -1)
        return JS_EXCEPTION;
    return qp_long_from_int64(ctx, h);
}

/* id(): the address of heap values; immediate values (small ints, floats,
   bools, None) map to a stable odd number derived from tag and payload so
   that equal immediates share an id, like CPython's cached singletons. */
static JSValue qp_bi_id(JSContext *ctx, JSValueConst self, JSValueConst *args,
                        int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst v = args[0];
    uint64_t id;

    if (JS_VALUE_HAS_REF_COUNT(v)) {
        id = (uint64_t)(uintptr_t)JS_VALUE_GET_PTR(v);
    } else {
        int tag = JS_VALUE_GET_TAG(v);
        uint64_t payload;
        if (tag == JS_TAG_FLOAT64) {
            double d = JS_VALUE_GET_FLOAT64(v);
            memcpy(&payload, &d, sizeof(payload));
        } else if (tag == JS_TAG_NULL) {
            tag = JS_TAG_UNDEFINED; /* JS null is None */
            payload = 0;
        } else {
            payload = (uint64_t)(uint32_t)JS_VALUE_GET_INT(v);
#ifdef JS_TAG_SHORT_BIG_INT
            if (tag == JS_TAG_SHORT_BIG_INT)
                payload = (uint64_t)JS_VALUE_GET_SHORT_BIG_INT(v);
#endif
        }
        id = ((payload * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)(tag & 0xff) << 56)) | 1;
        id &= 0x7fffffffffffffffULL;
    }
    return qp_long_from_uint64(ctx, id);
}

static JSValue qp_bi_isinstance(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    int r;
    if (qp_arg_check_positional(ctx, magic ? "issubclass" : "isinstance", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    r = magic ? qp_issubclass(ctx, args[0], args[1]) : qp_isinstance(ctx, args[0], args[1]);
    if (r < 0)
        return JS_EXCEPTION;
    return qp_bool_new(r);
}

static JSValue qp_bi_iter(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          int nargs, JSValueConst kwnames, int magic)
{
    if (qp_arg_check_positional(ctx, "iter", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    if (nargs == 1)
        return qp_iter(ctx, args[0]);
    if (!qp_bi_callable(ctx, args[0]))
        return qp_raise_type_error(ctx, "iter(v, w): v must be callable");
    return qp_calliter_new(ctx, args[0], args[1]);
}

/* PyObject_GetAIter */
static JSValue qp_bi_aiter(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    QPType *t = qp_type_of(ctx, args[0]);
    JSValue it;
    if (!t->slots.am.am_aiter)
        return qp_bi_raise_tp(ctx, QP_EXC(ctx, TypeError),
                              "'%.200U' object is not an async iterable", args[0]);
    it = t->slots.am.am_aiter(ctx, args[0]);
    if (JS_IsException(it))
        return JS_EXCEPTION;
    if (!qp_type_of(ctx, it)->slots.am.am_anext) {
        qp_bi_raise_tp(ctx, QP_EXC(ctx, TypeError),
                       "aiter() returned not an async iterator of type '%.100U'", it);
        JS_FreeValue(ctx, it);
        return JS_EXCEPTION;
    }
    return it;
}

static JSValue qp_bi_anext_awaitable_new(JSContext *ctx, JSValueConst awaitable,
                                         JSValueConst default_value);

static JSValue qp_bi_anext(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    QPType *t;
    JSValue awaitable, r;

    if (qp_arg_check_positional(ctx, "anext", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    t = qp_type_of(ctx, args[0]);
    if (!t->slots.am.am_anext)
        return qp_bi_raise_tp(ctx, QP_EXC(ctx, TypeError),
                              "'%.200U' object is not an async iterator", args[0]);
    awaitable = t->slots.am.am_anext(ctx, args[0]);
    if (JS_IsException(awaitable) || nargs < 2)
        return awaitable;
    r = qp_bi_anext_awaitable_new(ctx, awaitable, args[1]);
    JS_FreeValue(ctx, awaitable);
    return r;
}

static JSValue qp_bi_len(JSContext *ctx, JSValueConst self, JSValueConst *args,
                         int nargs, JSValueConst kwnames, int magic)
{
    Py_ssize_t n = qp_len(ctx, args[0]);
    if (n < 0)
        return JS_EXCEPTION;
    return qp_long_from_int64(ctx, n);
}

static JSValue qp_bi_locals(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    JSStackFrame *sf = qp_bi_caller_frame(ctx);
    if (!sf)
        return JS_UNDEFINED;
    return qp_frame_locals(ctx, sf);
}

/* min() (magic QP_CMP_LT) and max() (magic QP_CMP_GT) */
static JSValue qp_bi_min_max(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int op)
{
    static const char *const kw[] = { "key", "default", NULL };
    static const QPArgSpec spec_min = { "min", kw, 0, 0, 0 };
    static const QPArgSpec spec_max = { "max", kw, 0, 0, 0 };
    const char *name = (op == QP_CMP_LT) ? "min" : "max";
    JSValueConst kwv[2] = { QP_NULL, QP_NULL };
    JSValueConst keyfunc, defaultval;
    JSValue it = QP_NULL, item, val, maxitem = QP_NULL, maxval = QP_NULL;
    int positional, i = 0;

    if (nargs == 0)
        return qp_raise_type_error(ctx, "%s expected at least 1 argument, got 0", name);
    if (qp_bi_nkw(kwnames) > 0 &&
        qp_arg_parse(ctx, op == QP_CMP_LT ? &spec_min : &spec_max, args + nargs, 0, kwnames, kwv) < 0)
        return JS_EXCEPTION;
    keyfunc = kwv[0];
    defaultval = kwv[1];
    positional = nargs > 1; /* False iff nargs == 1 */
    if (positional && !QP_IS_NULL(defaultval))
        return qp_raise_type_error(ctx, "Cannot specify a default for %s() with multiple "
                                   "positional arguments", name);
    if (!positional) {
        it = qp_iter(ctx, args[0]);
        if (JS_IsException(it))
            return JS_EXCEPTION;
    }
    if (!QP_IS_NULL(keyfunc) && qp_bi_is_none(keyfunc))
        keyfunc = QP_NULL;

    for (;;) {
        if (QP_IS_NULL(it)) {
            if (i >= nargs)
                break;
            item = JS_DupValue(ctx, args[i++]);
        } else {
            item = qp_next(ctx, it);
            if (JS_IsException(item))
                goto fail;
            if (QP_IS_NULL(item))
                break;
        }
        /* get the value from the key function */
        if (!QP_IS_NULL(keyfunc)) {
            val = qp_call1(ctx, keyfunc, item);
            if (JS_IsException(val)) {
                JS_FreeValue(ctx, item);
                goto fail;
            }
        } else {
            val = JS_DupValue(ctx, item);
        }
        if (QP_IS_NULL(maxval)) {
            maxitem = item;
            maxval = val;
        } else {
            int cmp = qp_richcompare_bool(ctx, val, maxval, op);
            if (cmp < 0) {
                JS_FreeValue(ctx, val);
                JS_FreeValue(ctx, item);
                goto fail;
            } else if (cmp > 0) {
                JS_FreeValue(ctx, maxval);
                JS_FreeValue(ctx, maxitem);
                maxval = val;
                maxitem = item;
            } else {
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, val);
            }
        }
    }
    if (QP_IS_NULL(maxval)) {
        if (!QP_IS_NULL(defaultval)) {
            maxitem = JS_DupValue(ctx, defaultval);
        } else {
            qp_raise_value_error(ctx, "%s() iterable argument is empty", name);
            maxitem = JS_EXCEPTION;
        }
    } else {
        JS_FreeValue(ctx, maxval);
    }
    JS_FreeValue(ctx, it);
    return maxitem;

fail:
    JS_FreeValue(ctx, maxval);
    JS_FreeValue(ctx, maxitem);
    JS_FreeValue(ctx, it);
    return JS_EXCEPTION;
}

static JSValue qp_bi_next(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          int nargs, JSValueConst kwnames, int magic)
{
    QPType *t;
    JSValue res;

    if (qp_arg_check_positional(ctx, "next", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    t = qp_type_of(ctx, args[0]);
    if (!t->slots.tp_iternext)
        return qp_bi_raise_tp(ctx, QP_EXC(ctx, TypeError),
                              "'%.200U' object is not an iterator", args[0]);
    res = t->slots.tp_iternext(ctx, args[0]);
    if (!JS_IsException(res) && !QP_IS_NULL(res))
        return res;
    if (nargs > 1) {
        if (JS_IsException(res)) {
            if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, StopIteration)))
                return JS_EXCEPTION;
            qp_bi_clear_exc(ctx);
        }
        return JS_DupValue(ctx, args[1]);
    }
    if (JS_IsException(res))
        return JS_EXCEPTION;
    return qp_raise_value(ctx, QP_EXC(ctx, StopIteration), JS_UNDEFINED);
}

static JSValue qp_bi_ord(JSContext *ctx, JSValueConst self, JSValueConst *args,
                         int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst c = args[0];
    Py_ssize_t size;

    if (qp_bytes_check(ctx, c) || qp_bytearray_check(ctx, c)) {
        const uint8_t *data = qp_bytes_data(c, &size);
        if (size == 1)
            return JS_NewInt32(ctx, data[0]);
    } else if (qp_str_check(ctx, c)) {
        JSValue s = qp_str_value(ctx, c);
        qp_strview view;
        uint32_t cp = 0;
        if (JS_IsException(s))
            return JS_EXCEPTION;
        if (qp_str_view_acquire(ctx, s, &view) < 0) {
            JS_FreeValue(ctx, s);
            return JS_EXCEPTION;
        }
        size = view.len;
        if (size == 1) {
            if (view.kind == 1)
                cp = ((const uint8_t *)view.data)[0];
            else if (view.kind == 2)
                cp = ((const uint16_t *)view.data)[0];
            else
                cp = ((const uint32_t *)view.data)[0];
        }
        qp_str_view_release(ctx, &view);
        JS_FreeValue(ctx, s);
        if (size == 1)
            return JS_NewInt32(ctx, (int32_t)cp);
    } else {
        return qp_bi_raise_tp(ctx, QP_EXC(ctx, TypeError),
                              "ord() expected string of length 1, but %.200U found", c);
    }
    return qp_raise_type_error(ctx, "ord() expected a character, but string of length %zd found",
                               size);
}

static JSValue qp_bi_pow(JSContext *ctx, JSValueConst self, JSValueConst *args,
                         int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "base", "exp", "mod", NULL };
    static const QPArgSpec spec = { "pow", kw, 2, 3, 0 };
    JSValueConst a[3];
    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    return qp_power(ctx, a[0], a[1], QP_IS_NULL(a[2]) ? JS_UNDEFINED : a[2]);
}

static JSValue qp_bi_print(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "sep", "end", "file", "flush", NULL };
    static const QPArgSpec spec = { "print", kw, 0, 0, 1 };
    JSValueConst a[4];
    JSValueConst sep, end;
    JSValue file;
    int i, flush = 0, err;

    if (qp_bi_nkw(kwnames) > 0) {
        if (qp_arg_parse(ctx, &spec, args + nargs, 0, kwnames, a) < 0)
            return JS_EXCEPTION;
    } else {
        a[0] = a[1] = a[2] = a[3] = QP_NULL;
    }
    if (!QP_IS_NULL(a[3]) && qp_arg_bool(ctx, a[3], &flush) < 0)
        return JS_EXCEPTION;

    if (QP_IS_NULL(a[2]) || qp_bi_is_none(a[2])) {
        file = qp_bi_sys_attr(ctx, QP_ATOM(ctx, stdout));
        if (JS_IsException(file))
            return JS_EXCEPTION;
        /* sys.stdout may be None when FILE* stdout isn't connected */
        if (qp_bi_is_none(file))
            return JS_UNDEFINED;
    } else {
        file = JS_DupValue(ctx, a[2]);
    }

    sep = a[0];
    if (!QP_IS_NULL(sep) && qp_bi_is_none(sep)) {
        sep = QP_NULL;
    } else if (!QP_IS_NULL(sep) && !qp_str_check(ctx, sep)) {
        qp_bi_raise_tp(ctx, QP_EXC(ctx, TypeError), "sep must be None or a string, not %.200U", sep);
        JS_FreeValue(ctx, file);
        return JS_EXCEPTION;
    }
    end = a[1];
    if (!QP_IS_NULL(end) && qp_bi_is_none(end)) {
        end = QP_NULL;
    } else if (!QP_IS_NULL(end) && !qp_str_check(ctx, end)) {
        qp_bi_raise_tp(ctx, QP_EXC(ctx, TypeError), "end must be None or a string, not %.200U", end);
        JS_FreeValue(ctx, file);
        return JS_EXCEPTION;
    }

    for (i = 0; i < nargs; i++) {
        if (i > 0) {
            if (QP_IS_NULL(sep))
                err = qp_bi_file_write_cstr(ctx, " ", file);
            else
                err = qp_bi_file_write(ctx, sep, file);
            if (err)
                goto fail;
        }
        if (qp_bi_file_write(ctx, args[i], file))
            goto fail;
    }
    if (QP_IS_NULL(end))
        err = qp_bi_file_write_cstr(ctx, "\n", file);
    else
        err = qp_bi_file_write(ctx, end, file);
    if (err)
        goto fail;
    if (flush && qp_bi_file_flush(ctx, file) < 0)
        goto fail;
    JS_FreeValue(ctx, file);
    return JS_UNDEFINED;
fail:
    JS_FreeValue(ctx, file);
    return JS_EXCEPTION;
}

/* PyFile_GetLine(f, -1) */
static JSValue qp_bi_file_getline(JSContext *ctx, JSValueConst f)
{
    JSValue result = qp_call_method(ctx, f, QP_ATOM(ctx, readline), NULL, 0, QP_NULL);
    if (JS_IsException(result))
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, result) && !qp_bytes_check(ctx, result)) {
        JS_FreeValue(ctx, result);
        return qp_raise_type_error(ctx, "object.readline() returned non-string");
    }
    if (qp_bytes_check(ctx, result)) {
        Py_ssize_t len;
        const uint8_t *s = qp_bytes_data(result, &len);
        if (len == 0) {
            JS_FreeValue(ctx, result);
            return qp_raise(ctx, QP_EXC(ctx, EOFError), "EOF when reading a line");
        }
        if (s[len - 1] == '\n') {
            JSValue v = qp_bytes_new(ctx, s, len - 1);
            JS_FreeValue(ctx, result);
            return v;
        }
    } else {
        Py_ssize_t len = qp_str_len(ctx, result);
        if (len < 0) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
        if (len == 0) {
            JS_FreeValue(ctx, result);
            return qp_raise(ctx, QP_EXC(ctx, EOFError), "EOF when reading a line");
        } else {
            JSValue s = qp_str_value(ctx, result), last, v;
            JS_FreeValue(ctx, result);
            if (JS_IsException(s))
                return JS_EXCEPTION;
            last = qp_str_substring(ctx, s, len - 1, len);
            if (JS_IsException(last)) {
                JS_FreeValue(ctx, s);
                return JS_EXCEPTION;
            }
            if (JS_VALUE_GET_TAG(last) == JS_TAG_STRING &&
                JS_VALUE_GET_STRING(last)->len == 1 && string_get(JS_VALUE_GET_STRING(last), 0) == '\n') {
                v = qp_str_substring(ctx, s, 0, len - 1);
                JS_FreeValue(ctx, s);
                JS_FreeValue(ctx, last);
                return v;
            }
            JS_FreeValue(ctx, last);
            return s;
        }
    }
    return result;
}

/* input([prompt]): the non-interactive path of CPython (prompt written to
   sys.stdout, line read with sys.stdin.readline()); line editing through
   GNU readline is not available in this runtime. */
static JSValue qp_bi_input(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    JSValue fin = JS_UNDEFINED, fout = JS_UNDEFINED, ferr = JS_UNDEFINED, r = JS_EXCEPTION;

    if (qp_arg_check_positional(ctx, "input", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    /* Check that stdin/out/err are intact */
    fin = qp_bi_sys_attr(ctx, QP_ATOM(ctx, stdin));
    if (JS_IsException(fin)) {
        fin = JS_UNDEFINED;
        goto done;
    }
    if (qp_bi_is_none(fin)) {
        qp_raise_runtime_error(ctx, "lost sys.stdin");
        goto done;
    }
    fout = qp_bi_sys_attr(ctx, QP_ATOM(ctx, stdout));
    if (JS_IsException(fout)) {
        fout = JS_UNDEFINED;
        goto done;
    }
    if (qp_bi_is_none(fout)) {
        qp_raise_runtime_error(ctx, "lost sys.stdout");
        goto done;
    }
    ferr = qp_bi_sys_attr(ctx, QP_ATOM(ctx, stderr));
    if (JS_IsException(ferr)) {
        ferr = JS_UNDEFINED;
        goto done;
    }
    if (qp_bi_is_none(ferr)) {
        qp_raise_runtime_error(ctx, "lost sys.stderr");
        goto done;
    }
    /* First of all, flush stderr */
    if (qp_bi_file_flush(ctx, ferr) < 0)
        qp_bi_clear_exc(ctx);
    if (nargs > 0) {
        if (qp_bi_file_write(ctx, args[0], fout) != 0)
            goto done;
    }
    if (qp_bi_file_flush(ctx, fout) < 0)
        qp_bi_clear_exc(ctx);
    r = qp_bi_file_getline(ctx, fin);
done:
    JS_FreeValue(ctx, fin);
    JS_FreeValue(ctx, fout);
    JS_FreeValue(ctx, ferr);
    return r;
}

static JSValue qp_bi_repr(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          int nargs, JSValueConst kwnames, int magic)
{
    return qp_repr(ctx, args[0]);
}

static JSValue qp_bi_round(JSContext *ctx, JSValueConst self, JSValueConst *args,
                           int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "number", "ndigits", NULL };
    static const QPArgSpec spec = { "round", kw, 1, 2, 0 };
    JSValueConst a[2];
    JSValue meth, result;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    meth = qp_lookup_special(ctx, a[0], QP_ATOM(ctx, __round__));
    if (QP_IS_NULL(meth)) {
        JSValue tn = qp_bi_tp_name(ctx, a[0]);
        if (JS_IsException(tn))
            return JS_EXCEPTION;
        qp_raise_type_error(ctx, "type %.100U doesn't define __round__ method", tn);
        JS_FreeValue(ctx, tn);
        return JS_EXCEPTION;
    }
    if (JS_IsException(meth))
        return JS_EXCEPTION;
    if (QP_IS_NULL(a[1]) || qp_bi_is_none(a[1]))
        result = qp_call0(ctx, meth);
    else
        result = qp_call1(ctx, meth, a[1]);
    JS_FreeValue(ctx, meth);
    return result;
}

static JSValue qp_bi_setattr(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSAtom name;
    int r;
    if (qp_arg_check_positional(ctx, "setattr", nargs, 3, 3) < 0)
        return JS_EXCEPTION;
    name = qp_bi_attr_name(ctx, args[1]);
    if (name == JS_ATOM_NULL)
        return JS_EXCEPTION;
    r = qp_setattr(ctx, args[0], name, args[2]);
    JS_FreeAtom(ctx, name);
    return r < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

static JSValue qp_bi_sorted(JSContext *ctx, JSValueConst self, JSValueConst *args,
                            int nargs, JSValueConst kwnames, int magic)
{
    JSValue newlist, v;

    /* Keyword arguments are passed through list.sort() which will check
       them. */
    if (qp_arg_check_positional(ctx, "sorted", nargs, 1, 1) < 0)
        return JS_EXCEPTION;
    newlist = qp_list_from_iterable(ctx, args[0]);
    if (JS_IsException(newlist))
        return JS_EXCEPTION;
    v = qp_call_method(ctx, newlist, QP_ATOM(ctx, sort), args + 1, nargs - 1, kwnames);
    if (JS_IsException(v)) {
        JS_FreeValue(ctx, newlist);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, v);
    return newlist;
}

/* ---- sum: Neumaier compensated summation for floats ---------------------- */

typedef struct {
    double hi;     /* high-order bits for a running sum */
    double lo;     /* a running compensation for lost low-order bits */
} QPCompensatedSum;

static inline QPCompensatedSum qp_cs_from_double(double x)
{
    QPCompensatedSum s = { x, 0.0 };
    return s;
}

static inline QPCompensatedSum qp_cs_add(QPCompensatedSum total, double x)
{
    double t = total.hi + x;
    if (fabs(total.hi) >= fabs(x))
        total.lo += (total.hi - t) + x;
    else
        total.lo += (x - t) + total.hi;
    total.hi = t;
    return total;
}

static inline double qp_cs_to_double(QPCompensatedSum total)
{
    /* Avoid losing the sign on a negative result, and don't let adding the
       compensation convert an infinite or overflowed sum to a NaN. */
    if (total.lo && isfinite(total.lo))
        return total.hi + total.lo;
    return total.hi;
}

static inline BOOL qp_bi_float_exact(JSValueConst v)
{
    return JS_VALUE_GET_TAG(v) == JS_TAG_FLOAT64;
}

static BOOL qp_bi_complex_exact(JSContext *ctx, JSValueConst v)
{
    return qp_bi_has_class(v, JS_CLASS_PY_COMPLEX) && qp_type_of(ctx, v) == QP_TYPE(ctx, complex);
}

/* PyLong_AsDouble of an int or int-subclass instance. */
static int qp_bi_long_as_double(JSContext *ctx, JSValueConst v, double *out)
{
    JSValue iv = qp_long_value(ctx, v);
    int r;
    if (JS_IsException(iv))
        return -1;
    r = qp_long_as_double(ctx, iv, out);
    JS_FreeValue(ctx, iv);
    return r;
}

/* PyNumber_Add(result, item), consuming both. */
static JSValue qp_bi_add_consume(JSContext *ctx, JSValue result, JSValue item)
{
    JSValue r = qp_binop(ctx, result, item, QP_NB_ADD);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, item);
    return r;
}

static JSValue qp_bi_sum(JSContext *ctx, JSValueConst self, JSValueConst *args,
                         int nargs, JSValueConst kwnames, int magic)
{
    static const char *const kw[] = { "", "start", NULL };
    static const QPArgSpec spec = { "sum", kw, 1, 2, 0 };
    JSValueConst a[2];
    JSValue result, iter, item;

    if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
        return JS_EXCEPTION;
    iter = qp_iter(ctx, a[0]);
    if (JS_IsException(iter))
        return JS_EXCEPTION;
    if (QP_IS_NULL(a[1])) {
        result = JS_NewInt32(ctx, 0);
    } else {
        /* reject string values for 'start' parameter */
        if (qp_str_check(ctx, a[1])) {
            JS_FreeValue(ctx, iter);
            return qp_raise_type_error(ctx, "sum() can't sum strings [use ''.join(seq) instead]");
        }
        if (qp_bytes_check(ctx, a[1])) {
            JS_FreeValue(ctx, iter);
            return qp_raise_type_error(ctx, "sum() can't sum bytes [use b''.join(seq) instead]");
        }
        if (qp_bytearray_check(ctx, a[1])) {
            JS_FreeValue(ctx, iter);
            return qp_raise_type_error(ctx, "sum() can't sum bytearray [use b''.join(seq) instead]");
        }
        result = JS_DupValue(ctx, a[1]);
    }

    /* Fast addition by keeping temporary sums in C instead of new Python
       objects.  Assumes all inputs are the same type.  If the assumption
       fails, default to the more general routine. */
    if (qp_long_check_exact(result)) {
        int64_t i_result;
        int overflow = 0;
        qp_long_as_int64_overflow(result, &i_result, &overflow);
        /* If this already overflowed, don't even enter the loop. */
        if (overflow == 0) {
            JS_FreeValue(ctx, result);
            result = QP_NULL;
        }
        while (QP_IS_NULL(result)) {
            item = qp_next(ctx, iter);
            if (JS_IsException(item)) {
                JS_FreeValue(ctx, iter);
                return JS_EXCEPTION;
            }
            if (QP_IS_NULL(item)) {
                JS_FreeValue(ctx, iter);
                return qp_long_from_int64(ctx, i_result);
            }
            if (qp_long_check_exact(item) || JS_VALUE_GET_TAG(item) == JS_TAG_BOOL) {
                int64_t b = 0;
                overflow = 0;
                if (JS_VALUE_GET_TAG(item) == JS_TAG_INT)
                    b = JS_VALUE_GET_INT(item);
                else if (JS_VALUE_GET_TAG(item) == JS_TAG_BOOL)
                    b = JS_VALUE_GET_BOOL(item);
                else
                    qp_long_as_int64_overflow(item, &b, &overflow);
                if (overflow == 0 &&
                    (i_result >= 0 ? (b <= INT64_MAX - i_result)
                                   : (b >= INT64_MIN - i_result))) {
                    i_result += b;
                    JS_FreeValue(ctx, item);
                    continue;
                }
            }
            /* Either overflowed or is not an int.  Restore real objects
               and process normally */
            result = qp_long_from_int64(ctx, i_result);
            if (JS_IsException(result)) {
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, iter);
                return JS_EXCEPTION;
            }
            result = qp_bi_add_consume(ctx, result, item);
            if (JS_IsException(result)) {
                JS_FreeValue(ctx, iter);
                return JS_EXCEPTION;
            }
        }
    }
    if (qp_bi_float_exact(result)) {
        QPCompensatedSum re_sum = qp_cs_from_double(JS_VALUE_GET_FLOAT64(result));
        JS_FreeValue(ctx, result);
        result = QP_NULL;
        while (QP_IS_NULL(result)) {
            item = qp_next(ctx, iter);
            if (JS_IsException(item)) {
                JS_FreeValue(ctx, iter);
                return JS_EXCEPTION;
            }
            if (QP_IS_NULL(item)) {
                JS_FreeValue(ctx, iter);
                return qp_float_new(ctx, qp_cs_to_double(re_sum));
            }
            if (qp_bi_float_exact(item)) {
                re_sum = qp_cs_add(re_sum, JS_VALUE_GET_FLOAT64(item));
                continue;
            }
            if (qp_long_check(ctx, item)) {
                double value;
                int r = qp_bi_long_as_double(ctx, item, &value);
                JS_FreeValue(ctx, item);
                if (r < 0) {
                    JS_FreeValue(ctx, iter);
                    return JS_EXCEPTION;
                }
                re_sum = qp_cs_add(re_sum, value);
                continue;
            }
            result = qp_float_new(ctx, qp_cs_to_double(re_sum));
            result = qp_bi_add_consume(ctx, result, item);
            if (JS_IsException(result)) {
                JS_FreeValue(ctx, iter);
                return JS_EXCEPTION;
            }
        }
    }
    if (qp_bi_complex_exact(ctx, result)) {
        QPComplex z;
        QPCompensatedSum re_sum, im_sum;
        if (qp_complex_as_c(ctx, result, &z) < 0) {
            JS_FreeValue(ctx, result);
            JS_FreeValue(ctx, iter);
            return JS_EXCEPTION;
        }
        re_sum = qp_cs_from_double(z.real);
        im_sum = qp_cs_from_double(z.imag);
        JS_FreeValue(ctx, result);
        result = QP_NULL;
        while (QP_IS_NULL(result)) {
            item = qp_next(ctx, iter);
            if (JS_IsException(item)) {
                JS_FreeValue(ctx, iter);
                return JS_EXCEPTION;
            }
            if (QP_IS_NULL(item)) {
                JS_FreeValue(ctx, iter);
                return qp_complex_new(ctx, qp_cs_to_double(re_sum), qp_cs_to_double(im_sum));
            }
            if (qp_bi_complex_exact(ctx, item)) {
                if (qp_complex_as_c(ctx, item, &z) < 0) {
                    JS_FreeValue(ctx, item);
                    JS_FreeValue(ctx, iter);
                    return JS_EXCEPTION;
                }
                re_sum = qp_cs_add(re_sum, z.real);
                im_sum = qp_cs_add(im_sum, z.imag);
                JS_FreeValue(ctx, item);
                continue;
            }
            if (qp_long_check(ctx, item)) {
                double value;
                int r = qp_bi_long_as_double(ctx, item, &value);
                JS_FreeValue(ctx, item);
                if (r < 0) {
                    JS_FreeValue(ctx, iter);
                    return JS_EXCEPTION;
                }
                re_sum = qp_cs_add(re_sum, value);
                continue;
            }
            if (qp_float_check(ctx, item)) {
                double value;
                int r = qp_float_as_double(ctx, item, &value);
                JS_FreeValue(ctx, item);
                if (r < 0) {
                    JS_FreeValue(ctx, iter);
                    return JS_EXCEPTION;
                }
                re_sum = qp_cs_add(re_sum, value);
                continue;
            }
            result = qp_complex_new(ctx, qp_cs_to_double(re_sum), qp_cs_to_double(im_sum));
            if (JS_IsException(result)) {
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, iter);
                return JS_EXCEPTION;
            }
            result = qp_bi_add_consume(ctx, result, item);
            if (JS_IsException(result)) {
                JS_FreeValue(ctx, iter);
                return JS_EXCEPTION;
            }
        }
    }
    for (;;) {
        item = qp_next(ctx, iter);
        if (JS_IsException(item)) {
            JS_FreeValue(ctx, result);
            result = JS_EXCEPTION;
            break;
        }
        if (QP_IS_NULL(item))
            break;
        /* PyNumber_Add, not in-place addition: sum([[x]], empty) must not
           change `empty` (bpo-18305). */
        result = qp_bi_add_consume(ctx, result, item);
        if (JS_IsException(result))
            break;
    }
    JS_FreeValue(ctx, iter);
    return result;
}

static JSValue qp_bi_vars(JSContext *ctx, JSValueConst self, JSValueConst *args,
                          int nargs, JSValueConst kwnames, int magic)
{
    JSValue d;
    int r;

    if (qp_arg_check_positional(ctx, "vars", nargs, 0, 1) < 0)
        return JS_EXCEPTION;
    if (nargs == 0) {
        JSStackFrame *sf = qp_bi_caller_frame(ctx);
        if (!sf)
            return qp_raise_system_error(ctx, "frame does not exist");
        return qp_frame_locals(ctx, sf);
    }
    r = qp_getattr_opt(ctx, args[0], QP_ATOM(ctx, __dict__), &d);
    if (r < 0)
        return JS_EXCEPTION;
    if (r == 0)
        return qp_raise_type_error(ctx, "vars() argument must have __dict__ attribute");
    return d;
}

/* ========================================================================= */
/* Iterator types: map, filter, zip, enumerate, reversed                      */
/* ========================================================================= */

static JSValue qp_bi_self_iter(JSContext *ctx, JSValueConst self)
{
    return JS_DupValue(ctx, self);
}

/* New instance of subtype with its QPInstance slots; NULL slots on
   failure (exception pending). */
static JSValue qp_bi_new_instance(JSContext *ctx, QPType *subtype, JSValue **pslots)
{
    JSValue obj = qp_object_new_instance(ctx, subtype, JS_CLASS_PY_OBJECT);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    *pslots = qp_instance_slots(obj);
    if (!*pslots) {
        JS_FreeValue(ctx, obj);
        return qp_raise_system_error(ctx, "%s instance has no field storage",
                                     subtype->tp_name ? subtype->tp_name : "object");
    }
    return obj;
}

/* Store v (consumed) in slot i. */
static inline void qp_bi_slot_set(JSContext *ctx, JSValue *slots, int i, JSValue v)
{
    JSValue old = slots[i];
    slots[i] = v;
    JS_FreeValue(ctx, old);
}

/* Slots of a builtin iterator instance, or NULL when the object was not
   initialized by its tp_new (then the iterator behaves as exhausted). */
static JSValue *qp_bi_slots(JSValueConst self, int n)
{
    JSValue *s = qp_instance_slots(self);
    if (!s || QP_IS_NULL(s[0]))
        return NULL;
    (void)n;
    return s;
}

/* Parse the keyword-only `strict` flag of zip()/map() ("|$p:name"). */
static int qp_bi_parse_strict(JSContext *ctx, const char *fname, JSValueConst *kwvals,
                              JSValueConst kwnames, int *strict)
{
    static const char *const kw[] = { "strict", NULL };
    QPArgSpec spec = { fname, kw, 0, 0, 0 };
    JSValueConst out[1];
    *strict = 0;
    if (qp_bi_nkw(kwnames) == 0)
        return 0;
    if (qp_arg_parse(ctx, &spec, kwvals, 0, kwnames, out) < 0)
        return -1;
    if (!QP_IS_NULL(out[0]))
        return qp_arg_bool(ctx, out[0], strict);
    return 0;
}

/* tuple of iter(args[i]) */
static JSValue qp_bi_iter_tuple(JSContext *ctx, JSValueConst *args, int n)
{
    JSValue *items;
    JSValue t;
    int i;
    items = js_malloc(ctx, sizeof(JSValue) * (n ? n : 1));
    if (!items)
        return JS_EXCEPTION;
    for (i = 0; i < n; i++) {
        items[i] = qp_iter(ctx, args[i]);
        if (JS_IsException(items[i])) {
            while (--i >= 0)
                JS_FreeValue(ctx, items[i]);
            js_free(ctx, items);
            return JS_EXCEPTION;
        }
    }
    t = qp_tuple_new_steal(ctx, n, items);
    js_free(ctx, items);
    return t;
}

/* Raise "<name>() argument %d is shorter/longer than argument(s) ..." */
static JSValue qp_bi_strict_error(JSContext *ctx, const char *name, const char *what, int i)
{
    const char *plural = i == 1 ? " " : "s 1-";
    return qp_raise_value_error(ctx, "%s() argument %d is %s than argument%s%d",
                                name, i + 1, what, plural, i);
}

/* Strict-mode tail of zip/map: iterator i (> 0 known) ran out first, or
   iterator 0 ran out; check that every later iterator is exhausted too. */
static JSValue qp_bi_strict_check(JSContext *ctx, const char *name, JSValueConst iters, int i)
{
    int n = (int)QP_TUPLE_SIZE(iters);
    if (i) {
        /* ValueError: zip() argument 2 is shorter than argument 1 */
        return qp_bi_strict_error(ctx, name, "shorter", i);
    }
    for (i = 1; i < n; i++) {
        JSValue val = qp_next(ctx, QP_TUPLE_ITEMS(iters)[i]);
        if (JS_IsException(val))
            return JS_EXCEPTION;
        if (!QP_IS_NULL(val)) {
            JS_FreeValue(ctx, val);
            return qp_bi_strict_error(ctx, name, "longer", i);
        }
        /* Argument i is exhausted.  So far so good... */
    }
    /* All arguments are exhausted.  Success! */
    return QP_NULL;
}

/* ---- map: slots func, iters, strict ---- */
enum { QP_MAP_FUNC, QP_MAP_ITERS, QP_MAP_STRICT, QP_MAP_NSLOTS };

static JSValue qp_map_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                          size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf), strict;
    JSValue iters, obj, *s;

    if (qp_bi_parse_strict(ctx, "map", args + nargs, kwnames, &strict) < 0)
        return JS_EXCEPTION;
    if (nargs < 2)
        return qp_raise_type_error(ctx, "map() must have at least two arguments.");
    iters = qp_bi_iter_tuple(ctx, args + 1, nargs - 1);
    if (JS_IsException(iters))
        return JS_EXCEPTION;
    obj = qp_bi_new_instance(ctx, subtype, &s);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, iters);
        return JS_EXCEPTION;
    }
    qp_bi_slot_set(ctx, s, QP_MAP_FUNC, JS_DupValue(ctx, args[0]));
    qp_bi_slot_set(ctx, s, QP_MAP_ITERS, iters);
    qp_bi_slot_set(ctx, s, QP_MAP_STRICT, qp_bool_new(strict));
    return obj;
}

static JSValue qp_map_next(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_bi_slots(self, QP_MAP_NSLOTS);
    JSValue small_stack[8], *stack, result = JS_EXCEPTION, iters;
    int niters, nargs = 0, i;

    if (!s)
        return QP_NULL;
    iters = JS_DupValue(ctx, s[QP_MAP_ITERS]);
    niters = (int)QP_TUPLE_SIZE(iters);
    if (niters <= (int)countof(small_stack)) {
        stack = small_stack;
    } else {
        stack = js_malloc(ctx, sizeof(JSValue) * niters);
        if (!stack) {
            JS_FreeValue(ctx, iters);
            return JS_EXCEPTION;
        }
    }
    for (i = 0; i < niters; i++) {
        JSValue val = qp_next(ctx, QP_TUPLE_ITEMS(iters)[i]);
        if (JS_IsException(val))
            goto exit;
        if (QP_IS_NULL(val)) {
            if (JS_ToBool(ctx, s[QP_MAP_STRICT]))
                result = qp_bi_strict_check(ctx, "map", iters, i);
            else
                result = QP_NULL;
            goto exit;
        }
        stack[i] = val;
        nargs++;
    }
    result = qp_vectorcall(ctx, s[QP_MAP_FUNC], (JSValueConst *)stack, niters, QP_NULL);
exit:
    for (i = 0; i < nargs; i++)
        JS_FreeValue(ctx, stack[i]);
    if (stack != small_stack)
        js_free(ctx, stack);
    JS_FreeValue(ctx, iters);
    return result;
}

static JSValue qp_map_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSValue *s = qp_instance_slots(self);
    JSValue items[3], fargs;
    Py_ssize_t n, i;
    if (!s || QP_IS_NULL(s[QP_MAP_ITERS]))
        return qp_raise_type_error(ctx, "uninitialized map object");
    n = QP_TUPLE_SIZE(s[QP_MAP_ITERS]);
    fargs = qp_tuple_new(ctx, n + 1);
    if (JS_IsException(fargs))
        return JS_EXCEPTION;
    JS_FreeValue(ctx, QP_TUPLE_ITEMS(fargs)[0]);
    QP_TUPLE_ITEMS(fargs)[0] = JS_DupValue(ctx, s[QP_MAP_FUNC]);
    for (i = 0; i < n; i++) {
        JS_FreeValue(ctx, QP_TUPLE_ITEMS(fargs)[i + 1]);
        QP_TUPLE_ITEMS(fargs)[i + 1] = JS_DupValue(ctx, QP_TUPLE_ITEMS(s[QP_MAP_ITERS])[i]);
    }
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)));
    items[1] = fargs;
    if (JS_ToBool(ctx, s[QP_MAP_STRICT])) {
        items[2] = JS_TRUE;
        return qp_tuple_new_steal(ctx, 3, items);
    }
    return qp_tuple_new_steal(ctx, 2, items);
}

static JSValue qp_map_setstate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    JSValue *s = qp_instance_slots(self);
    int strict = qp_bool(ctx, args[0]);
    if (strict < 0)
        return JS_EXCEPTION;
    if (s)
        qp_bi_slot_set(ctx, s, QP_MAP_STRICT, qp_bool_new(strict));
    return JS_UNDEFINED;
}

/* ---- filter: slots func, it ---- */
enum { QP_FILTER_FUNC, QP_FILTER_IT, QP_FILTER_NSLOTS };

static JSValue qp_filter_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                             size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);
    JSValue it, obj, *s;
    QPType *ft = QP_TYPE(ctx, filter);

    if ((subtype == ft || subtype->slots.tp_init == ft->slots.tp_init) &&
        qp_arg_no_keywords(ctx, "filter", kwnames) < 0)
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "filter", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    /* Get iterator. */
    it = qp_iter(ctx, args[1]);
    if (JS_IsException(it))
        return JS_EXCEPTION;
    obj = qp_bi_new_instance(ctx, subtype, &s);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, it);
        return JS_EXCEPTION;
    }
    qp_bi_slot_set(ctx, s, QP_FILTER_FUNC, JS_DupValue(ctx, args[0]));
    qp_bi_slot_set(ctx, s, QP_FILTER_IT, it);
    return obj;
}

static JSValue qp_filter_next(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_bi_slots(self, QP_FILTER_NSLOTS);
    JSValue func, it, item;
    BOOL checktrue;
    int ok;

    if (!s)
        return QP_NULL;
    func = JS_DupValue(ctx, s[QP_FILTER_FUNC]);
    it = JS_DupValue(ctx, s[QP_FILTER_IT]);
    checktrue = qp_bi_is_none(func) || qp_bi_same(func, QP_TYPE_VALUE(QP_TYPE(ctx, bool)));
    for (;;) {
        item = qp_next(ctx, it);
        if (JS_IsException(item) || QP_IS_NULL(item))
            break;
        if (checktrue) {
            ok = qp_bool(ctx, item);
        } else {
            JSValue good = qp_call1(ctx, func, item);
            if (JS_IsException(good)) {
                JS_FreeValue(ctx, item);
                item = JS_EXCEPTION;
                break;
            }
            ok = qp_bool(ctx, good);
            JS_FreeValue(ctx, good);
        }
        if (ok > 0)
            break;
        JS_FreeValue(ctx, item);
        if (ok < 0) {
            item = JS_EXCEPTION;
            break;
        }
    }
    JS_FreeValue(ctx, func);
    JS_FreeValue(ctx, it);
    return item;
}

static JSValue qp_filter_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    JSValue *s = qp_instance_slots(self);
    JSValue items[2];
    JSValueConst fa[2];
    if (!s || QP_IS_NULL(s[QP_FILTER_IT]))
        return qp_raise_type_error(ctx, "uninitialized filter object");
    fa[0] = s[QP_FILTER_FUNC];
    fa[1] = s[QP_FILTER_IT];
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)));
    items[1] = qp_tuple_pack(ctx, 2, fa);
    if (JS_IsException(items[1])) {
        JS_FreeValue(ctx, items[0]);
        return JS_EXCEPTION;
    }
    return qp_tuple_new_steal(ctx, 2, items);
}

/* ---- zip: slots ittuple, strict ---- */
enum { QP_ZIP_ITERS, QP_ZIP_STRICT, QP_ZIP_NSLOTS };

static JSValue qp_zip_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                          size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf), strict;
    JSValue iters, obj, *s;

    if (qp_bi_parse_strict(ctx, "zip", args + nargs, kwnames, &strict) < 0)
        return JS_EXCEPTION;
    /* obtain iterators */
    iters = qp_bi_iter_tuple(ctx, args, nargs);
    if (JS_IsException(iters))
        return JS_EXCEPTION;
    obj = qp_bi_new_instance(ctx, subtype, &s);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, iters);
        return JS_EXCEPTION;
    }
    qp_bi_slot_set(ctx, s, QP_ZIP_ITERS, iters);
    qp_bi_slot_set(ctx, s, QP_ZIP_STRICT, qp_bool_new(strict));
    return obj;
}

static JSValue qp_zip_next(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_bi_slots(self, QP_ZIP_NSLOTS);
    JSValue iters, *items, result;
    int n, i;

    if (!s)
        return QP_NULL;
    iters = JS_DupValue(ctx, s[QP_ZIP_ITERS]);
    n = (int)QP_TUPLE_SIZE(iters);
    if (n == 0) {
        JS_FreeValue(ctx, iters);
        return QP_NULL;
    }
    items = js_malloc(ctx, sizeof(JSValue) * n);
    if (!items) {
        JS_FreeValue(ctx, iters);
        return JS_EXCEPTION;
    }
    for (i = 0; i < n; i++) {
        JSValue item = qp_next(ctx, QP_TUPLE_ITEMS(iters)[i]);
        if (JS_IsException(item) || QP_IS_NULL(item)) {
            int j;
            for (j = 0; j < i; j++)
                JS_FreeValue(ctx, items[j]);
            js_free(ctx, items);
            if (QP_IS_NULL(item) && JS_ToBool(ctx, s[QP_ZIP_STRICT]))
                result = qp_bi_strict_check(ctx, "zip", iters, i);
            else
                result = item;
            JS_FreeValue(ctx, iters);
            return result;
        }
        items[i] = item;
    }
    result = qp_tuple_new_steal(ctx, n, items);
    js_free(ctx, items);
    JS_FreeValue(ctx, iters);
    return result;
}

static JSValue qp_zip_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                             int nargs, JSValueConst kwnames, int magic)
{
    JSValue *s = qp_instance_slots(self);
    JSValue items[3];
    if (!s || QP_IS_NULL(s[QP_ZIP_ITERS]))
        return qp_raise_type_error(ctx, "uninitialized zip object");
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)));
    items[1] = JS_DupValue(ctx, s[QP_ZIP_ITERS]);
    if (JS_ToBool(ctx, s[QP_ZIP_STRICT])) {
        items[2] = JS_TRUE;
        return qp_tuple_new_steal(ctx, 3, items);
    }
    return qp_tuple_new_steal(ctx, 2, items);
}

static JSValue qp_zip_setstate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    JSValue *s = qp_instance_slots(self);
    int strict = qp_bool(ctx, args[0]);
    if (strict < 0)
        return JS_EXCEPTION;
    if (s)
        qp_bi_slot_set(ctx, s, QP_ZIP_STRICT, qp_bool_new(strict));
    return JS_UNDEFINED;
}

/* ---- enumerate: slots it, index (int) ---- */
enum { QP_ENUM_IT, QP_ENUM_INDEX, QP_ENUM_NSLOTS };

static JSValue qp_enum_new_impl(JSContext *ctx, QPType *type, JSValueConst iterable,
                                JSValueConst start)
{
    JSValue obj, *s, index, it;

    obj = qp_bi_new_instance(ctx, type, &s);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    if (!QP_IS_NULL(start)) {
        index = qp_index(ctx, start);
        if (JS_IsException(index)) {
            JS_FreeValue(ctx, obj);
            return JS_EXCEPTION;
        }
    } else {
        index = JS_NewInt32(ctx, 0);
    }
    it = qp_iter(ctx, iterable);
    if (JS_IsException(it)) {
        JS_FreeValue(ctx, index);
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }
    qp_bi_slot_set(ctx, s, QP_ENUM_IT, it);
    qp_bi_slot_set(ctx, s, QP_ENUM_INDEX, index);
    return obj;
}

static int qp_enum_check_keyword(JSContext *ctx, JSValueConst kwnames, int index, const char *name)
{
    JSValueConst kw = QP_TUPLE_ITEMS(kwnames)[index];
    const char *s = JS_ToCString(ctx, kw);
    int ok;
    if (!s)
        return -1;
    ok = strcmp(s, name) == 0;
    JS_FreeCString(ctx, s);
    if (!ok) {
        qp_raise_type_error(ctx, "'%S' is an invalid keyword argument for enumerate()", kw);
        return -1;
    }
    return 0;
}

static JSValue qp_enum_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                           size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf), nkwargs = qp_bi_nkw(kwnames);

    if (subtype != QP_TYPE(ctx, enumerate)) {
        /* Argument Clinic signature enumerate(iterable, start=0) */
        static const char *const kw[] = { "iterable", "start", NULL };
        static const QPArgSpec spec = { "enumerate", kw, 1, 2, 0 };
        JSValueConst a[2];
        if (qp_arg_parse(ctx, &spec, args, nargs, kwnames, a) < 0)
            return JS_EXCEPTION;
        return qp_enum_new_impl(ctx, subtype, a[0], a[1]);
    }
    /* Manually implement enumerate(iterable, start=...) */
    if (nargs + nkwargs == 2) {
        if (nkwargs == 1) {
            if (qp_enum_check_keyword(ctx, kwnames, 0, "start") < 0)
                return JS_EXCEPTION;
        } else if (nkwargs == 2) {
            const char *kw0 = JS_ToCString(ctx, QP_TUPLE_ITEMS(kwnames)[0]);
            int is_start;
            if (!kw0)
                return JS_EXCEPTION;
            is_start = strcmp(kw0, "start") == 0;
            JS_FreeCString(ctx, kw0);
            if (is_start) {
                if (qp_enum_check_keyword(ctx, kwnames, 1, "iterable") < 0)
                    return JS_EXCEPTION;
                return qp_enum_new_impl(ctx, subtype, args[1], args[0]);
            }
            if (qp_enum_check_keyword(ctx, kwnames, 0, "iterable") < 0 ||
                qp_enum_check_keyword(ctx, kwnames, 1, "start") < 0)
                return JS_EXCEPTION;
        }
        return qp_enum_new_impl(ctx, subtype, args[0], args[1]);
    }
    if (nargs + nkwargs == 1) {
        if (nkwargs == 1 && qp_enum_check_keyword(ctx, kwnames, 0, "iterable") < 0)
            return JS_EXCEPTION;
        return qp_enum_new_impl(ctx, subtype, args[0], QP_NULL);
    }
    if (nargs == 0)
        return qp_raise_type_error(ctx, "enumerate() missing required argument 'iterable'");
    return qp_raise_type_error(ctx, "enumerate() takes at most 2 arguments (%d given)",
                               nargs + nkwargs);
}

static JSValue qp_enum_next(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_bi_slots(self, QP_ENUM_NSLOTS);
    JSValue item, index, next_index, pair[2];

    if (!s)
        return QP_NULL;
    item = qp_next(ctx, s[QP_ENUM_IT]);
    if (JS_IsException(item) || QP_IS_NULL(item))
        return item;
    index = s[QP_ENUM_INDEX];
    if (JS_VALUE_GET_TAG(index) == JS_TAG_INT && JS_VALUE_GET_INT(index) != INT32_MAX) {
        next_index = JS_NewInt32(ctx, JS_VALUE_GET_INT(index) + 1);
    } else {
        JSValue one = JS_NewInt32(ctx, 1);
        next_index = qp_binop(ctx, index, one, QP_NB_ADD);
        if (JS_IsException(next_index)) {
            JS_FreeValue(ctx, item);
            return JS_EXCEPTION;
        }
    }
    pair[0] = s[QP_ENUM_INDEX]; /* ownership moves into the tuple */
    s[QP_ENUM_INDEX] = next_index;
    pair[1] = item;
    return qp_tuple_new_steal(ctx, 2, pair);
}

static JSValue qp_enum_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    JSValue *s = qp_instance_slots(self);
    JSValue items[2];
    JSValueConst fa[2];
    if (!s || QP_IS_NULL(s[QP_ENUM_IT]))
        return qp_raise_type_error(ctx, "uninitialized enumerate object");
    fa[0] = s[QP_ENUM_IT];
    fa[1] = s[QP_ENUM_INDEX];
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)));
    items[1] = qp_tuple_pack(ctx, 2, fa);
    if (JS_IsException(items[1])) {
        JS_FreeValue(ctx, items[0]);
        return JS_EXCEPTION;
    }
    return qp_tuple_new_steal(ctx, 2, items);
}

/* Py_GenericAlias as a classmethod (enumerate[int]) */
static JSValue qp_bi_class_getitem(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    return qp_generic_alias_new(ctx, self, args[0]);
}

/* ---- reversed: slots seq, index ---- */
enum { QP_REV_SEQ, QP_REV_INDEX, QP_REV_NSLOTS };

/* PySequence_Check */
static BOOL qp_bi_sequence_check(JSContext *ctx, JSValueConst v)
{
    if (qp_dict_check(ctx, v))
        return FALSE;
    return qp_type_of(ctx, v)->slots.sq.sq_item != NULL;
}

static int64_t qp_rev_index(JSValueConst v)
{
    int64_t x = -1;
    int overflow = 0;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT)
        return JS_VALUE_GET_INT(v);
    if (qp_long_as_int64_overflow(v, &x, &overflow) < 0 || overflow)
        return -1;
    return x;
}

static JSValue qp_reversed_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                               size_t nargsf, JSValueConst kwnames)
{
    int nargs = (int)qp_nargs(nargsf);
    JSValueConst seq;
    JSValue reversed_meth, obj, *s;
    Py_ssize_t n;

    if (subtype == QP_TYPE(ctx, reversed) && qp_arg_no_keywords(ctx, "reversed", kwnames) < 0)
        return JS_EXCEPTION;
    if (subtype != QP_TYPE(ctx, reversed) && qp_arg_no_keywords(ctx, "reversed", kwnames) < 0)
        return JS_EXCEPTION;
    if (qp_arg_check_positional(ctx, "reversed", nargs, 1, 1) < 0)
        return JS_EXCEPTION;
    seq = args[0];

    reversed_meth = qp_lookup_special(ctx, seq, QP_ATOM(ctx, __reversed__));
    if (JS_IsException(reversed_meth))
        return JS_EXCEPTION;
    if (!QP_IS_NULL(reversed_meth) && qp_bi_is_none(reversed_meth)) {
        return qp_bi_raise_tp(ctx, QP_EXC(ctx, TypeError),
                              "'%.200U' object is not reversible", seq);
    }
    if (!QP_IS_NULL(reversed_meth)) {
        JSValue res = qp_call0(ctx, reversed_meth);
        JS_FreeValue(ctx, reversed_meth);
        return res;
    }
    if (!qp_bi_sequence_check(ctx, seq))
        return qp_bi_raise_tp(ctx, QP_EXC(ctx, TypeError),
                              "'%.200U' object is not reversible", seq);
    n = qp_len(ctx, seq);
    if (n == -1)
        return JS_EXCEPTION;
    obj = qp_bi_new_instance(ctx, subtype, &s);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    qp_bi_slot_set(ctx, s, QP_REV_SEQ, JS_DupValue(ctx, seq));
    qp_bi_slot_set(ctx, s, QP_REV_INDEX, qp_long_from_int64(ctx, n - 1));
    return obj;
}

static JSValue qp_reversed_next(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_bi_slots(self, QP_REV_NSLOTS);
    int64_t index;

    if (!s)
        return QP_NULL;
    index = qp_rev_index(s[QP_REV_INDEX]);
    if (index >= 0) {
        JSValue seq = JS_DupValue(ctx, s[QP_REV_SEQ]);
        QPType *t = qp_type_of(ctx, seq);
        JSValue item;
        if (t->slots.sq.sq_item) {
            item = t->slots.sq.sq_item(ctx, seq, (Py_ssize_t)index);
        } else {
            JSValue key = qp_long_from_int64(ctx, index);
            item = qp_getitem(ctx, seq, key);
            JS_FreeValue(ctx, key);
        }
        JS_FreeValue(ctx, seq);
        qp_bi_slot_set(ctx, s, QP_REV_INDEX, qp_long_from_int64(ctx, index - 1));
        if (!JS_IsException(item))
            return item;
        if (!qp_exc_pending_matches(ctx, QP_EXC(ctx, IndexError)) &&
            !qp_exc_pending_matches(ctx, QP_EXC(ctx, StopIteration)))
            return JS_EXCEPTION;
        qp_bi_clear_exc(ctx);
    }
    qp_bi_slot_set(ctx, s, QP_REV_INDEX, JS_NewInt32(ctx, -1));
    qp_bi_slot_set(ctx, s, QP_REV_SEQ, QP_NULL);
    return QP_NULL;
}

static JSValue qp_reversed_len(JSContext *ctx, JSValueConst self, JSValueConst *args,
                               int nargs, JSValueConst kwnames, int magic)
{
    JSValue *s = qp_instance_slots(self);
    Py_ssize_t position, seqsize;

    if (!s || QP_IS_NULL(s[QP_REV_SEQ]))
        return JS_NewInt32(ctx, 0);
    seqsize = qp_len(ctx, s[QP_REV_SEQ]);
    if (seqsize == -1)
        return JS_EXCEPTION;
    position = (Py_ssize_t)qp_rev_index(s[QP_REV_INDEX]) + 1;
    return qp_long_from_int64(ctx, seqsize < position ? 0 : position);
}

static JSValue qp_reversed_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    JSValue *s = qp_instance_slots(self);
    JSValue items[3];
    items[0] = JS_DupValue(ctx, QP_TYPE_VALUE(qp_type_of(ctx, self)));
    if (s && !QP_IS_NULL(s[QP_REV_SEQ])) {
        items[1] = qp_tuple_pack(ctx, 1, (JSValueConst *)&s[QP_REV_SEQ]);
        if (JS_IsException(items[1])) {
            JS_FreeValue(ctx, items[0]);
            return JS_EXCEPTION;
        }
        items[2] = JS_DupValue(ctx, s[QP_REV_INDEX]);
        return qp_tuple_new_steal(ctx, 3, items);
    } else {
        JSValueConst empty = QP_STATE(ctx)->empty_tuple;
        items[1] = qp_tuple_pack(ctx, 1, &empty);
        if (JS_IsException(items[1])) {
            JS_FreeValue(ctx, items[0]);
            return JS_EXCEPTION;
        }
        return qp_tuple_new_steal(ctx, 2, items);
    }
}

static JSValue qp_reversed_setstate(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    JSValue *s = qp_instance_slots(self);
    Py_ssize_t index, n;
    JSValue iobj;

    iobj = qp_index(ctx, args[0]);
    if (JS_IsException(iobj))
        return JS_EXCEPTION;
    if (qp_long_as_ssize(ctx, iobj, &index) < 0) {
        JS_FreeValue(ctx, iobj);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, iobj);
    if (s && !QP_IS_NULL(s[QP_REV_SEQ])) {
        n = qp_len(ctx, s[QP_REV_SEQ]);
        if (n < 0)
            return JS_EXCEPTION;
        if (index < -1)
            index = -1;
        else if (index > n - 1)
            index = n - 1;
        qp_bi_slot_set(ctx, s, QP_REV_INDEX, qp_long_from_int64(ctx, index));
    }
    return JS_UNDEFINED;
}

/* ========================================================================= */
/* sentinel (Objects/sentinelobject.c)                                        */
/* ========================================================================= */

enum { QP_SENTINEL_NAME, QP_SENTINEL_MODULE, QP_SENTINEL_REPR, QP_SENTINEL_NSLOTS };

static JSValue qp_sentinel_new(JSContext *ctx, QPType *subtype, JSValueConst *args,
                               size_t nargsf, JSValueConst kwnames)
{
    static const char *const kw[] = { "", "repr", NULL };
    static const QPArgSpec spec = { "sentinel", kw, 1, 1, 0 };
    JSValueConst a[2];
    JSValueConst repr;
    JSValue obj, *s, module = JS_UNDEFINED;
    JSStackFrame *sf;

    if (qp_arg_parse(ctx, &spec, args, (int)qp_nargs(nargsf), kwnames, a) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, a[0]))
        return qp_bi_bad_argument(ctx, "sentinel", "argument 1", "str", a[0]);
    repr = a[1];
    if (QP_IS_NULL(repr) || qp_bi_is_none(repr))
        repr = QP_NULL;
    else if (!qp_str_check(ctx, repr))
        return qp_bi_bad_argument(ctx, "sentinel", "argument 'repr'", "str or None", repr);
    /* caller(): module of the calling Python function */
    sf = qp_bi_caller_frame(ctx);
    if (sf) {
        QPFuncData *fd = qp_function_data(sf->cur_func);
        if (fd && !QP_IS_NULL(fd->module))
            module = JS_DupValue(ctx, fd->module);
    }
    obj = qp_bi_new_instance(ctx, subtype, &s);
    if (JS_IsException(obj)) {
        JS_FreeValue(ctx, module);
        return JS_EXCEPTION;
    }
    qp_bi_slot_set(ctx, s, QP_SENTINEL_NAME, JS_DupValue(ctx, a[0]));
    qp_bi_slot_set(ctx, s, QP_SENTINEL_MODULE, module);
    qp_bi_slot_set(ctx, s, QP_SENTINEL_REPR, JS_DupValue(ctx, repr));
    return obj;
}

static JSValue qp_sentinel_repr(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_instance_slots(self);
    if (!s)
        return qp_raise_type_error(ctx, "uninitialized sentinel object");
    if (!QP_IS_NULL(s[QP_SENTINEL_REPR]))
        return JS_DupValue(ctx, s[QP_SENTINEL_REPR]);
    return JS_DupValue(ctx, s[QP_SENTINEL_NAME]);
}

static JSValue qp_sentinel_copy(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                int nargs, JSValueConst kwnames, int magic)
{
    return JS_DupValue(ctx, self);
}

static JSValue qp_sentinel_reduce(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    JSValue *s = qp_instance_slots(self);
    if (!s)
        return qp_raise_type_error(ctx, "uninitialized sentinel object");
    return JS_DupValue(ctx, s[QP_SENTINEL_NAME]);
}

/* nb_or = _Py_union_type_or */
static JSValue qp_sentinel_or(JSContext *ctx, JSValueConst a, JSValueConst b)
{
    return qp_union_new(ctx, a, b);
}

/* ========================================================================= */
/* anext_awaitable (Objects/iterobject.c)                                     */
/* ========================================================================= */

enum { QP_ANEXT_WRAPPED, QP_ANEXT_DEFAULT, QP_ANEXT_NSLOTS };

static JSValue qp_bi_anext_awaitable_new(JSContext *ctx, JSValueConst awaitable,
                                         JSValueConst default_value)
{
    JSValue obj, *s;
    obj = qp_bi_new_instance(ctx, QP_TYPE(ctx, anext_awaitable), &s);
    if (JS_IsException(obj))
        return JS_EXCEPTION;
    qp_bi_slot_set(ctx, s, QP_ANEXT_WRAPPED, JS_DupValue(ctx, awaitable));
    qp_bi_slot_set(ctx, s, QP_ANEXT_DEFAULT, JS_DupValue(ctx, default_value));
    return obj;
}

/* anextawaitable_getiter */
static JSValue qp_anext_getiter(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_instance_slots(self);
    JSValue awaitable;
    QPType *t;

    if (!s || QP_IS_NULL(s[QP_ANEXT_WRAPPED]))
        return qp_raise_type_error(ctx, "uninitialized anext_awaitable object");
    awaitable = qp_get_awaitable(ctx, s[QP_ANEXT_WRAPPED], 0);
    if (JS_IsException(awaitable))
        return JS_EXCEPTION;
    t = qp_type_of(ctx, awaitable);
    if (t->slots.tp_iternext == NULL && t->slots.am.am_await) {
        /* _PyCoro_GetAwaitableIter returns a Coroutine, a Generator, or an
           iterator.  Of these, only coroutines lack tp_iternext. */
        JSValue new_awaitable = t->slots.am.am_await(ctx, awaitable);
        JS_FreeValue(ctx, awaitable);
        if (JS_IsException(new_awaitable))
            return JS_EXCEPTION;
        awaitable = new_awaitable;
        if (!qp_bi_iter_check(ctx, awaitable)) {
            qp_raise_type_error(ctx, "%T.__await__() must return an iterable, not %T",
                                self, awaitable);
            JS_FreeValue(ctx, awaitable);
            return JS_EXCEPTION;
        }
    }
    return awaitable;
}

/* StopAsyncIteration -> StopIteration(default) after a failed step. */
static JSValue qp_anext_translate(JSContext *ctx, JSValueConst self)
{
    JSValue *s = qp_instance_slots(self);
    if (qp_exc_pending_matches(ctx, QP_EXC(ctx, StopAsyncIteration))) {
        qp_bi_clear_exc(ctx);
        return qp_raise_stop_iteration(ctx, s ? s[QP_ANEXT_DEFAULT] : JS_UNDEFINED);
    }
    return JS_EXCEPTION;
}

static JSValue qp_anext_iternext(JSContext *ctx, JSValueConst self)
{
    JSValue awaitable = qp_anext_getiter(ctx, self), result;
    int r;
    if (JS_IsException(awaitable))
        return JS_EXCEPTION;
    r = qp_iter_send(ctx, awaitable, JS_UNDEFINED, &result);
    JS_FreeValue(ctx, awaitable);
    if (r == QP_SEND_NEXT)
        return result;
    if (r == QP_SEND_RETURN) {
        JSValue e = qp_raise_stop_iteration(ctx, result);
        JS_FreeValue(ctx, result);
        return e;
    }
    return qp_anext_translate(ctx, self);
}

/* anextawaitable_proxy: magic 0 send(arg), 1 throw(*args), 2 close() */
static JSValue qp_anext_proxy(JSContext *ctx, JSValueConst self, JSValueConst *args,
                              int nargs, JSValueConst kwnames, int magic)
{
    static const QPAtomId meths[3] = { QP_ATOM_send, QP_ATOM_throw, QP_ATOM_close };
    JSValue awaitable = qp_anext_getiter(ctx, self), ret;
    if (JS_IsException(awaitable))
        return JS_EXCEPTION;
    if (magic == 1 && qp_arg_check_positional(ctx, "throw", nargs, 1, 3) < 0) {
        JS_FreeValue(ctx, awaitable);
        return JS_EXCEPTION;
    }
    ret = qp_call_method(ctx, awaitable, QP_STATE(ctx)->atoms[meths[magic]], args,
                         magic == 2 ? 0 : (magic == 0 ? 1 : nargs), QP_NULL);
    JS_FreeValue(ctx, awaitable);
    if (!JS_IsException(ret))
        return ret;
    return qp_anext_translate(ctx, self);
}

/* ========================================================================= */
/* Type specs                                                                 */
/* ========================================================================= */

static const QPMethodDef qp_map_methods[] = {
    { "__reduce__", qp_map_reduce, QP_METH_NOARGS, 0, "Return state information for pickling." },
    { "__setstate__", qp_map_setstate, QP_METH_O, 0, "Set state information for unpickling." },
    { NULL }
};

static const QPMethodDef qp_filter_methods[] = {
    { "__reduce__", qp_filter_reduce, QP_METH_NOARGS, 0, "Return state information for pickling." },
    { NULL }
};

static const QPMethodDef qp_zip_methods[] = {
    { "__reduce__", qp_zip_reduce, QP_METH_NOARGS, 0, "Return state information for pickling." },
    { "__setstate__", qp_zip_setstate, QP_METH_O, 0, "Set state information for unpickling." },
    { NULL }
};

static const QPMethodDef qp_enum_methods[] = {
    { "__reduce__", qp_enum_reduce, QP_METH_NOARGS, 0, "Return state information for pickling." },
    { "__class_getitem__", qp_bi_class_getitem, QP_METH_O | QP_METH_CLASS, 0,
      "See PEP 585" },
    { NULL }
};

static const QPMethodDef qp_reversed_methods[] = {
    { "__length_hint__", qp_reversed_len, QP_METH_NOARGS, 0,
      "Private method returning an estimate of len(list(it))." },
    { "__reduce__", qp_reversed_reduce, QP_METH_NOARGS, 0, "Return state information for pickling." },
    { "__setstate__", qp_reversed_setstate, QP_METH_O, 0, "Set state information for unpickling." },
    { NULL }
};

static const QPMethodDef qp_sentinel_methods[] = {
    { "__copy__", qp_sentinel_copy, QP_METH_NOARGS, 0, NULL },
    { "__deepcopy__", qp_sentinel_copy, QP_METH_O, 0, NULL },
    { "__reduce__", qp_sentinel_reduce, QP_METH_NOARGS, 0, NULL },
    { NULL }
};

static const QPMemberDef qp_sentinel_members[] = {
    { "__name__", QP_SENTINEL_NAME, QP_MEMBER_READONLY, NULL },
    { "__module__", QP_SENTINEL_MODULE, 0, NULL },
    { NULL }
};

static const QPMethodDef qp_anext_methods[] = {
    { "send", qp_anext_proxy, QP_METH_O, 0,
      "send(arg) -> send 'arg' into the wrapped iterator,\nreturn next yielded value or raise StopIteration." },
    { "throw", qp_anext_proxy, QP_METH_FASTCALL, 1,
      "throw(value)\nthrow(typ[,val[,tb]])\n\nRaise exception in the wrapped iterator, return next yielded value\nor raise StopIteration.\nthe (type, val, tb) signature is deprecated, \nand may be removed in a future version of Python." },
    { "close", qp_anext_proxy, QP_METH_NOARGS, 2, "close() -> raise GeneratorExit inside generator." },
    { NULL }
};

#define QP_BI_ITER_SPEC(tname, tdoc, nslot, newf, nextf, meths)              \
    {                                                                        \
        .name = tname, .doc = tdoc,                                          \
        .flags = QP_TPFLAGS_BASETYPE | QP_TPFLAGS_IMMUTABLETYPE,             \
        .base = -1, .exc_base = -1,                                          \
        .instance_class_id = JS_CLASS_PY_OBJECT, .nslots = nslot,            \
        .slots = { .tp_getattro = qp_generic_getattr, .tp_iter = qp_bi_self_iter, \
                   .tp_iternext = nextf, .tp_new = newf },                   \
        .methods = meths,                                                    \
    }

static const QPTypeSpec qp_map_spec =
    QP_BI_ITER_SPEC("map", qp_bi_doc_map, QP_MAP_NSLOTS, qp_map_new, qp_map_next, qp_map_methods);
static const QPTypeSpec qp_filter_spec =
    QP_BI_ITER_SPEC("filter", qp_bi_doc_filter, QP_FILTER_NSLOTS, qp_filter_new, qp_filter_next,
                    qp_filter_methods);
static const QPTypeSpec qp_zip_spec =
    QP_BI_ITER_SPEC("zip", qp_bi_doc_zip, QP_ZIP_NSLOTS, qp_zip_new, qp_zip_next, qp_zip_methods);
static const QPTypeSpec qp_enum_spec =
    QP_BI_ITER_SPEC("enumerate", qp_bi_doc_enum_enum_new, QP_ENUM_NSLOTS, qp_enum_new,
                    qp_enum_next, qp_enum_methods);
static const QPTypeSpec qp_reversed_spec =
    QP_BI_ITER_SPEC("reversed", qp_bi_doc_enum_reversed_new, QP_REV_NSLOTS, qp_reversed_new,
                    qp_reversed_next, qp_reversed_methods);

static const QPTypeSpec qp_sentinel_spec = {
    .name = "sentinel", .doc = qp_bi_doc_sentinel,
    .flags = QP_TPFLAGS_IMMUTABLETYPE,
    .base = -1, .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT, .nslots = QP_SENTINEL_NSLOTS,
    .slots = { .tp_getattro = qp_generic_getattr, .tp_repr = qp_sentinel_repr,
               .tp_new = qp_sentinel_new, .nb = { .nb_or = qp_sentinel_or } },
    .methods = qp_sentinel_methods,
    .members = qp_sentinel_members,
};

static const QPTypeSpec qp_anext_spec = {
    .name = "anext_awaitable", .doc = NULL,
    .flags = QP_TPFLAGS_IMMUTABLETYPE | QP_TPFLAGS_DISALLOW_INSTANTIATION,
    .base = -1, .exc_base = -1,
    .instance_class_id = JS_CLASS_PY_OBJECT, .nslots = QP_ANEXT_NSLOTS,
    .slots = { .tp_getattro = qp_generic_getattr, .tp_iter = qp_bi_self_iter,
               .tp_iternext = qp_anext_iternext, .am = { .am_await = qp_bi_self_iter } },
    .methods = qp_anext_methods,
};

/* ========================================================================= */
/* The builtins module                                                        */
/* ========================================================================= */

#define QP_BI_FAST (QP_METH_FASTCALL)
#define QP_BI_FASTKW (QP_METH_FASTCALL | QP_METH_KEYWORDS)

static const QPMethodDef qp_builtin_methods[] = {
    { "__build_class__", qp_bi_build_class, QP_BI_FASTKW, 0, qp_bi_doc_build_class },
    { "__import__", qp_bi_import, QP_BI_FASTKW, 0, qp_bi_doc_builtin___import__ },
    { "__lazy_import__", qp_bi_lazy_import, QP_BI_FASTKW, 0, qp_bi_doc_builtin___lazy_import__ },
    { "abs", qp_bi_abs, QP_METH_O, 0, qp_bi_doc_builtin_abs },
    { "all", qp_bi_all_any, QP_METH_O, 0, qp_bi_doc_builtin_all },
    { "any", qp_bi_all_any, QP_METH_O, 1, qp_bi_doc_builtin_any },
    { "ascii", qp_bi_ascii, QP_METH_O, 0, qp_bi_doc_builtin_ascii },
    { "bin", qp_bi_to_base, QP_METH_O, 2, qp_bi_doc_builtin_bin },
    { "breakpoint", qp_bi_breakpoint, QP_BI_FASTKW, 0, qp_bi_doc_breakpoint },
    { "callable", qp_bi_callable_fn, QP_METH_O, 0, qp_bi_doc_builtin_callable },
    { "chr", qp_bi_chr, QP_METH_O, 0, qp_bi_doc_builtin_chr },
    { "compile", qp_bi_compile, QP_BI_FASTKW, 0, qp_bi_doc_builtin_compile },
    { "delattr", qp_bi_delattr, QP_BI_FAST, 0, qp_bi_doc_builtin_delattr },
    { "dir", qp_bi_dir, QP_BI_FAST, 0, qp_bi_doc_dir },
    { "divmod", qp_bi_divmod, QP_BI_FAST, 0, qp_bi_doc_builtin_divmod },
    { "eval", qp_bi_eval, QP_BI_FASTKW, 0, qp_bi_doc_builtin_eval },
    { "exec", qp_bi_exec, QP_BI_FASTKW, 0, qp_bi_doc_builtin_exec },
    { "format", qp_bi_format, QP_BI_FAST, 0, qp_bi_doc_builtin_format },
    { "getattr", qp_bi_getattr, QP_BI_FAST, 0, qp_bi_doc_getattr },
    { "globals", qp_bi_globals, QP_METH_NOARGS, 0, qp_bi_doc_builtin_globals },
    { "hasattr", qp_bi_hasattr, QP_BI_FAST, 0, qp_bi_doc_builtin_hasattr },
    { "hash", qp_bi_hash, QP_METH_O, 0, qp_bi_doc_builtin_hash },
    { "hex", qp_bi_to_base, QP_METH_O, 16, qp_bi_doc_builtin_hex },
    { "id", qp_bi_id, QP_METH_O, 0, qp_bi_doc_builtin_id },
    { "input", qp_bi_input, QP_BI_FAST, 0, qp_bi_doc_builtin_input },
    { "isinstance", qp_bi_isinstance, QP_BI_FAST, 0, qp_bi_doc_builtin_isinstance },
    { "issubclass", qp_bi_isinstance, QP_BI_FAST, 1, qp_bi_doc_builtin_issubclass },
    { "iter", qp_bi_iter, QP_BI_FAST, 0, qp_bi_doc_iter },
    { "aiter", qp_bi_aiter, QP_METH_O, 0, qp_bi_doc_builtin_aiter },
    { "len", qp_bi_len, QP_METH_O, 0, qp_bi_doc_builtin_len },
    { "locals", qp_bi_locals, QP_METH_NOARGS, 0, qp_bi_doc_builtin_locals },
    { "max", qp_bi_min_max, QP_BI_FASTKW, QP_CMP_GT, qp_bi_doc_max },
    { "min", qp_bi_min_max, QP_BI_FASTKW, QP_CMP_LT, qp_bi_doc_min },
    { "next", qp_bi_next, QP_BI_FAST, 0, qp_bi_doc_next },
    { "anext", qp_bi_anext, QP_BI_FAST, 0, qp_bi_doc_builtin_anext },
    { "oct", qp_bi_to_base, QP_METH_O, 8, qp_bi_doc_builtin_oct },
    { "ord", qp_bi_ord, QP_METH_O, 0, qp_bi_doc_builtin_ord },
    { "pow", qp_bi_pow, QP_BI_FASTKW, 0, qp_bi_doc_builtin_pow },
    { "print", qp_bi_print, QP_BI_FASTKW, 0, qp_bi_doc_builtin_print },
    { "repr", qp_bi_repr, QP_METH_O, 0, qp_bi_doc_builtin_repr },
    { "round", qp_bi_round, QP_BI_FASTKW, 0, qp_bi_doc_builtin_round },
    { "setattr", qp_bi_setattr, QP_BI_FAST, 0, qp_bi_doc_builtin_setattr },
    { "sorted", qp_bi_sorted, QP_BI_FASTKW, 0, qp_bi_doc_builtin_sorted },
    { "sum", qp_bi_sum, QP_BI_FASTKW, 0, qp_bi_doc_builtin_sum },
    { "vars", qp_bi_vars, QP_BI_FAST, 0, qp_bi_doc_vars },
    { "open", qp_bi_open, QP_BI_FASTKW, 0,
      "open(file, mode='r', buffering=-1, encoding=None, errors=None, newline=None, "
      "closefd=True, opener=None)\n--\n\nOpen file and return a stream.  Raise OSError upon failure." },
    { NULL }
};

/* dict[name] = value (value borrowed) */
static int qp_bi_dict_set(JSContext *ctx, JSValueConst dict, const char *name, JSValueConst value)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    int r;
    if (atom == JS_ATOM_NULL)
        return -1;
    r = qp_dict_set_item_atom(ctx, dict, atom, value);
    JS_FreeAtom(ctx, atom);
    return r;
}

/* Module exec function: constants, types, exceptions and the module
   attributes that qp_module_from_def does not set. */
static int qp_bi_exec_module(JSContext *ctx, JSValueConst mod)
{
    QPState *st = QP_STATE(ctx);
    JSValueConst dict = qp_module_get_dict(mod);
    JSValue v;
    int i;
    static const struct { const char *name; int id; } type_names[] = {
        { "bool", QP_T_bool }, { "memoryview", QP_T_memoryview },
        { "bytearray", QP_T_bytearray }, { "bytes", QP_T_bytes },
        { "classmethod", QP_T_classmethod }, { "complex", QP_T_complex },
        { "dict", QP_T_dict }, { "enumerate", QP_T_enumerate }, { "filter", QP_T_filter },
        { "float", QP_T_float }, { "frozendict", QP_T_frozendict },
        { "frozenset", QP_T_frozenset }, { "property", QP_T_property }, { "int", QP_T_int },
        { "list", QP_T_list }, { "map", QP_T_map }, { "object", QP_T_object },
        { "range", QP_T_range }, { "reversed", QP_T_reversed }, { "sentinel", QP_T_sentinel },
        { "set", QP_T_set }, { "slice", QP_T_slice }, { "staticmethod", QP_T_staticmethod },
        { "str", QP_T_str }, { "super", QP_T_super }, { "tuple", QP_T_tuple },
        { "type", QP_T_type }, { "zip", QP_T_zip },
    };
    static const char *const exc_names[] = {
#define QP_BI_EXC_NAME(n, base) #n,
        QP_EXCEPTIONS(QP_BI_EXC_NAME)
#undef QP_BI_EXC_NAME
    };

    v = qp_str_from_cstr(ctx, "");
    if (JS_IsException(v) || qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __package__), v) < 0)
        goto fail_v;
    JS_FreeValue(ctx, v);
    if (qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __loader__), JS_UNDEFINED) < 0 ||
        qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __spec__), JS_UNDEFINED) < 0)
        goto fail;

    if (qp_bi_dict_set(ctx, dict, "None", JS_UNDEFINED) < 0 ||
        qp_bi_dict_set(ctx, dict, "Ellipsis", st->ellipsis) < 0 ||
        qp_bi_dict_set(ctx, dict, "NotImplemented", st->not_implemented) < 0 ||
        qp_bi_dict_set(ctx, dict, "False", JS_FALSE) < 0 ||
        qp_bi_dict_set(ctx, dict, "True", JS_TRUE) < 0)
        goto fail;
    for (i = 0; i < (int)countof(type_names); i++) {
        if (qp_bi_dict_set(ctx, dict, type_names[i].name,
                           QP_TYPE_VALUE(st->types[type_names[i].id])) < 0)
            goto fail;
    }
    if (qp_dict_set_item_atom(ctx, dict, QP_ATOM(ctx, __debug__),
                              qp_bool_new(st->config.optimize == 0)) < 0)
        goto fail;

    /* exception classes (_PyBuiltins_AddExceptions); js.JsError lives in
       the js module, not in builtins */
    for (i = 0; i < QP_EXC__COUNT; i++) {
        if (i == QP_EXC_JsError)
            continue;
        if (qp_bi_dict_set(ctx, dict, exc_names[i], QP_TYPE_VALUE(st->exc_types[i])) < 0)
            goto fail;
    }
    if (qp_bi_dict_set(ctx, dict, "EnvironmentError", QP_TYPE_VALUE(QP_EXC(ctx, OSError))) < 0 ||
        qp_bi_dict_set(ctx, dict, "IOError", QP_TYPE_VALUE(QP_EXC(ctx, OSError))) < 0)
        goto fail;

    return 0;

fail_v:
    JS_FreeValue(ctx, v);
fail:
    return -1;
}

static const QPModuleDef qp_moddef_builtins = {
    "builtins", qp_bi_doc_builtin, qp_builtin_methods, qp_bi_exec_module,
};

/* Ready the k iterator types, create the builtins module, then sys
   (qp_init_sys registers both in sys.modules). */
static int qp_init_builtins(JSContext *ctx)
{
    QPState *st = QP_STATE(ctx);
    JSValue mod, v;
    int i;
    static const struct { int id; const QPTypeSpec *spec; } own_types[] = {
        { QP_T_map, &qp_map_spec },
        { QP_T_filter, &qp_filter_spec },
        { QP_T_zip, &qp_zip_spec },
        { QP_T_enumerate, &qp_enum_spec },
        { QP_T_reversed, &qp_reversed_spec },
        { QP_T_sentinel, &qp_sentinel_spec },
        { QP_T_anext_awaitable, &qp_anext_spec },
    };

    for (i = 0; i < (int)countof(own_types); i++) {
        if (qp_type_ready(ctx, st->types[own_types[i].id], own_types[i].spec) < 0)
            return -1;
    }
    mod = qp_module_from_def(ctx, &qp_moddef_builtins);
    if (JS_IsException(mod))
        return -1;
    st->builtins_module = mod;
    st->builtins_dict = JS_DupValue(ctx, qp_module_get_dict(mod));
    /* fast path of IMPORT_NAME: the original builtins.__import__ */
    if (qp_dict_get_item_atom(ctx, st->builtins_dict, QP_ATOM(ctx, __import__), &v) <= 0)
        return -1;
    JS_FreeValue(ctx, st->import_func);
    st->import_func = v;
    return qp_init_sys(ctx);
}
