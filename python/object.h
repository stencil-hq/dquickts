/*
 * dquickts Python, milestone M2: object model contract.
 *
 * Shared by every M2 unit.  Declarations and data-layout contracts only; the
 * owning unit of every function is given by the section it is declared in
 * (see doc "m2-design.md", FILE OWNERSHIP MAP).  Slot names and signatures,
 * flag values and the argument conventions are modelled on CPython 3.15
 * Include/object.h, Include/cpython/object.h, Include/methodobject.h,
 * Include/descrobject.h, Include/cpython/code.h (PSF-2.0).
 *
 * This header is included twice by quickjs.c:
 *  1. inside the JS_CLASS_* enum with QP_CLASS defined, to append the
 *     Python class ids (and once more for the class definition table);
 *  2. normally, right after `enum OPCodeEnum` (before the first function
 *     prototype), so that JS_CallInternal and the bytecode walkers see the
 *     declarations.  All python/ .c files are included at the end of
 *     quickjs.c, so every function here is `static` and may be defined
 *     (or `static inline`) later in the translation unit.
 *
 * ===================== CONVENTIONS (every unit) ===========================
 * - References: arguments are BORROWED (JSValueConst) unless the comment says
 *   "consumes"/"steals"; returned JSValues are NEW references unless the
 *   comment says "borrowed".
 * - Errors: functions returning JSValue return JS_EXCEPTION with a pending
 *   exception; int functions return -1; Py_ssize_t functions return -1;
 *   Py_hash_t functions return -1.  Pending exceptions are Python exception
 *   objects (JS_CLASS_PY_EXCEPTION) thrown with JS_Throw(); other thrown
 *   values are converted by qp_exc_normalize() when Python code sees them.
 * - QP_NULL (= JS_UNINITIALIZED) is CPython's NULL: "absent value without an
 *   error" (iternext exhaustion, optional argument missing, empty slot,
 *   unbound local, stack NULL of push_null / load_method).  It is never a
 *   Python value.
 * - None is JS_UNDEFINED; JS null is also None for `is`/`==`/hash (D3).
 * - int is JS_TAG_INT | JS_TAG_SHORT_BIG_INT | JS_TAG_BIG_INT.  Every int
 *   PRODUCED by Python code is canonical: fits int32 -> JS_TAG_INT, else
 *   fits a short bigint -> JS_TAG_SHORT_BIG_INT, else JS_TAG_BIG_INT
 *   (qp_long_normalize).  Consumers accept all three tags for any value.
 * - float is JS_TAG_FLOAT64 created with __JS_NewFloat64() (qp_float_new);
 *   NEVER JS_NewFloat64(), which folds integral doubles into ints.
 * - bool is JS_TAG_BOOL; str is JS_TAG_STRING or JS_TAG_STRING_ROPE.
 * - Attribute / variable / keyword names are JSAtoms.
 * - Memory: js_malloc/js_free (context), js_malloc_rt/js_free_rt (runtime),
 *   qp_sys_malloc/qp_sys_free (compat.h) for context-free buffers; raw
 *   malloc/free do not compile in quickjs.c.
 * - Every new static symbol is qp_* / QP* / _Py* (CPython public Py* names
 *   are reserved for the C-API layer, see m1-contract "Namespace rule").
 * - Edits to this file after the design: append-only lines in the block
 *   labelled with your unit (QP_ATOMS, type lists); anything else through
 *   Main.
 */

#ifdef QP_CLASS
/* ======================= JS class ids (D4) ================================
 * QP_CLASS(id, finalizer, gc_mark): appended to the JS_CLASS_* enum as
 * JS_CLASS_<id>, and to the runtime class table with class name
 * JS_ATOM_Object, the given finalizer and gc_mark (NULL = none).  Python
 * classes register NO JSClass 'call' handler: JS_CallInternal dispatches
 * class ids in [JS_CLASS_PY_FIRST, JS_CLASS_PY_LAST] to qp_call_object()
 * (unit a).  Type of an instance = its JS prototype when that is a
 * JS_CLASS_PY_TYPE object (see qp_type_of), so one class id may serve many
 * Python types with the same payload layout.  The owning unit implements
 * the finalizer and gc_mark with exactly these names. */
/*       id                 finalizer                 gc_mark                  owner */
QP_CLASS(PY_TYPE,           qp_type_finalizer,        qp_type_mark)         /* h: u.opaque = QPType* */
QP_CLASS(PY_OBJECT,         qp_instance_finalizer,    qp_instance_mark)     /* h: u.opaque = QPInstance* or NULL */
QP_CLASS(PY_BOXED,          qp_boxed_finalizer,       qp_boxed_mark)        /* h: u.object_data = int/float/str value of a subclass instance */
QP_CLASS(PY_CFUNCTION,      qp_cfunction_finalizer,   qp_cfunction_mark)    /* h: QPCFunctionObject* (builtin_function_or_method) */
QP_CLASS(PY_DESCR,          qp_descr_finalizer,       qp_descr_mark)        /* h: QPDescr* (method/classmethod/getset/member/wrapper descriptors, method-wrapper) */
QP_CLASS(PY_CLASSMETHOD,    qp_funcwrap_finalizer,    qp_funcwrap_mark)     /* h: QPFuncWrap* */
QP_CLASS(PY_STATICMETHOD,   qp_funcwrap_finalizer,    qp_funcwrap_mark)     /* h: QPFuncWrap* */
QP_CLASS(PY_PROPERTY,       qp_property_finalizer,    qp_property_mark)     /* h: private payload */
QP_CLASS(PY_SUPER,          qp_super_finalizer,       qp_super_mark)        /* h: private payload */
QP_CLASS(PY_FUNC_DATA,      qp_funcdata_finalizer,    qp_funcdata_mark)     /* a: QPFuncData*, hidden companion in u.func.home_object of Python functions */
QP_CLASS(PY_METHOD,         qp_method_finalizer,      qp_method_mark)       /* a: QPMethodObject* (bound method) */
QP_CLASS(PY_CELL,           qp_cell_finalizer,        qp_cell_mark)         /* a: QPCell* */
QP_CLASS(PY_CODE,           qp_code_finalizer,        qp_code_mark)         /* a: private payload (bytecode ref) */
QP_CLASS(PY_FRAME,          qp_frame_finalizer,       qp_frame_mark)        /* a: private payload */
QP_CLASS(PY_COMPLEX,        qp_complex_finalizer,     NULL)                 /* c: QPComplex* */
QP_CLASS(PY_TUPLE,          qp_tuple_finalizer,       qp_tuple_mark)        /* e: QPTuple* */
QP_CLASS(PY_RANGE,          qp_range_finalizer,       qp_range_mark)        /* e: private payload */
QP_CLASS(PY_SLICE,          qp_slice_finalizer,       qp_slice_mark)        /* e: private payload */
QP_CLASS(PY_ITER,           qp_iter_finalizer,        qp_iter_mark)         /* e: private payload (sequence/callable/list/tuple/range/str/bytes iterators) */
QP_CLASS(PY_DICT,           qp_dict_finalizer,        qp_dict_mark)         /* f: QPDict* */
QP_CLASS(PY_FROZENDICT,     qp_dict_finalizer,        qp_dict_mark)         /* f: QPDict* */
QP_CLASS(PY_SET,            qp_set_finalizer,         qp_set_mark)          /* f: QPSet* */
QP_CLASS(PY_FROZENSET,      qp_set_finalizer,         qp_set_mark)          /* f: QPSet* */
QP_CLASS(PY_DICT_ITER,      qp_dictiter_finalizer,    qp_dictiter_mark)     /* f: private payload (dict and set iterators) */
QP_CLASS(PY_DICT_VIEW,      qp_dictview_finalizer,    qp_dictview_mark)     /* f: private payload (dict views, mappingproxy) */
QP_CLASS(PY_BYTES,          qp_bytes_finalizer,       qp_bytes_mark)        /* g: private payload over an immutable ArrayBuffer */
QP_CLASS(PY_BYTEARRAY,      qp_bytes_finalizer,       qp_bytes_mark)        /* g: private payload over a resizable ArrayBuffer */
QP_CLASS(PY_EXCEPTION,      qp_exc_finalizer,         qp_exc_mark)          /* i: private payload */
QP_CLASS(PY_TRACEBACK,      qp_tb_finalizer,          qp_tb_mark)           /* i: private payload */
QP_CLASS(PY_GENERATOR,      qp_gen_finalizer,         qp_gen_mark)          /* j: private payload */
QP_CLASS(PY_COROUTINE,      qp_gen_finalizer,         qp_gen_mark)          /* j: private payload */
QP_CLASS(PY_ASYNC_GEN,      qp_gen_finalizer,         qp_gen_mark)          /* j: private payload */
QP_CLASS(PY_GEN_AUX,        qp_genaux_finalizer,      qp_genaux_mark)       /* j: coroutine_wrapper, asend, athrow, async_generator_wrapped_value */
QP_CLASS(PY_MODULE,         qp_module_finalizer,      qp_module_mark)       /* l: private payload */
QP_CLASS(PY_NATIVE,         NULL,                     NULL)                 /* D5 (M5): JS companion of a C-extension object; unused in M2 */
#else /* !QP_CLASS */

#ifndef QP_OBJECT_H
#define QP_OBJECT_H

#include "compat.h"
#include "dictcore.h"
#include "stringlib.h"
#include "opcode.h"

#define JS_CLASS_PY_FIRST JS_CLASS_PY_TYPE
#define JS_CLASS_PY_LAST  JS_CLASS_PY_NATIVE
#define QP_IS_PY_CLASS(class_id) \
    ((unsigned)((class_id) - JS_CLASS_PY_FIRST) <= (unsigned)(JS_CLASS_PY_LAST - JS_CLASS_PY_FIRST))

struct JSFunctionDef;
struct JSParseState;
struct symtable;

typedef struct QPType QPType;
typedef struct QPState QPState;

/* ========================================================================= */
/* Basic values and call conventions                                          */
/* ========================================================================= */

#define QP_NULL JS_UNINITIALIZED
#define QP_IS_NULL(v) (JS_VALUE_GET_TAG(v) == JS_TAG_UNINITIALIZED)

/* Vectorcall (CPython Include/cpython/abstract.h): nargsf = positional
   count | QP_VECTORCALL_ARGUMENTS_OFFSET.  With the flag, the callee may
   temporarily overwrite args[-1] (bound methods prepend self without
   copying) and must restore it before returning. */
#define QP_VECTORCALL_ARGUMENTS_OFFSET ((size_t)1 << (8 * sizeof(size_t) - 1))
#define qp_nargs(nargsf) ((Py_ssize_t)((nargsf) & ~QP_VECTORCALL_ARGUMENTS_OFFSET))

/* JS_CallInternal flags added for Python (bits 0-4 are used by quickjs.c).
   JS_CALL_FLAG_PY_KW: argv = positional values then keyword values;
   argc counts both; new_target holds the kwnames tuple (exact str items,
   atom strings when built by codegen).  Python-aware callees (bytecode
   functions with is_py, class ids QP_IS_PY_CLASS) bind them; any other JS
   callee receives argc - len(kwnames) positional values plus ONE trailing
   plain object {name: value} and no flag.
   JS_CALL_FLAG_PY_ARGS_OFFSET: argv[-1] is a writable scratch slot (set by
   OP_call*, OP_py call subops and qp_vectorcall with
   QP_VECTORCALL_ARGUMENTS_OFFSET). */
#define JS_CALL_FLAG_PY_KW          (1 << 5)
#define JS_CALL_FLAG_PY_ARGS_OFFSET (1 << 6)

/* am_send results (CPython PySendResult) */
#define QP_SEND_RETURN 0
#define QP_SEND_ERROR  (-1)
#define QP_SEND_NEXT   1

/* code flags (CPython Include/cpython/code.h).  CO_FUTURE_* are in
   symtable.h. */
#ifndef CO_OPTIMIZED
#define CO_OPTIMIZED            0x0001
#define CO_NEWLOCALS            0x0002 /* absent: hidden argument 0 is the locals mapping */
#define CO_VARARGS              0x0004
#define CO_VARKEYWORDS          0x0008
#define CO_NESTED               0x0010
#define CO_GENERATOR            0x0020
#define CO_COROUTINE            0x0080
#define CO_ITERABLE_COROUTINE   0x0100
#define CO_ASYNC_GENERATOR      0x0200
#define CO_HAS_DOCSTRING        0x4000000
#define CO_METHOD               0x8000000
#endif

/* type flags (subset of Py_TPFLAGS_*, same values) */
#define QP_TPFLAGS_HAS_DEL              (1UL << 0)  /* the type (or a base) defines __del__ */
#define QP_TPFLAGS_MANAGED_DICT         (1UL << 4)  /* instances have a __dict__ */
#define QP_TPFLAGS_SEQUENCE             (1UL << 5)  /* match: sequence pattern */
#define QP_TPFLAGS_MAPPING              (1UL << 6)  /* match: mapping pattern */
#define QP_TPFLAGS_DISALLOW_INSTANTIATION (1UL << 7)
#define QP_TPFLAGS_IMMUTABLETYPE        (1UL << 8)
#define QP_TPFLAGS_HEAPTYPE             (1UL << 9)
#define QP_TPFLAGS_BASETYPE             (1UL << 10)
#define QP_TPFLAGS_READY                (1UL << 12)
#define QP_TPFLAGS_READYING             (1UL << 13)
#define QP_TPFLAGS_METHOD_DESCRIPTOR    (1UL << 17) /* load_method may skip binding */
#define QP_TPFLAGS_VALID_VERSION_TAG    (1UL << 19)
#define QP_TPFLAGS_IS_ABSTRACT          (1UL << 20)
#define QP_TPFLAGS_MATCH_SELF           (1UL << 22) /* class pattern C(x) matches the subject itself */
#define QP_TPFLAGS_LONG_SUBCLASS        (1UL << 24)
#define QP_TPFLAGS_LIST_SUBCLASS        (1UL << 25)
#define QP_TPFLAGS_TUPLE_SUBCLASS       (1UL << 26)
#define QP_TPFLAGS_BYTES_SUBCLASS       (1UL << 27)
#define QP_TPFLAGS_UNICODE_SUBCLASS     (1UL << 28)
#define QP_TPFLAGS_DICT_SUBCLASS        (1UL << 29)
#define QP_TPFLAGS_BASE_EXC_SUBCLASS    (1UL << 30)
#define QP_TPFLAGS_TYPE_SUBCLASS        (1UL << 31)
#define QP_TPFLAGS_WEAKREF              ((uint64_t)1 << 40) /* instances support weak references (CPython tp_weaklistoffset != 0); builtin specs set it, heap types per type_new add_weak rules (M2TypeObj) */
#define QP_TPFLAGS_DEFAULT              0

/* ========================================================================= */
/* Builtin types, exceptions and interned atoms                               */
/* ========================================================================= */

/* QP_BUILTIN_TYPES(X): X(id, owner).  Enumerators QP_T_<id>; the QPType of
   every entry exists (allocated, not ready) after phase 1 of qp_state_init
   and is readied by its owner's init function. */
#define QP_BUILTIN_TYPES(X)                                                  \
    /* h: typeobject.c, object.c, descrobject.c, abstract.c */              \
    X(object, h) X(type, h) X(super, h) X(property, h) X(classmethod, h)     \
    X(staticmethod, h) X(builtin_function_or_method, h)                      \
    X(method_descriptor, h) X(classmethod_descriptor, h)                     \
    X(getset_descriptor, h) X(member_descriptor, h)                          \
    X(wrapper_descriptor, h) X(method_wrapper, h) X(NoneType, h)             \
    X(NotImplementedType, h) X(JsObject, h)                                  \
    /* a: funcobject.c, codeobject.c, frameobject.c */                      \
    X(function, a) X(method, a) X(code, a) X(cell, a) X(frame, a)            \
    /* c: longobject.c, floatobject.c, complexobject.c */                   \
    X(int, c) X(bool, c) X(float, c) X(complex, c)                           \
    /* d: unicodeobject.c, templateobject.c */                              \
    X(str, d) X(Template, d) X(Interpolation, d)                             \
    X(TemplateIter, d) X(formatteriterator, d) X(fieldnameiterator, d)      \
    /* e: listobject.c, tupleobject.c, rangeobject.c, sliceobject.c, iterobject.c */ \
    X(list, e) X(tuple, e) X(range, e) X(slice, e) X(ellipsis, e)            \
    X(list_iterator, e) X(list_reverseiterator, e) X(tuple_iterator, e)      \
    X(range_iterator, e) X(longrange_iterator, e) X(str_ascii_iterator, e)   \
    X(str_iterator, e) X(bytes_iterator, e) X(bytearray_iterator, e)         \
    X(iterator, e) X(callable_iterator, e)                                   \
    /* f: dictobject.c, setobject.c */                                      \
    X(dict, f) X(frozendict, f) X(dict_keys, f) X(dict_values, f)            \
    X(dict_items, f) X(dict_keyiterator, f) X(dict_valueiterator, f)         \
    X(dict_itemiterator, f) X(dict_reversekeyiterator, f)                    \
    X(dict_reversevalueiterator, f) X(dict_reverseitemiterator, f)           \
    X(mappingproxy, f) X(set, f) X(frozenset, f) X(set_iterator, f)          \
    /* g: bytesobject.c, bytearrayobject.c, memoryobject.c */               \
    X(bytes, g) X(bytearray, g) X(memoryview, g)                             \
    /* i: exceptions.c, traceback.c (exception types: QP_EXCEPTIONS) */     \
    X(traceback, i)                                                          \
    /* j: genobject.c */                                                    \
    X(generator, j) X(coroutine, j) X(async_generator, j)                    \
    X(coroutine_wrapper, j) X(async_generator_asend, j)                      \
    X(async_generator_athrow, j) X(async_generator_wrapped_value, j)         \
    /* k: bltinmodule.c, sysmodule.c, typevarobject.c */                    \
    X(enumerate, k) X(reversed, k) X(zip, k) X(map, k) X(filter, k)          \
    X(sentinel, k) X(TypeVar, k) X(ParamSpec, k) X(TypeVarTuple, k)          \
    X(ParamSpecArgs, k) X(ParamSpecKwargs, k) X(TypeAliasType, k)            \
    X(Generic, k) X(GenericAlias, k) X(Union, k) X(StdPrinter, k)            \
    X(anext_awaitable, k)                                                    \
    X(NoDefaultType, k) X(ConstEvaluator, k) X(generic_alias_iterator, k)    \
    X(SimpleNamespace, k)                                                    \
    /* l: moduleobject.c, import.c */                                       \
    X(module, l) X(lazy_import, l)

typedef enum QPTypeId {
#define QP_TYPE_ENUM(id, owner) QP_T_ ## id,
    QP_BUILTIN_TYPES(QP_TYPE_ENUM)
#undef QP_TYPE_ENUM
    QP_T__COUNT,
} QPTypeId;

/* QP_EXCEPTIONS(X): X(name, base) in CPython 3.15 Objects/exceptions.c
   order of definition (bases first).  Enumerators QP_EXC_<name>; owner (i).
   ExceptionGroup additionally inherits Exception (two bases), JsError is
   `js.JsError(Exception)` (D3), EnvironmentError/IOError are aliases of
   OSError in builtins (no entry). */
#define QP_EXCEPTIONS(X)                                                     \
    X(BaseException, BaseException)                                          \
    X(Exception, BaseException)                                              \
    X(TypeError, Exception)                                                  \
    X(StopAsyncIteration, Exception)                                         \
    X(StopIteration, Exception)                                              \
    X(GeneratorExit, BaseException)                                          \
    X(SystemExit, BaseException)                                             \
    X(KeyboardInterrupt, BaseException)                                      \
    X(BaseExceptionGroup, BaseException)                                     \
    X(ExceptionGroup, BaseExceptionGroup)                                    \
    X(ImportError, Exception)                                                \
    X(ImportCycleError, ImportError)                                         \
    X(ModuleNotFoundError, ImportError)                                      \
    X(OSError, Exception)                                                    \
    X(BlockingIOError, OSError)                                              \
    X(ConnectionError, OSError)                                              \
    X(ChildProcessError, OSError)                                            \
    X(BrokenPipeError, ConnectionError)                                      \
    X(ConnectionAbortedError, ConnectionError)                               \
    X(ConnectionRefusedError, ConnectionError)                               \
    X(ConnectionResetError, ConnectionError)                                 \
    X(FileExistsError, OSError)                                              \
    X(FileNotFoundError, OSError)                                            \
    X(IsADirectoryError, OSError)                                            \
    X(NotADirectoryError, OSError)                                           \
    X(InterruptedError, OSError)                                             \
    X(PermissionError, OSError)                                              \
    X(ProcessLookupError, OSError)                                           \
    X(TimeoutError, OSError)                                                 \
    X(EOFError, Exception)                                                   \
    X(RuntimeError, Exception)                                               \
    X(RecursionError, RuntimeError)                                          \
    X(PythonFinalizationError, RuntimeError)                                 \
    X(NotImplementedError, RuntimeError)                                     \
    X(NameError, Exception)                                                  \
    X(UnboundLocalError, NameError)                                          \
    X(AttributeError, Exception)                                             \
    X(SyntaxError, Exception)                                                \
    X(IndentationError, SyntaxError)                                         \
    X(TabError, IndentationError)                                            \
    X(_IncompleteInputError, SyntaxError)                                    \
    X(LookupError, Exception)                                                \
    X(IndexError, LookupError)                                               \
    X(KeyError, LookupError)                                                 \
    X(ValueError, Exception)                                                 \
    X(UnicodeError, ValueError)                                              \
    X(UnicodeEncodeError, UnicodeError)                                      \
    X(UnicodeDecodeError, UnicodeError)                                      \
    X(UnicodeTranslateError, UnicodeError)                                   \
    X(AssertionError, Exception)                                             \
    X(ArithmeticError, Exception)                                            \
    X(FloatingPointError, ArithmeticError)                                   \
    X(OverflowError, ArithmeticError)                                        \
    X(ZeroDivisionError, ArithmeticError)                                    \
    X(SystemError, Exception)                                                \
    X(ReferenceError, Exception)                                             \
    X(MemoryError, Exception)                                                \
    X(BufferError, Exception)                                                \
    X(Warning, Exception)                                                    \
    X(UserWarning, Warning)                                                  \
    X(EncodingWarning, Warning)                                              \
    X(DeprecationWarning, Warning)                                           \
    X(PendingDeprecationWarning, Warning)                                    \
    X(SyntaxWarning, Warning)                                                \
    X(RuntimeWarning, Warning)                                               \
    X(FutureWarning, Warning)                                                \
    X(ImportWarning, Warning)                                                \
    X(UnicodeWarning, Warning)                                               \
    X(BytesWarning, Warning)                                                 \
    X(ResourceWarning, Warning)                                              \
    X(JsError, Exception)

typedef enum QPExcId {
#define QP_EXC_ENUM(name, base) QP_EXC_ ## name,
    QP_EXCEPTIONS(QP_EXC_ENUM)
#undef QP_EXC_ENUM
    QP_EXC__COUNT,
} QPExcId;

/* Interned atoms created by qp_state_init (unit a) and freed with the
   state: QP_ATOM(ctx, QP_ATOM_<id>) is the atom of the string <id>.
   X(id) for identifiers; XS(id, "string") when the text is not a C
   identifier.  Units append lines to their own block only. */
#define QP_ATOMS(X, XS)                                                      \
    /* shared dunders (h) */                                                \
    X(__init__) X(__new__) X(__call__) X(__class__) X(__dict__) X(__doc__)    \
    X(__module__) X(__name__) X(__qualname__) X(__slots__) X(__weakref__)     \
    X(__getattr__) X(__getattribute__) X(__setattr__) X(__delattr__)          \
    X(__get__) X(__set__) X(__delete__) X(__set_name__) X(__init_subclass__) \
    X(__class_getitem__) X(__mro_entries__) X(__prepare__) X(__classcell__)  \
    X(__classdictcell__) X(__classdict__) X(__static_attributes__)           \
    X(__firstlineno__) X(__annotations__) X(__annotate__) X(__annotate_func__) \
    X(__annotations_cache__) X(__type_params__) X(__orig_bases__)            \
    X(__bases__) X(__base__) X(__mro__) X(mro) X(__abstractmethods__)        \
    X(__instancecheck__) X(__subclasscheck__) X(__subclasshook__)            \
    X(__repr__) X(__str__) X(__format__) X(__hash__) X(__bool__) X(__len__)  \
    X(__length_hint__) X(__iter__) X(__next__) X(__reversed__)               \
    X(__contains__) X(__getitem__) X(__setitem__) X(__delitem__)             \
    X(__missing__) X(__index__) X(__int__) X(__float__) X(__complex__)       \
    X(__trunc__) X(__floor__) X(__ceil__) X(__round__) X(__abs__)            \
    X(__neg__) X(__pos__) X(__invert__)                                      \
    X(__eq__) X(__ne__) X(__lt__) X(__le__) X(__gt__) X(__ge__)              \
    X(__add__) X(__radd__) X(__iadd__) X(__sub__) X(__rsub__) X(__isub__)    \
    X(__mul__) X(__rmul__) X(__imul__) X(__matmul__) X(__rmatmul__)          \
    X(__imatmul__) X(__truediv__) X(__rtruediv__) X(__itruediv__)            \
    X(__floordiv__) X(__rfloordiv__) X(__ifloordiv__) X(__mod__) X(__rmod__) \
    X(__imod__) X(__divmod__) X(__rdivmod__) X(__pow__) X(__rpow__)          \
    X(__ipow__) X(__lshift__) X(__rlshift__) X(__ilshift__) X(__rshift__)    \
    X(__rrshift__) X(__irshift__) X(__and__) X(__rand__) X(__iand__)         \
    X(__or__) X(__ror__) X(__ior__) X(__xor__) X(__rxor__) X(__ixor__)       \
    X(__enter__) X(__exit__) X(__aenter__) X(__aexit__) X(__await__)         \
    X(__aiter__) X(__anext__) X(__del__) X(__dir__) X(__sizeof__)            \
    X(__reduce__) X(__reduce_ex__) X(__getstate__) X(__copy__)               \
    X(__deepcopy__) X(__fspath__) X(__bytes__) X(__buffer__)                 \
    X(__release_buffer__) X(__match_args__) X(__self__) X(__func__)          \
    X(__wrapped__) X(__isabstractmethod__) X(__objclass__)                   \
    X(__text_signature__) X(__weakrefoffset__) X(__dictoffset__)             \
    /* a: functions, code, frames, interpreter */                           \
    X(__defaults__) X(__kwdefaults__) X(__code__) X(__globals__)             \
    X(__closure__) X(__builtins__) X(__build_class__) X(__debug__)           \
    X(__conditional_annotations__) XS(dot_format, ".format")                 \
    XS(anon_lambda, "<lambda>") XS(anon_genexpr, "<genexpr>")                \
    XS(anon_listcomp, "<listcomp>") XS(anon_setcomp, "<setcomp>")            \
    XS(anon_dictcomp, "<dictcomp>") XS(anon_module, "<module>")              \
    XS(dot_locals, "<locals>")                                               \
    /* i: exceptions */                                                     \
    X(__traceback__) X(__cause__) X(__context__) X(__suppress_context__)    \
    X(__notes__) X(args) X(value) X(errno) X(strerror) X(filename)           \
    X(filename2) X(lineno) X(offset) X(end_lineno) X(end_offset) X(text)     \
    X(msg) X(name) X(obj) X(path) X(code) X(exceptions) X(message)          \
    X(add_note) X(with_traceback)                                            \
    X(derive) X(split) X(name_from)                                          \
    /* j: generators */                                                     \
    X(send) X(throw) X(close)                                                \
    /* k: builtins, sys */                                                  \
    X(__import__) X(displayhook) X(excepthook) X(unraisablehook)             \
    X(stdout) X(stderr) X(stdin) X(write) X(flush) X(metaclass) X(keys)      \
    X(items) X(values) X(get) X(copy) X(_)                                   \
    X(__parameters__) X(__typing_subst__) X(__typing_prepare_subst__)        \
    X(__typing_is_unpacked_typevartuple__) X(__typing_unpacked_tuple_args__) \
    X(__orig_class__) X(__origin__) X(__args__) X(__unpacked__) X(next)      \
    X(iter) X(_type_check) X(fileno) X(__stdout__) X(__stderr__)             \
    X(__displayhook__) X(__excepthook__) X(breakpointhook) X(exit)           \
    X(sort) X(readline) X(open) X(builtins)                                  \
    /* l: modules, import */                                                \
    X(__spec__) X(__loader__) X(__package__) X(__path__) X(__file__)         \
    X(__all__) X(__main__) X(__cached__) X(__lazy_modules__) X(modules)      \
    X(meta_path) X(argv) X(has_location) X(origin) X(parent)                \
    X(_initializing) X(_uninitialized_submodules) X(loader)                  \
    X(ps1) X(ps2) X(last_exc) X(last_type) X(last_value) X(last_traceback)   \
    X(__lazy_import__) X(encoding) X(resolve) X(_path)                       \
    X(stdlib_module_names) X(_run_module_as_main) X(_handle_fromlist)

typedef enum QPAtomId {
#define QP_ATOM_ENUM(id) QP_ATOM_ ## id,
#define QP_ATOM_ENUM_S(id, str) QP_ATOM_ ## id,
    QP_ATOMS(QP_ATOM_ENUM, QP_ATOM_ENUM_S)
#undef QP_ATOM_ENUM
#undef QP_ATOM_ENUM_S
    QP_ATOM__COUNT,
} QPAtomId;

/* ========================================================================= */
/* Type objects and slots                                                     */
/* ========================================================================= */

/* Slot signatures (CPython names in comments).  `self`/`a` are borrowed. */
typedef JSValue QPUnaryFunc(JSContext *ctx, JSValueConst a);                         /* unaryfunc; reprfunc; getiterfunc */
typedef JSValue QPBinaryFunc(JSContext *ctx, JSValueConst a, JSValueConst b);        /* binaryfunc */
typedef JSValue QPTernaryFunc(JSContext *ctx, JSValueConst a, JSValueConst b, JSValueConst c); /* ternaryfunc (pow; c may be None) */
typedef int QPInquiry(JSContext *ctx, JSValueConst a);                               /* inquiry (nb_bool): 1/0/-1 */
typedef Py_ssize_t QPLenFunc(JSContext *ctx, JSValueConst a);                        /* lenfunc: >= 0 or -1 */
typedef JSValue QPSsizeArgFunc(JSContext *ctx, JSValueConst a, Py_ssize_t i);        /* ssizeargfunc (sq_item: i already adjusted) */
typedef int QPSsizeObjArgProc(JSContext *ctx, JSValueConst a, Py_ssize_t i, JSValueConst v); /* sq_ass_item; v == QP_NULL deletes */
typedef int QPObjObjProc(JSContext *ctx, JSValueConst a, JSValueConst b);            /* objobjproc (sq_contains): 1/0/-1 */
typedef int QPObjObjArgProc(JSContext *ctx, JSValueConst a, JSValueConst k, JSValueConst v); /* mp_ass_subscript; v == QP_NULL deletes */
typedef JSValue QPGetAttroFunc(JSContext *ctx, JSValueConst obj, JSAtom name);       /* getattrofunc */
typedef int QPSetAttroFunc(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v); /* setattrofunc; v == QP_NULL deletes */
typedef Py_hash_t QPHashFunc(JSContext *ctx, JSValueConst a);                        /* hashfunc */
typedef JSValue QPRichCmpFunc(JSContext *ctx, JSValueConst a, JSValueConst b, int op); /* richcmpfunc, op = QP_CMP_* */
typedef JSValue QPIterNextFunc(JSContext *ctx, JSValueConst it);                     /* iternextfunc: QP_NULL = exhausted (no exception) */
/* virtual iteration (CPython 3.15 _tp_iteritem): 1 = *pitem set, 0 = index
   past the end, -1 = error.  Only for types whose length fits int32 when
   get_iter chooses virtual iteration. */
typedef int QPIterItemFunc(JSContext *ctx, JSValueConst seq, Py_ssize_t index, JSValue *pitem);
typedef JSValue QPDescrGetFunc(JSContext *ctx, JSValueConst descr, JSValueConst obj, JSValueConst type); /* obj == QP_NULL: class access */
typedef int QPDescrSetFunc(JSContext *ctx, JSValueConst descr, JSValueConst obj, JSValueConst v); /* v == QP_NULL deletes */
/* Calls use vectorcall: args[0..nargs) positional then len(kwnames)
   keyword values; kwnames is a tuple of str or QP_NULL. */
typedef JSValue QPVectorcallFunc(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                                 size_t nargsf, JSValueConst kwnames);               /* vectorcallfunc / tp_call */
typedef int QPInitProc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                       size_t nargsf, JSValueConst kwnames);                         /* initproc */
typedef JSValue QPNewFunc(JSContext *ctx, QPType *subtype, JSValueConst *args,
                          size_t nargsf, JSValueConst kwnames);                      /* newfunc */
typedef int QPSendFunc(JSContext *ctx, JSValueConst iter, JSValueConst arg, JSValue *presult); /* sendfunc: QP_SEND_* */

typedef struct QPBuffer QPBuffer;
typedef int QPGetBufferProc(JSContext *ctx, JSValueConst obj, QPBuffer *view, int flags);
typedef void QPReleaseBufferProc(JSContext *ctx, QPBuffer *view);

typedef struct QPNumberMethods {
    QPBinaryFunc *nb_add, *nb_subtract, *nb_multiply, *nb_remainder, *nb_divmod;
    QPTernaryFunc *nb_power;
    QPUnaryFunc *nb_negative, *nb_positive, *nb_absolute;
    QPInquiry *nb_bool;
    QPUnaryFunc *nb_invert;
    QPBinaryFunc *nb_lshift, *nb_rshift, *nb_and, *nb_xor, *nb_or;
    QPUnaryFunc *nb_int, *nb_float;
    QPBinaryFunc *nb_inplace_add, *nb_inplace_subtract, *nb_inplace_multiply,
        *nb_inplace_remainder;
    QPTernaryFunc *nb_inplace_power;
    QPBinaryFunc *nb_inplace_lshift, *nb_inplace_rshift, *nb_inplace_and,
        *nb_inplace_xor, *nb_inplace_or;
    QPBinaryFunc *nb_floor_divide, *nb_true_divide, *nb_inplace_floor_divide,
        *nb_inplace_true_divide;
    QPUnaryFunc *nb_index;
    QPBinaryFunc *nb_matrix_multiply, *nb_inplace_matrix_multiply;
} QPNumberMethods;

typedef struct QPSequenceMethods {
    QPLenFunc *sq_length;
    QPBinaryFunc *sq_concat;
    QPSsizeArgFunc *sq_repeat;
    QPSsizeArgFunc *sq_item;
    QPSsizeObjArgProc *sq_ass_item;
    QPObjObjProc *sq_contains;
    QPBinaryFunc *sq_inplace_concat;
    QPSsizeArgFunc *sq_inplace_repeat;
} QPSequenceMethods;

typedef struct QPMappingMethods {
    QPLenFunc *mp_length;
    QPBinaryFunc *mp_subscript;
    QPObjObjArgProc *mp_ass_subscript;
} QPMappingMethods;

typedef struct QPAsyncMethods {
    QPUnaryFunc *am_await, *am_aiter, *am_anext;
    QPSendFunc *am_send;
} QPAsyncMethods;

typedef struct QPBufferProcs {
    QPGetBufferProc *bf_getbuffer;
    QPReleaseBufferProc *bf_releasebuffer;
} QPBufferProcs;

/* Everything a type can implement in C.  Builtin types fill it in their
   QPTypeSpec; heap types get the generic slot_* dispatchers of typeobject.c
   for the dunders defined in their namespace (update_slot/fixup_slot
   semantics), and inherit the rest (inherit_slots). */
typedef struct QPTypeSlots {
    QPGetAttroFunc *tp_getattro;
    QPSetAttroFunc *tp_setattro;
    QPUnaryFunc *tp_repr;
    QPUnaryFunc *tp_str;
    QPHashFunc *tp_hash;            /* NULL after inherit = unhashable when __eq__ overridden */
    QPVectorcallFunc *tp_call;      /* calling instances */
    QPVectorcallFunc *tp_vectorcall;/* calling the TYPE object itself (fast int(x), str(x), ...); NULL = generic type_call */
    QPRichCmpFunc *tp_richcompare;
    QPUnaryFunc *tp_iter;
    QPIterNextFunc *tp_iternext;
    QPIterItemFunc *tp_iteritem;
    QPDescrGetFunc *tp_descr_get;
    QPDescrSetFunc *tp_descr_set;
    QPInitProc *tp_init;
    QPNewFunc *tp_new;
    QPNumberMethods nb;
    QPSequenceMethods sq;
    QPMappingMethods mp;
    QPAsyncMethods am;
    QPBufferProcs bf;
} QPTypeSlots;

/* Builtin method table entry (CPython PyMethodDef).  `meth` always has the
   vectorcall shape; the flags tell the descriptor/cfunction dispatcher which
   argument checks to make before calling, with CPython's messages:
   QP_METH_NOARGS "%U() takes no arguments (%zd given)",
   QP_METH_O "%U() takes exactly one argument (%zd given)",
   no QP_METH_KEYWORDS "%U() takes no keyword arguments". */
typedef JSValue QPCFunc(JSContext *ctx, JSValueConst self, JSValueConst *args,
                        int nargs, JSValueConst kwnames, int magic);
#define QP_METH_NOARGS   0x0001
#define QP_METH_O        0x0002
#define QP_METH_FASTCALL 0x0004 /* any number of positional arguments */
#define QP_METH_KEYWORDS 0x0008 /* with FASTCALL: keyword arguments accepted (kwnames may be non-NULL) */
#define QP_METH_CLASS    0x0010 /* classmethod_descriptor: self is the type */
#define QP_METH_STATIC   0x0020 /* staticmethod: self is QP_NULL */
#define QP_METH_COEXIST  0x0040 /* do not replace an existing slot wrapper */

typedef struct QPMethodDef {
    const char *name;
    QPCFunc *meth;
    uint16_t flags;     /* QP_METH_* */
    int32_t magic;      /* passed to meth (D5 (CapiB) R1: int32, C-API registry index) */
    const char *doc;    /* may start with the "name(sig)\n--\n\n" __text_signature__ block */
} QPMethodDef;

/* getset_descriptor (CPython PyGetSetDef); set == NULL: read-only */
typedef JSValue QPGetter(JSContext *ctx, JSValueConst self, int magic);
typedef int QPSetter(JSContext *ctx, JSValueConst self, JSValueConst value /* QP_NULL = delete */, int magic);
typedef struct QPGetSetDef {
    const char *name;
    QPGetter *get;
    QPSetter *set;
    int magic;
    const char *doc;
} QPGetSetDef;

/* member_descriptor over a QPInstance slot (PyMemberDef, T_OBJECT_EX) */
#define QP_MEMBER_READONLY  0x01
#define QP_MEMBER_NONE_IF_NULL 0x02  /* T_OBJECT: an empty slot reads as None instead of AttributeError */
typedef struct QPMemberDef {
    const char *name;
    uint16_t slot;      /* QPInstance.slots index */
    uint8_t flags;      /* QP_MEMBER_* */
    const char *doc;
} QPMemberDef;

/* Static description of a builtin type, given to qp_type_ready(). */
typedef struct QPTypeSpec {
    const char *name;           /* tp_name: "int" (builtins) or "module.Name" */
    const char *doc;            /* __doc__ (may hold a __text_signature__ block), NULL = None */
    uint64_t flags;             /* QP_TPFLAGS_* (READY etc. are set by qp_type_ready) */
    int base;                   /* QPTypeId of tp_base, -1 = object (only `object` itself has none) */
    int exc_base;               /* exception types only: QPExcId of tp_base, else -1 */
    uint16_t instance_class_id; /* JS class of instances (JS_CLASS_PY_OBJECT, JS_CLASS_ARRAY, ...) */
    uint16_t nslots;            /* QPInstance slots used by this type's own fields */
    QPTypeSlots slots;
    const QPMethodDef *methods; /* terminated by name == NULL; may be NULL */
    const QPGetSetDef *getsets;
    const QPMemberDef *members;
} QPTypeSpec;

/* The C side of a type object (payload of JS_CLASS_PY_TYPE). */
struct QPType {
    JSObject *obj;              /* the type object; the QPType is freed by its finalizer */
    JSContext *ctx;             /* realm that created it */
    const char *tp_name;        /* builtin: spec name; heap types: NULL */
    JSAtom name;                /* __name__ (owned) */
    JSValue qualname;           /* __qualname__ str */
    uint64_t flags;             /* QP_TPFLAGS_* */
    uint32_t version_tag;       /* 0 = none; unique per context, never reused (method cache, ICs) */
    uint16_t instance_class_id; /* JS class id of instances (solid layout) */
    uint16_t nslots;            /* QPInstance slots of instances incl. bases */
    QPType *base;               /* tp_base (solid base chain), NULL for object; counted via `bases` */
    JSValue bases;              /* tuple of type objects */
    JSValue mro;                /* tuple, JS_UNDEFINED until ready */
    JSValue dict;               /* PY_DICT namespace (type.__dict__ is a mappingproxy of it) */
    JSValue slot_names;         /* tuple of __slots__ names (mangled), or JS_UNDEFINED */
    QPType **subclasses;        /* weak back pointers for version invalidation; a type removes itself in its finalizer */
    int subclass_count, subclass_size;
    const QPTypeSpec *spec;     /* builtin types: the spec they were readied from, else NULL */
    QPTypeSlots slots;
};

#define QP_TYPE_VALUE(t) JS_MKPTR(JS_TAG_OBJECT, (t)->obj)   /* borrowed JSValue of a QPType */

/* Payload of JS_CLASS_PY_OBJECT instances that have slots (__slots__
   members, or builtin types that keep fixed JSValue fields there).  NULL
   when the type has no slot.  An unset slot holds QP_NULL.  Instance
   attributes of PY_OBJECT/PY_EXCEPTION/PY_BOXED/PY_TUPLE/... instances live
   in QuickJS shape properties (D4) until __dict__ is materialized; then
   JSObject.py_dict_mode is set and the dict is the hidden property
   QP_STATE(ctx)->atom_dict_slot (see unit h).  Instances whose class id
   has JS-visible own properties (JS_CLASS_ARRAY for list subclasses) are
   always in dict mode. */
typedef struct QPInstance {
    uint32_t nslots;
    JSValue slots[];
} QPInstance;

/* ========================================================================= */
/* Payload layouts shared between units (other payloads are private to the  */
/* owner and reached through the accessor functions below)                    */
/* ========================================================================= */

/* tuple (unit e).  Immutable once published; hash cached (3.14+). */
typedef struct QPTuple {
    Py_hash_t hash;             /* -1 = not computed */
    uint32_t len;
    JSValue items[];
} QPTuple;
#define QP_TUPLE(v)       ((QPTuple *)JS_VALUE_GET_OBJ(v)->u.opaque)
#define QP_TUPLE_SIZE(v)  ((Py_ssize_t)QP_TUPLE(v)->len)
#define QP_TUPLE_ITEMS(v) (QP_TUPLE(v)->items)   /* borrowed */

/* dict / frozendict (unit f).  `uid` is unique per dict for the lifetime of
   the context (never reused), so (uid, core.version) identifies a dict
   state for global inline caches. */
typedef struct QPDict {
    qp_dictcore core;
    uint64_t uid;
    Py_hash_t hash;             /* frozendict hash cache, -1 = not computed */
} QPDict;
#define QP_DICT(v) ((QPDict *)JS_VALUE_GET_OBJ(v)->u.opaque)

/* set / frozenset (unit f) */
typedef struct QPSet {
    qp_setcore core;
    Py_hash_t hash;             /* frozenset hash cache, -1 = not computed */
} QPSet;
#define QP_SET(v) ((QPSet *)JS_VALUE_GET_OBJ(v)->u.opaque)

/* complex (unit c) */
typedef struct QPComplex {
    double real, imag;
} QPComplex;

/* Python function companion (unit a): JSObject of class PY_FUNC_DATA stored
   in u.func.home_object of every Python function object (Python never uses
   JS `super`, so the slot is free; the existing bytecode-function mark and
   finalizer keep it alive and release it).  All JSValue fields are owned. */
typedef struct QPFuncData {
    JSValue globals;            /* __globals__ (PY_DICT) */
    JSValue builtins;           /* __builtins__ dict resolved at creation (CPython func_builtins) */
    JSValue name;               /* __name__ (str) */
    JSValue qualname;           /* __qualname__ (str) */
    JSValue module;             /* __module__ (globals['__name__'] at creation) */
    JSValue doc;                /* __doc__ */
    JSValue defaults;           /* tuple or None */
    JSValue kwdefaults;         /* dict or None */
    JSValue annotations;        /* dict or None (materialized __annotations__) */
    JSValue annotate;           /* __annotate__ callable or None (PEP 649) */
    JSValue type_params;        /* tuple (empty by default) */
    JSValue dict;               /* __dict__ or QP_NULL until first use */
    JSValue code_obj;           /* cached PY_CODE facade or QP_NULL */
    uint32_t version;           /* bumped when __code__/__defaults__/__kwdefaults__ change */
} QPFuncData;

/* bound method (unit a) */
typedef struct QPMethodObject {
    JSValue func;               /* __func__ */
    JSValue self;               /* __self__ */
} QPMethodObject;

/* cell (unit a): wraps a JSVarRef (counted GC reference).  A cell made by
   cell() owns a detached JSVarRef.  Empty = the referenced value is
   JS_UNINITIALIZED. */
typedef struct QPCell {
    struct JSVarRef *var_ref;
} QPCell;

/* builtin_function_or_method (unit h) */
typedef struct QPCFunctionObject {
    const QPMethodDef *ml;
    JSValue self;               /* __self__: module or bound object; QP_NULL = none */
    JSValue module;             /* __module__ (str or None) */
} QPCFunctionObject;

/* classmethod / staticmethod (unit h) */
typedef struct QPFuncWrap {
    JSValue callable;           /* __func__ */
    JSValue dict;               /* __dict__ or QP_NULL (functools.wraps copies attributes) */
} QPFuncWrap;

/* descriptors made by qp_type_ready / __slots__ (unit h) */
enum {
    QP_DESCR_METHOD,            /* method_descriptor */
    QP_DESCR_CLASSMETHOD,       /* classmethod_descriptor */
    QP_DESCR_GETSET,            /* getset_descriptor */
    QP_DESCR_MEMBER,            /* member_descriptor */
    QP_DESCR_WRAPPER,           /* wrapper_descriptor (slot wrapper, e.g. int.__add__) */
    QP_DESCR_METHOD_WRAPPER,    /* method-wrapper (bound slot wrapper, e.g. (1).__add__) */
};
typedef struct QPSlotDef QPSlotDef;  /* typeobject.c slotdefs entry (private to h) */
typedef struct QPDescr {
    uint8_t kind;               /* QP_DESCR_* */
    JSValue objclass;           /* __objclass__ type object */
    JSAtom name;                /* owned */
    JSValue self;               /* QP_DESCR_METHOD_WRAPPER only, else QP_NULL */
    union {
        const QPMethodDef *method;
        const QPGetSetDef *getset;
        struct {
            uint16_t slot;      /* QPInstance index (class PY_OBJECT) */
            uint8_t flags;      /* QP_MEMBER_* */
            JSAtom private_atom;/* hidden-property key for instances of other classes; owned */
            const QPGetSetDef *getset; /* D5 (CapiB) R2: C-API PyMemberDef wrapper (get/set/delete dispatch like a getset), NULL for slot members */
        } member;
        struct {
            const QPSlotDef *slot;
            void *wrapped;      /* the C slot function it wraps */
        } wrapper;
    } u;
} QPDescr;

/* buffer protocol (unit g; CPython Py_buffer, 1-D C-contiguous subset) */
struct QPBuffer {
    JSValue obj;                /* owned reference to the exporter */
    uint8_t *buf;
    Py_ssize_t len;             /* bytes */
    int readonly;
    Py_ssize_t itemsize;
    const char *format;         /* struct format, "B" for bytes */
};
#define QP_BUF_SIMPLE   0
#define QP_BUF_WRITABLE 0x0001

/* ---- code metadata attached to JSFunctionBytecode (unit a) -------------- */

/* Source location (CPython co_positions): 1-based lines, 0-based UTF-8
   byte columns, -1 = unknown. */
typedef struct QPLoc {
    int lineno, end_lineno, col, end_col;
} QPLoc;

/* Attribute inline cache (semantics: unit h, storage: unit a).  Valid when
   the receiver's JSObject shape == shape AND its type's version_tag ==
   type_version.  `shape` is a COUNTED reference marked by the owning
   bytecode (pointer reuse would otherwise validate stale entries). */
typedef struct QPAttrCache {
    struct JSShape *shape;      /* NULL = empty entry */
    uint32_t type_version;
    uint8_t kind;               /* QP_ATTR_IC_* (unit h) */
    uint32_t index;             /* shape property index / QPInstance slot */
    JSValue value;              /* borrowed class attribute (descriptor/method), valid while type_version matches */
} QPAttrCache;

/* Global inline cache for load_global / load_from_dict_or_globals. */
typedef struct QPGlobalCache {
    uint64_t globals_uid, globals_version;
    uint64_t builtins_uid, builtins_version;
    JSValue value;              /* borrowed, valid while both (uid, version) pairs match */
} QPGlobalCache;

typedef union QPInlineCache {
    QPAttrCache attr;
    QPGlobalCache global;
} QPInlineCache;

/* Python code metadata (CPython code object fields that QuickJS bytecode
   lacks).  JSFunctionDef.py_code during compilation (filled by unit b
   through the unit-a API below; fd->is_py set), then moved to
   JSFunctionBytecode.py_code (b->is_py set) by
   js_create_function.  Owned by the bytecode; freed by
   free_function_bytecode; inline caches are runtime state (zeroed on
   creation and on bytecode reading).
   Frame layout: Python parameters are the JS arguments
   vardefs[0 .. b->arg_count) in co_varnames order (posonly, positional,
   kwonly, *args, **kwargs); code without CO_NEWLOCALS (module, class body,
   exec) has exactly one hidden argument: the locals mapping.  Locals are
   vars; cell variables are captured vars (JSVarRef); free variables are
   closure_var entries in co_freevars order.  Unbound = JS_UNINITIALIZED. */
typedef struct QPCodeInfo {
    uint32_t co_flags;          /* CO_* */
    uint16_t argcount;          /* co_argcount (positional incl. posonly) */
    uint16_t posonlyargcount;
    uint16_t kwonlyargcount;
    int firstlineno;
    JSAtom qualname;            /* co_qualname (owned); co_name is b->func_name */
    int doc_cpool_idx;          /* co_consts index of the docstring, -1 if none */
    /* location table: line_num operands index locs[] (compile time);
       resolve_labels turns them into the sorted pc -> loc map */
    QPLoc *locs;
    uint32_t loc_count, loc_size;
    struct { uint32_t pc; uint32_t loc; } *pcloc;
    uint32_t pcloc_count;
    /* inline caches indexed by the IC operands */
    QPInlineCache *ic;
    uint16_t ic_count;
} QPCodeInfo;

/* ---- exceptions state (unit i; generators link their own item, unit j) */
typedef struct QPExcStackItem {
    JSValue exc_value;          /* handled exception, or None */
    struct QPExcStackItem *previous;
} QPExcStackItem;

/* ---- method cache (unit h; CPython type_cache) ------------------------- */
#define QP_MCACHE_SIZE_EXP 12
typedef struct QPMethodCacheEntry {
    uint32_t version;           /* type version_tag, 0 = empty */
    JSAtom name;                /* not counted (valid while version matches) */
    JSValue value;              /* borrowed lookup result, QP_NULL = absent */
} QPMethodCacheEntry;

/* ========================================================================= */
/* Per-context runtime state: JSContext.py (field added by unit a)            */
/* ========================================================================= */

/* Interpreter configuration given by the launcher (unit l). */
typedef struct QPConfig {
    const char *executable;     /* sys.executable */
    const char *hash_seed;      /* PYTHONHASHSEED text, NULL = "random" (see numfmt.h qp_hash_key_init) */
    int optimize;               /* -O count: sys.flags.optimize; __debug__ is optimize == 0 */
    int verbose, quiet, isolated, ignore_environment, safe_path;
    int unbuffered, inspect, interactive, dont_write_bytecode;
    int int_max_str_digits;     /* -1 = default 4300 */
    int lazy_imports;           /* -X lazy_imports / PYTHON_LAZY_IMPORTS: -1 normal, 0 none, 1 all */
    int recursion_limit;        /* 0 = default 1000 */
    int argc;                   /* sys.argv */
    char **argv;
    int orig_argc;              /* sys.orig_argv */
    char **orig_argv;
    const char *sys_path_0;     /* sys.path[0] computed by the launcher (script dir, "" for -c/stdin, cwd for -m); NULL = none (-P/-I) */
    const char *pythonpath;     /* PYTHONPATH (NULL if unset or -E/-I) */
    const char *stdio_encoding; /* PYTHONIOENCODING (NULL = default) */
    int no_user_site, no_site, bytes_warning, utf8_mode, dev_mode, warn_default_encoding;
    int nwarnoptions;           /* -W options, in order */
    char **warnoptions;
    int nxoptions;              /* -X options as given ("utf8", "lazy_imports=all", ...) for sys._xoptions */
    char **xoptions;
    const char *run_command;    /* -c text, NULL otherwise */
    const char *run_module;     /* -m name, NULL otherwise */
    const char *run_filename;   /* script path, NULL otherwise */
} QPConfig;

struct QPState {
    JSContext *ctx;
    QPConfig config;                        /* copied (argv strings are not copied: caller keeps them alive) */
    QPType *types[QP_T__COUNT];             /* builtin types (h phase 1) */
    QPType *exc_types[QP_EXC__COUNT];       /* builtin exceptions (i) */
    JSAtom atoms[QP_ATOM__COUNT];           /* interned names (a) */
    JSAtom atom_dict_slot;                  /* private symbol: hidden __dict__ property (h) */

    /* singletons (owners in parentheses) */
    JSValue not_implemented;                /* (h) */
    JSValue ellipsis;                       /* (e) */
    JSValue empty_tuple;                    /* (e) */
    JSValue empty_frozenset;                /* (f) */
    JSValue empty_bytes;                    /* (g) */

    /* modules (k, l) */
    JSValue builtins_module, builtins_dict; /* (k) */
    JSValue sys_module, sys_dict;           /* (k) */
    JSValue typing_nodefault;               /* (k) typing.NoDefault singleton */
    JSValue audit_hooks;                    /* (k) sys.addaudithook list */
    JSValue sys_structseq_types;            /* (k) tuple of sys struct-sequence types (sysmodule.c QP_SYS_SS_* order) */
    JSValue modules;                        /* sys.modules dict (l) */
    JSValue import_func;                    /* original builtins.__import__ for the import_name fast path (l) */
    JSValue importlib;                      /* importlib._bootstrap module, set by M3 boot code; JS_UNDEFINED until then (l: qp_import_name calls its _find_and_load like CPython import_find_and_load; unset -> ModuleNotFoundError) */

    /* interpreter (a) */
    int recursion_depth;                    /* Python frames currently running */
    int recursion_limit;                    /* sys.getrecursionlimit() */
    uint8_t reraise;                        /* set by reraise-like subops before `goto exception` */

    /* exceptions (i) */
    QPExcStackItem exc_info_base;
    QPExcStackItem *exc_info;               /* innermost; generators push their own item (j) */

    /* type system (h) */
    uint32_t next_type_version;             /* 0 after exhaustion: no more caching */
    QPMethodCacheEntry mcache[1 << QP_MCACHE_SIZE_EXP];
    JSValue repr_stack;                     /* list of objects being repr'd (Py_ReprEnter) */
    JSValue pending_del;                    /* list of resurrected objects whose __del__ must run */

    /* dicts (f) */
    uint64_t next_dict_uid;

    /* str (d) */
    uint8_t hash_key[16];                   /* SipHash key from QPConfig.hash_seed */
    struct QPStrCache *str_cache;           /* private: atom hash cache, UCS4 views */

    /* numbers (c) */
    int int_max_str_digits;

    /* generators (j): sys.set_asyncgen_hooks */
    JSValue asyncgen_firstiter, asyncgen_finalizer;

    /* import (l) */
    int lazy_imports_mode;
    int import_lock_level;                  /* _imp.acquire_lock/release_lock recursion count (CPython IMPORT_LOCK; single-threaded until M5) */
    int override_frozen_modules;            /* _imp._override_frozen_modules_for_tests */

    /* M3 native modules */
    struct QPModState *mod_state;           /* M3 native-module state (python/modstate.h, M3Misc); NULL until first use */
};

#define QP_STATE(ctx)      ((ctx)->py)
#define QP_TYPE(ctx, id)   (QP_STATE(ctx)->types[QP_T_ ## id])
#define QP_EXC(ctx, id)    (QP_STATE(ctx)->exc_types[QP_EXC_ ## id])
#define QP_ATOM(ctx, id)   (QP_STATE(ctx)->atoms[QP_ATOM_ ## id])
#define qp_none()          JS_UNDEFINED
#define qp_bool_new(b)     JS_NewBool(NULL, (b))   /* JS_NewBool ignores ctx */
#define qp_float_new(ctx, d) __JS_NewFloat64((ctx), (d))
#define qp_not_implemented(ctx) JS_DupValue((ctx), QP_STATE(ctx)->not_implemented)
#define qp_is_notimpl(ctx, v) \
    (JS_VALUE_GET_TAG(v) == JS_TAG_OBJECT && JS_VALUE_GET_PTR(v) == JS_VALUE_GET_PTR(QP_STATE(ctx)->not_implemented))

/* ========================================================================= */
/* (a) INTERPRETER, CALLS, FUNCTIONS, CODE, CELLS, FRAMES                     */
/*     python/ceval.c, python/ceval_ops.h, python/funcobject.c,               */
/*     python/codeobject.c, python/frameobject.c (+ quickjs.c integration)    */
/* ========================================================================= */

/* Create ctx->py, intern QP_ATOMS, then run the init functions of every unit
   in the order of m2-design.md "Init order".  cfg may be NULL (defaults).
   0 or -1 (exception pending, state freed). */
static int qp_state_init(JSContext *ctx, const QPConfig *cfg);
/* Release ctx->py (called by JS_FreeContext before the context's values go). */
static void qp_state_free(JSContext *ctx);
/* Mark every JSValue held by QPState (called by JS_MarkContext). */
static void qp_state_mark(JSRuntime *rt, QPState *s, JS_MarkFunc *mark_func);
/* Init of unit a: function, method, code, cell, frame types. */
static int qp_init_funcobject(JSContext *ctx);

/* Call any callable (CPython PyObject_Vectorcall).  args[0..nargs) then
   len(kwnames) keyword values; kwnames QP_NULL or a tuple of str.  args are
   not modified unless nargsf has QP_VECTORCALL_ARGUMENTS_OFFSET (then
   args[-1] is scratch). */
static JSValue qp_vectorcall(JSContext *ctx, JSValueConst callable, JSValueConst *args,
                             size_t nargsf, JSValueConst kwnames);
/* callable(*args_tuple, **kwargs_dict) (PyObject_Call); kwargs may be QP_NULL. */
static JSValue qp_call_tuple(JSContext *ctx, JSValueConst callable, JSValueConst args_tuple,
                             JSValueConst kwargs);
static JSValue qp_call0(JSContext *ctx, JSValueConst callable);
static JSValue qp_call1(JSContext *ctx, JSValueConst callable, JSValueConst a);
static JSValue qp_call2(JSContext *ctx, JSValueConst callable, JSValueConst a, JSValueConst b);
/* JS_CallInternal path for class ids QP_IS_PY_CLASS: dispatch to the
   type's tp_call ("'%T' object is not callable"); kwnames per
   JS_CALL_FLAG_PY_KW.  Called only by quickjs.c. */
static JSValue qp_call_object(JSContext *ctx, JSValueConst callable, JSValueConst *argv,
                              int argc, JSValueConst kwnames, int flags);
/* (M2Interp, ceval.c) Call a non-Python callee with kwnames: folds the
   len(kwnames) trailing values of argv into ONE trailing plain object
   {name: value} and calls func with argc - len(kwnames) + 1 arguments
   (flags without JS_CALL_FLAG_PY_KW). */
static JSValue qp_call_js_kw(JSContext *ctx, JSValueConst func, JSValueConst this_obj,
                             int argc, JSValueConst *argv, JSValueConst kwnames, int flags);
/* Bind arguments of a Python function at frame setup (CPython
   initialize_locals): fills arg_buf[0..b->arg_count) with new references
   (defaults, kwdefaults, *args tuple, **kwargs dict).  -1 with CPython's
   TypeError messages ("%U() missing %d required positional argument%s:
   ...", "takes %d positional arguments but %d were given", "got an
   unexpected keyword argument '%U'", "got multiple values for argument
   '%U'", "got some positional-only arguments passed as keyword
   arguments: '%U'"); arg_buf is then fully released. */
static int qp_bind_args(JSContext *ctx, JSValueConst func, struct JSFunctionBytecode *b,
                        JSValueConst *args, int nargs, JSValueConst kwnames, JSValue *arg_buf);
/* Run code without CO_NEWLOCALS (module body, class body, exec) with
   `locals` as its locals mapping (hidden argument 0). */
static JSValue qp_call_with_locals(JSContext *ctx, JSValueConst func, JSValueConst locals);
/* exec/eval core (PyEval_EvalCode): code = PY_CODE object; builds a
   function over globals (inserting __builtins__ like CPython if missing)
   and runs it with locals (globals if QP_NULL). */
static JSValue qp_eval_code(JSContext *ctx, JSValueConst code, JSValueConst globals,
                            JSValueConst locals);

/* Python function object from a bytecode value (JS_TAG_FUNCTION_BYTECODE)
   with no free variables (PyFunction_New); name/qualname/doc from the
   code, module from globals['__name__'].  Nested functions are created by
   OP_fclosure (js_closure, py path) with the same rules. */
static JSValue qp_function_new(JSContext *ctx, JSValueConst bytecode, JSValueConst globals);
/* (M2Func, funcobject.c) Called by js_closure for is_py bytecode once the
   JS_CLASS_BYTECODE_FUNCTION object `func` is built: creates its
   PY_FUNC_DATA companion in u.func.home_object with globals/builtins taken
   from parent_func (the running Python function) and name, qualname, doc,
   module from the code/globals as in PyFunction_New.  0 or -1. */
static int qp_function_init(JSContext *ctx, JSValueConst func, JSValueConst parent_func);
/* QPFuncData of a Python function, NULL if v is not one (no exception). */
static QPFuncData *qp_function_data(JSValueConst v);
static BOOL qp_is_py_function(JSValueConst v);
/* set_func_attr subop and INTRINSIC_SET_FUNCTION_TYPE_PARAMS
   (flag = QP_FUNC_ATTR_* or 0 for __type_params__); consumes nothing. */
static int qp_function_set_attr(JSContext *ctx, JSValueConst func, int flag, JSValueConst value);
/* Bound method object (PyMethod_New). */
static JSValue qp_method_new(JSContext *ctx, JSValueConst func, JSValueConst self);

/* Code metadata of a Python bytecode (NULL for JS bytecode). */
static QPCodeInfo *qp_code_info(struct JSFunctionBytecode *b);
/* PY_CODE facade over a bytecode value (co_* attributes, replace(),
   co_lines(), co_positions()). */
static JSValue qp_code_new(JSContext *ctx, JSValueConst bytecode);
/* Bytecode of a PY_CODE object or of a Python function, NULL otherwise. */
static struct JSFunctionBytecode *qp_code_bytecode(JSValueConst code_or_func);
/* Location of the instruction at byte offset pc (CPython
   PyCode_Addr2Location).  0 found, -1 none (loc set to -1s). */
static int qp_code_addr2loc(struct JSFunctionBytecode *b, uint32_t pc, QPLoc *loc);

/* Cells: var_ref NULL makes a new empty cell (cell()). */
static JSValue qp_cell_new(JSContext *ctx, struct JSVarRef *var_ref);
/* Cell contents, QP_NULL if empty (no exception). */
static JSValue qp_cell_get(JSContext *ctx, JSValueConst cell);
static int qp_cell_set(JSContext *ctx, JSValueConst cell, JSValueConst v);

/* Frame facades.  Frames are snapshots (f_lineno and f_locals as of
   creation) except generator frames, which read the live heap frame. */
static JSValue qp_frame_new(JSContext *ctx, JSValueConst func, uint32_t pc,
                            JSValueConst back, JSValueConst locals);
/* The depth-th Python frame above the running native function (0 = its
   Python caller), NULL if none. */
static struct JSStackFrame *qp_py_frame_at(JSContext *ctx, int depth);
/* sys._getframe(depth): facade chain with f_back (ValueError "call stack
   is not deep enough"). */
static JSValue qp_getframe(JSContext *ctx, int depth);
/* globals() / __builtins__ of a Python frame (borrowed; QP_NULL if sf is
   NULL or not Python). */
static JSValue qp_frame_globals(struct JSStackFrame *sf);
static JSValue qp_frame_builtins(struct JSStackFrame *sf);
/* locals() (PEP 667): the locals mapping for code without CO_NEWLOCALS,
   else a new dict snapshot of the bound locals and cells. */
static JSValue qp_frame_locals(JSContext *ctx, struct JSStackFrame *sf);
/* Zero-argument super(): first argument and __class__ cell of the frame
   (RuntimeError "super(): no arguments", "super(): __class__ cell not
   found", "super(): empty __class__ cell"). */
static int qp_frame_super_args(JSContext *ctx, struct JSStackFrame *sf,
                               JSValue *ptype, JSValue *pself);

/* ---- compile-time API used by unit b ------------------------------------ */
static QPCodeInfo *qp_code_info_new(JSContext *ctx);
/* Called by free_function_bytecode / JSFunctionDef freeing. */
static void qp_code_info_free(JSRuntime *rt, QPCodeInfo *ci);
/* Called by mark_children for function bytecode (IC shape references). */
static void qp_code_info_mark(JSRuntime *rt, QPCodeInfo *ci, JS_MarkFunc *mark_func);
/* Append a location; returns its index (the OP_line_num operand), -1 OOM. */
static int qp_fd_add_loc(JSContext *ctx, struct JSFunctionDef *fd, const QPLoc *loc);
/* Emit OP_py <subop> [operands] into s->cur_func (bookkeeping like
   emit_op: last_opcode_pos).  _atom* duplicate the atom; _atom_ic
   allocates the next inline-cache index of the function; _label registers
   a jump to `label` like emit_goto and returns the label. */
static void qp_emit_py(struct JSParseState *s, int subop);
static void qp_emit_py_u8(struct JSParseState *s, int subop, uint8_t v);
static void qp_emit_py_u16(struct JSParseState *s, int subop, uint16_t v);
static void qp_emit_py_atom(struct JSParseState *s, int subop, JSAtom atom);
static void qp_emit_py_atom_u8(struct JSParseState *s, int subop, JSAtom atom, uint8_t v);
static void qp_emit_py_atom_u16(struct JSParseState *s, int subop, JSAtom atom, uint16_t v);
static void qp_emit_py_atom_ic(struct JSParseState *s, int subop, JSAtom atom);
static int qp_emit_py_label(struct JSParseState *s, int subop, int label);
/* js_create_function for the root of a Python JSFunctionDef tree: skips
   resolve_variables, runs resolve_labels + compute_stack_size, moves fd->py_code
   into the bytecode.  Returns the bytecode value; frees the fd tree. */
static JSValue qp_create_function(JSContext *ctx, struct JSFunctionDef *fd);

/* ========================================================================= */
/* (b) CODEGEN: python/compile.c                                              */
/* ========================================================================= */

/* Compile a parsed module (CPython _PyAST_Compile): future features,
   symtable, qp_validate, codegen.  mode = Py_file_input (257),
   Py_eval_input (258), Py_single_input (256) (print_expr for expression
   statements); flags = QP_PF_* / CO_FUTURE_*; optimize = -O level (-1 =
   QPConfig.optimize).  Returns the module-level bytecode value
   (co_name "<module>", no CO_NEWLOCALS).  On failure JS_EXCEPTION: if
   err->kind != QP_ERR_NONE it is a compile error described by err and no
   JS exception is pending (convert with qp_raise_qp_error), else a JS
   exception is pending (out of memory). */
static JSValue qp_compile_ast(JSContext *ctx, mod_ty mod, const char *filename, int mode,
                              int flags, int optimize, qp_arena *arena, qp_error *err);
/* Parse (qp_parse_string) + qp_compile_ast with a private arena. */
static JSValue qp_compile_source(JSContext *ctx, const char *src, size_t len,
                                 const char *filename, int mode, int flags,
                                 int optimize, qp_error *err);

/* ========================================================================= */
/* (c) NUMBERS: python/longobject.c, python/floatobject.c,                    */
/*     python/complexobject.c                                                 */
/* ========================================================================= */

static int qp_init_numbers(JSContext *ctx);   /* int, bool, float, complex */

/* int checks: incl. bool and subclass instances / exact int tags only. */
static BOOL qp_long_check(JSContext *ctx, JSValueConst v);
static BOOL qp_long_check_exact(JSValueConst v);
/* Plain int value of an int, bool or int-subclass instance (new ref). */
static JSValue qp_long_value(JSContext *ctx, JSValueConst v);
/* Canonical int from a JS int/bigint value (consumes v). */
static JSValue qp_long_normalize(JSContext *ctx, JSValue v);
static JSValue qp_long_from_int64(JSContext *ctx, int64_t v);
static JSValue qp_long_from_uint64(JSContext *ctx, uint64_t v);
/* int(float) truncation ("cannot convert float NaN to integer",
   "cannot convert float infinity to integer"). */
static JSValue qp_long_from_double(JSContext *ctx, double d);
/* int(str, base) parsing rules incl. underscores, whitespace, base 0 and
   int_max_str_digits (ValueError "invalid literal for int() with base
   %d: %R"). */
static JSValue qp_long_from_str(JSContext *ctx, JSValueConst s, int base);
/* Exact int (or bool) -> C: OverflowError "Python int too large to convert
   to C long" / "... C ssize_t"; *overflow variant never raises for range
   (PyLong_AsLongAndOverflow). */
static int qp_long_as_int64(JSContext *ctx, JSValueConst v, int64_t *out);
static int qp_long_as_int64_overflow(JSValueConst v, int64_t *out, int *overflow);
static int qp_long_as_ssize(JSContext *ctx, JSValueConst v, Py_ssize_t *out);
/* OverflowError "int too large to convert to float". */
static int qp_long_as_double(JSContext *ctx, JSValueConst v, double *out);
static int qp_long_sign(JSValueConst v);                       /* exact ints: -1/0/1 */
static int qp_long_compare(JSValueConst a, JSValueConst b);    /* exact ints: -1/0/1 */
static Py_hash_t qp_long_hash(JSValueConst v);                 /* exact ints/bools, numfmt hashes */
/* str of an exact int in base 2/8/10/16 with CPython's prefixes for 2/8/16
   when alternate (bin/oct/hex) and the int_max_str_digits check for 10. */
static JSValue qp_long_to_str(JSContext *ctx, JSValueConst v, int base, int alternate);
/* PyNumber_Long (int(x)) and PyNumber_Float (float(x)). */
static JSValue qp_number_long(JSContext *ctx, JSValueConst v);
static JSValue qp_number_float(JSContext *ctx, JSValueConst v);

static BOOL qp_float_check(JSContext *ctx, JSValueConst v);    /* float or subclass instance */
/* PyFloat_AsDouble: float, __float__, __index__ ("must be real number,
   not %T"). */
static int qp_float_as_double(JSContext *ctx, JSValueConst v, double *out);
/* repr(float) (numfmt qp_float_repr rules) as str. */
static JSValue qp_float_to_repr(JSContext *ctx, double d);
/* float(str) ("could not convert string to float: %R"). */
static JSValue qp_float_from_str(JSContext *ctx, JSValueConst s);
static Py_hash_t qp_float_hash(double d);

static JSValue qp_complex_new(JSContext *ctx, double real, double imag);
static BOOL qp_complex_check(JSContext *ctx, JSValueConst v);
/* PyComplex_AsCComplex: complex, __complex__, __float__, __index__. */
static int qp_complex_as_c(JSContext *ctx, JSValueConst v, QPComplex *out);

/* ========================================================================= */
/* (d) STR: python/unicodeobject.c, python/unicode_format.c,                  */
/*     python/templateobject.c                                                */
/* ========================================================================= */

static int qp_init_str(JSContext *ctx);       /* str, Template, Interpolation; hash key */

static BOOL qp_str_check(JSContext *ctx, JSValueConst v);      /* str or subclass instance */
static BOOL qp_str_check_exact(JSValueConst v);                /* JS_TAG_STRING / _ROPE */
/* Plain str of a str or str-subclass instance (new ref). */
static JSValue qp_str_value(JSContext *ctx, JSValueConst v);
/* Strict UTF-8 decoding (UnicodeDecodeError). */
static JSValue qp_str_from_utf8(JSContext *ctx, const char *s, size_t len);
static JSValue qp_str_from_cstr(JSContext *ctx, const char *s);
static JSValue qp_str_from_ucs4(JSContext *ctx, const uint32_t *s, size_t len);
static JSValue qp_str_from_char(JSContext *ctx, uint32_t cp);
static JSValue qp_str_from_view(JSContext *ctx, qp_strview v);
static JSValue qp_str_from_atom(JSContext *ctx, JSAtom atom);
/* New atom for an exact str (JS_ATOM_NULL + exception on failure). */
static JSAtom qp_str_to_atom(JSContext *ctx, JSValueConst s);
/* PyUnicode_FromFormat: %s %d %i %u %ld %lld %zd %zu %x %c %p and
   %U (str JSValue), %S (str(obj)), %R (repr(obj)), %A (ascii(obj)),
   %T (fully qualified type name of an object), %N (fully qualified name of
   a QPType *), with CPython's width/precision rules (e.g. %.100s).
   JSValue arguments are passed by value (JSValueConst). */
static JSValue qp_str_from_format(JSContext *ctx, const char *fmt, ...);
static JSValue qp_str_from_format_v(JSContext *ctx, const char *fmt, va_list ap);
/* Length in code points (surrogate pairs count once). */
static Py_ssize_t qp_str_len(JSContext *ctx, JSValueConst s);
/* Code-point view (kind 1, 2 or 4; kind 4 for strings with surrogate
   pairs).  Valid until qp_str_view_release; several may be held at once. */
static int qp_str_view_acquire(JSContext *ctx, JSValueConst s, qp_strview *view);
static void qp_str_view_release(JSContext *ctx, qp_strview *view);
/* UTF-8 of an exact str, free with JS_FreeCString (UnicodeEncodeError
   "... surrogates not allowed" for lone surrogates). */
static const char *qp_str_utf8(JSContext *ctx, JSValueConst s, size_t *plen);
static JSValue qp_str_concat(JSContext *ctx, JSValueConst a, JSValueConst b);
/* sep.join(items) for exact strs (build_string uses sep ""). */
static JSValue qp_str_join_array(JSContext *ctx, JSValueConst sep, int n, JSValueConst *items);
static int qp_str_eq(JSContext *ctx, JSValueConst a, JSValueConst b);      /* 1/0, -1 OOM only */
static int qp_str_compare(JSContext *ctx, JSValueConst a, JSValueConst b); /* code point order: <0/0/>0; OOM -> exception, returns INT_MIN */
/* hash(str): CPython SipHash-1-3 keyed by QPState.hash_key (identical to
   CPython under the same PYTHONHASHSEED); cached for atoms. */
static Py_hash_t qp_str_hash(JSContext *ctx, JSValueConst s);
static JSValue qp_str_substring(JSContext *ctx, JSValueConst s, Py_ssize_t start, Py_ssize_t end);
static JSValue qp_str_repr(JSContext *ctx, JSValueConst s);
/* repr text with every non-ASCII code point escaped (ascii()). */
static JSValue qp_str_ascii_escape(JSContext *ctx, JSValueConst repr_str);
/* str.encode / bytes.decode codecs (utf-8, utf-16, utf-32, latin-1, ascii
   with strict/ignore/replace/backslashreplace/surrogateescape/
   surrogatepass/xmlcharrefreplace error handlers). */
static JSValue qp_str_encode(JSContext *ctx, JSValueConst s, const char *encoding, const char *errors);
static JSValue qp_str_decode(JSContext *ctx, const char *data, Py_ssize_t len,
                             const char *encoding, const char *errors);
/* string.templatelib objects (build_template / build_interpolation). */
static JSValue qp_template_new(JSContext *ctx, JSValueConst strings, JSValueConst interpolations);
static JSValue qp_interpolation_new(JSContext *ctx, JSValueConst value, JSValueConst expression,
                                    int conversion, JSValueConst format_spec);

/* ========================================================================= */
/* (e) LIST, TUPLE, RANGE, SLICE, ITERATORS: python/listobject.c,             */
/*     python/tupleobject.c, python/rangeobject.c, python/sliceobject.c,      */
/*     python/iterobject.c                                                    */
/* ========================================================================= */

static int qp_init_sequences(JSContext *ctx); /* list, tuple, range, slice, ellipsis, iterator types */

/* list = JS_CLASS_ARRAY fast array (D3).  Python list operations keep the
   JS `length` property in sync through the QuickJS array helpers. */
static BOOL qp_list_check(JSContext *ctx, JSValueConst v);     /* list or subclass */
static BOOL qp_list_check_exact(JSContext *ctx, JSValueConst v);
/* New list of n None items. */
static JSValue qp_list_new(JSContext *ctx, Py_ssize_t n);
/* New list from n values (steals items[0..n)). */
static JSValue qp_list_new_steal(JSContext *ctx, Py_ssize_t n, JSValue *items);
/* Element storage of a fast array (borrowed, invalidated by any list
   mutation); NULL if the array is not fast (then use the generic API). */
static JSValue *qp_list_items(JSValueConst list, Py_ssize_t *plen);
static Py_ssize_t qp_list_size(JSValueConst list);
static int qp_list_append(JSContext *ctx, JSValueConst list, JSValueConst v);
static int qp_list_extend(JSContext *ctx, JSValueConst list, JSValueConst iterable);
static int qp_list_insert(JSContext *ctx, JSValueConst list, Py_ssize_t index, JSValueConst v);
/* list(iterable) and tuple(list) */
static JSValue qp_list_from_iterable(JSContext *ctx, JSValueConst iterable);
static JSValue qp_list_as_tuple(JSContext *ctx, JSValueConst list);
/* list.sort(key=..., reverse=...) on any list (key QP_NULL = none). */
static int qp_list_sort(JSContext *ctx, JSValueConst list, JSValueConst key, int reverse);

static BOOL qp_tuple_check(JSContext *ctx, JSValueConst v);    /* tuple or subclass */
static BOOL qp_tuple_check_exact(JSContext *ctx, JSValueConst v);
/* New tuple of n None items (to be filled before publication). */
static JSValue qp_tuple_new(JSContext *ctx, Py_ssize_t n);
/* New tuple from n values (steals items[0..n); on failure they are freed). */
static JSValue qp_tuple_new_steal(JSContext *ctx, Py_ssize_t n, JSValue *items);
/* New tuple from n borrowed values. */
static JSValue qp_tuple_pack(JSContext *ctx, Py_ssize_t n, JSValueConst *items);
/* tuple(iterable) (PySequence_Tuple). */
static JSValue qp_tuple_from_iterable(JSContext *ctx, JSValueConst iterable);

static JSValue qp_range_new(JSContext *ctx, JSValueConst start, JSValueConst stop, JSValueConst step);
static JSValue qp_slice_new(JSContext *ctx, JSValueConst start, JSValueConst stop, JSValueConst step);
static BOOL qp_slice_check(JSValueConst v);
/* PySlice_Unpack (indices clamped to Py_ssize_t, "slice step cannot be
   zero") and PySlice_AdjustIndices (returns the slice length). */
static int qp_slice_unpack(JSContext *ctx, JSValueConst slice, Py_ssize_t *start,
                           Py_ssize_t *stop, Py_ssize_t *step);
static Py_ssize_t qp_slice_adjust_indices(Py_ssize_t length, Py_ssize_t *start,
                                          Py_ssize_t *stop, Py_ssize_t step);
/* iter(seq) for objects with only __getitem__, and iter(callable, sentinel). */
static JSValue qp_seqiter_new(JSContext *ctx, JSValueConst seq);
static JSValue qp_calliter_new(JSContext *ctx, JSValueConst callable, JSValueConst sentinel);
/* Unpacking of unpack_seq / unpack_ex (CPython unpack_iterable): stores
   before + (after >= 0 ? 1 + after : 0) values in out[] in stack order
   (out[0] is the deepest); after = -1 means exact length `before`.
   Messages "not enough values to unpack (expected %d, got %d)", "too many
   values to unpack (expected %d)", "cannot unpack non-iterable %T object". */
static int qp_unpack_iterable(JSContext *ctx, JSValueConst v, int before, int after, JSValue *out);

/* ========================================================================= */
/* (f) DICT, FROZENDICT, SET, FROZENSET, VIEWS: python/dictobject.c,          */
/*     python/setobject.c                                                     */
/* ========================================================================= */

static int qp_init_dicts(JSContext *ctx);     /* dict types, mappingproxy, set types */

static BOOL qp_dict_check(JSContext *ctx, JSValueConst v);     /* dict or subclass (not frozendict) */
static BOOL qp_dict_check_exact(JSContext *ctx, JSValueConst v);
static BOOL qp_anydict_check(JSContext *ctx, JSValueConst v);  /* dict or frozendict (or subclasses) */
static JSValue qp_dict_new(JSContext *ctx);
static JSValue qp_dict_new_presized(JSContext *ctx, Py_ssize_t n);
/* d[key] lookup without KeyError: 1 found (*pvalue new ref), 0 absent
   (*pvalue = QP_NULL), -1 error (unhashable key, __eq__ raised). */
static int qp_dict_get_item(JSContext *ctx, JSValueConst d, JSValueConst key, JSValue *pvalue);
/* Same with an atom key (the str of the atom); fast path for names. */
static int qp_dict_get_item_atom(JSContext *ctx, JSValueConst d, JSAtom key, JSValue *pvalue);
static int qp_dict_set_item(JSContext *ctx, JSValueConst d, JSValueConst key, JSValueConst value);
static int qp_dict_set_item_atom(JSContext *ctx, JSValueConst d, JSAtom key, JSValueConst value);
/* 1 deleted, 0 absent (no KeyError), -1 error. */
static int qp_dict_del_item(JSContext *ctx, JSValueConst d, JSValueConst key);
static int qp_dict_del_item_atom(JSContext *ctx, JSValueConst d, JSAtom key);
static int qp_dict_contains(JSContext *ctx, JSValueConst d, JSValueConst key);
static Py_ssize_t qp_dict_size(JSValueConst d);
/* Borrowed iteration (PyDict_Next): 1 with *pkey and *pvalue, 0 at the end;
   the dict must not be mutated meanwhile. */
static int qp_dict_next(JSValueConst d, Py_ssize_t *ppos, JSValue *pkey, JSValue *pvalue);
/* d.setdefault(key, dflt) -> new ref of the stored value. */
static JSValue qp_dict_setdefault(JSContext *ctx, JSValueConst d, JSValueConst key, JSValueConst dflt);
/* d.pop(key) without default: 1 (*pvalue new ref), 0 absent, -1 error. */
static int qp_dict_pop(JSContext *ctx, JSValueConst d, JSValueConst key, JSValue *pvalue);
/* PyDict_Merge(d, other, override): override 0 keep, 1 replace, 2 raise
   KeyError on duplicates; `other` any mapping (keys() + __getitem__). */
static int qp_dict_merge(JSContext *ctx, JSValueConst d, JSValueConst other, int override);
/* dict.update(other) semantics (mapping or iterable of pairs). */
static int qp_dict_update(JSContext *ctx, JSValueConst d, JSValueConst other);
/* dict_merge subop: ** argument into the call's kwargs dict with CPython's
   errors naming `callable` ("%U argument after ** must be a mapping, not
   %T", "%U got multiple values for keyword argument '%S'"). */
static int qp_dict_merge_kwargs(JSContext *ctx, JSValueConst d, JSValueConst other,
                                JSValueConst callable);
static JSValue qp_dict_copy(JSContext *ctx, JSValueConst d);
/* build_map: dict from n key/value pairs kv[2i], kv[2i+1] (borrowed). */
static JSValue qp_dict_from_pairs(JSContext *ctx, int n, JSValueConst *kv);
/* list(d.keys()) / list(d.values()) / list(d.items()) */
static JSValue qp_dict_keys_list(JSContext *ctx, JSValueConst d);
static JSValue qp_dict_values_list(JSContext *ctx, JSValueConst d);
static JSValue qp_dict_items_list(JSContext *ctx, JSValueConst d);
/* types.MappingProxyType(mapping) (type.__dict__). */
static JSValue qp_mappingproxy_new(JSContext *ctx, JSValueConst mapping);

static BOOL qp_set_check(JSContext *ctx, JSValueConst v);          /* set or subclass */
static BOOL qp_anyset_check(JSContext *ctx, JSValueConst v);       /* set, frozenset or subclasses */
static BOOL qp_frozenset_check(JSContext *ctx, JSValueConst v);
/* set(iterable) / frozenset(iterable); iterable QP_NULL = empty. */
static JSValue qp_set_new(JSContext *ctx, JSValueConst iterable);
static JSValue qp_frozenset_new(JSContext *ctx, JSValueConst iterable);
/* build_set: set of n borrowed values. */
static JSValue qp_set_from_array(JSContext *ctx, int n, JSValueConst *items);
static int qp_set_add(JSContext *ctx, JSValueConst set, JSValueConst key);
static int qp_set_contains(JSContext *ctx, JSValueConst set, JSValueConst key);
static int qp_set_discard(JSContext *ctx, JSValueConst set, JSValueConst key); /* 1/0/-1 */
static int qp_set_update(JSContext *ctx, JSValueConst set, JSValueConst iterable);
static Py_ssize_t qp_set_size(JSValueConst set);
/* Borrowed iteration (_PySet_NextEntry): 1 / 0. */
static int qp_set_next(JSValueConst set, Py_ssize_t *ppos, JSValue *pkey, Py_hash_t *phash);

/* ========================================================================= */
/* (g) BYTES, BYTEARRAY, MEMORYVIEW: python/bytesobject.c,                    */
/*     python/bytearrayobject.c, python/memoryobject.c                        */
/* ========================================================================= */

static int qp_init_bytes(JSContext *ctx);     /* bytes, bytearray, memoryview */

static JSValue qp_bytes_new(JSContext *ctx, const void *data, Py_ssize_t len);
static BOOL qp_bytes_check(JSContext *ctx, JSValueConst v);        /* bytes or subclass */
static BOOL qp_bytearray_check(JSContext *ctx, JSValueConst v);
/* Data of a bytes or bytearray (borrowed; bytearray data moves on resize). */
static const uint8_t *qp_bytes_data(JSValueConst v, Py_ssize_t *plen);
static JSValue qp_bytearray_new(JSContext *ctx, const void *data, Py_ssize_t len);
/* bytes(x) (PyBytes_FromObject + the bytes() constructor rules). */
static JSValue qp_bytes_from_object(JSContext *ctx, JSValueConst v);
/* repr of bytes/bytearray data (b'...' with CPython quoting/escapes). */
static JSValue qp_bytes_repr(JSContext *ctx, const uint8_t *data, Py_ssize_t len, int smartquotes);
/* Buffer protocol: bytes, bytearray, memoryview, JS typed arrays
   ("a bytes-like object is required, not '%T'"). */
static int qp_buffer_get(JSContext *ctx, JSValueConst obj, QPBuffer *view, int flags);
static void qp_buffer_release(JSContext *ctx, QPBuffer *view);

/* ========================================================================= */
/* (h) TYPE SYSTEM AND OBJECT PROTOCOL: python/typeobject.c,                  */
/*     python/object.c, python/descrobject.c, python/abstract.c               */
/* ========================================================================= */

/* Phase 1: allocate every QP_T_* type object (empty, not ready; proto =
   `type`) and the method cache.  Phase 2 (qp_init_typeobject): ready
   object, type, descriptor types, property, classmethod, staticmethod,
   super, builtin_function_or_method, NoneType, NotImplementedType,
   JsObject; create NotImplemented. */
static int qp_init_types_phase1(JSContext *ctx);
static int qp_init_typeobject(JSContext *ctx);

/* type(v) (borrowed; never NULL): by tag, then the JS prototype when it is
   a type object, then by class id (JS_CLASS_ARRAY -> list, Python bytecode
   functions -> function, typed arrays -> memoryview, else JsObject). */
static QPType *qp_type_of(JSContext *ctx, JSValueConst v);
static QPType *qp_type_from_value(JSValueConst v);             /* NULL if v is not a type */
static int qp_type_is_subtype(QPType *a, QPType *b);           /* MRO walk, cannot fail */
/* isinstance(v, t) without __instancecheck__ (PyObject_TypeCheck). */
static BOOL qp_typecheck(JSContext *ctx, JSValueConst v, QPType *t);
/* Ready a builtin type from its spec (PyType_Ready): bases, MRO, dict with
   methods/getsets/members/slot wrappers, slot inheritance, version tag. */
static int qp_type_ready(JSContext *ctx, QPType *t, const QPTypeSpec *spec);
/* PyType_FromSpec for native-module types: allocates a QPType (like
   builtin phase 1), runs qp_type_ready, __module__/__qualname__ from the
   dotted spec->name, base from spec->base QPTypeId.  New reference to the
   type object (QPType* via qp_type_from_value). */
static JSValue qp_type_from_spec(JSContext *ctx, const QPTypeSpec *spec);
/* type(name, bases, ns, **kwds) with metatype `meta` (type_new): __slots__,
   __classcell__, __set_name__, __init_subclass__ (via kwds), __qualname__,
   __static_attributes__, __firstlineno__.  kwds is a dict or QP_NULL. */
static JSValue qp_type_new(JSContext *ctx, QPType *meta, JSValueConst name, JSValueConst bases,
                           JSValueConst ns, JSValueConst kwds);
/* _PyType_Lookup through the method cache: borrowed value or QP_NULL;
   never raises. */
static JSValue qp_type_lookup(JSContext *ctx, QPType *t, JSAtom name);
/* PyType_Modified: invalidate version tags of t and its subclasses. */
static void qp_type_modified(JSContext *ctx, QPType *t);
/* Version tag (assigned on demand), 0 if exhausted. */
static uint32_t qp_type_version(JSContext *ctx, QPType *t);
/* New instance object of t: JS object of class t->instance_class_id with
   prototype t (QPInstance for t->nslots); payload of other classes is
   set by the caller.  Used by tp_new of every type for subclasses. */
static JSValue qp_object_new_instance(JSContext *ctx, QPType *t, int class_id);
/* QPInstance slot storage of a PY_OBJECT (NULL if none). */
static JSValue *qp_instance_slots(JSValueConst obj);

/* Attributes (PyObject_GetAttr etc.); name may be any atom. */
static JSValue qp_getattr(JSContext *ctx, JSValueConst obj, JSAtom name);
/* PyObject_GetOptionalAttr: 1 found (*pres new ref), 0 AttributeError
   suppressed (*pres = QP_NULL), -1 other error. */
static int qp_getattr_opt(JSContext *ctx, JSValueConst obj, JSAtom name, JSValue *pres);
static int qp_setattr(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v);
static int qp_delattr(JSContext *ctx, JSValueConst obj, JSAtom name);
/* object.__getattribute__ / __setattr__ (PyObject_GenericGetAttr/SetAttr). */
static JSValue qp_generic_getattr(JSContext *ctx, JSValueConst obj, JSAtom name);
static int qp_generic_setattr(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v);
/* obj.__dict__ (materializes shape attributes; AttributeError if none). */
static JSValue qp_object_get_dict(JSContext *ctx, JSValueConst obj);
/* Inline-cached forms for the getattr / load_method / setattr subops. */
static JSValue qp_getattr_ic(JSContext *ctx, JSValueConst obj, JSAtom name, QPAttrCache *ic);
/* load_method: *pmeth and *pself new refs; *pself = QP_NULL when the attribute
   is not an unbound method (then *pmeth is the attribute). */
static int qp_load_method_ic(JSContext *ctx, JSValueConst obj, JSAtom name, QPAttrCache *ic,
                             JSValue *pmeth, JSValue *pself);
static int qp_load_method(JSContext *ctx, JSValueConst obj, JSAtom name,
                          JSValue *pmeth, JSValue *pself);
static int qp_setattr_ic(JSContext *ctx, JSValueConst obj, JSAtom name, JSValueConst v,
                         QPAttrCache *ic);
/* obj.name(*args) (PyObject_VectorcallMethod). */
static JSValue qp_call_method(JSContext *ctx, JSValueConst obj, JSAtom name,
                              JSValueConst *args, size_t nargsf, JSValueConst kwnames);
/* Special method lookup on the type, bound to obj (_PyObject_LookupSpecial):
   QP_NULL if absent (no exception). */
static JSValue qp_lookup_special(JSContext *ctx, JSValueConst obj, JSAtom name);
/* load_special subop (which = QP_SPECIAL_*). */
static int qp_load_special(JSContext *ctx, JSValueConst obj, int which,
                           JSValue *pmeth, JSValue *pself);
/* load_super_attr / load_super_method subops (flags: QP_SUPER_TWO_ARGS,
   method form when pself != NULL). */
static int qp_super_lookup(JSContext *ctx, JSValueConst global_super, JSValueConst cls,
                           JSValueConst self, JSAtom name, int flags,
                           JSValue *pres, JSValue *pself);

static JSValue qp_repr(JSContext *ctx, JSValueConst v);
static JSValue qp_str(JSContext *ctx, JSValueConst v);
static JSValue qp_ascii(JSContext *ctx, JSValueConst v);
/* hash(v) (TypeError "unhashable type: '%T'"). */
static Py_hash_t qp_hash(JSContext *ctx, JSValueConst v);
/* PyObject_HashNotImplemented: TypeError "unhashable type: '%T'", -1.  As
   tp_hash of a builtin spec, qp_type_ready puts __hash__ = None in the
   dict; a heap type's __hash__ = None maps to it. */
static Py_hash_t qp_hash_not_implemented(JSContext *ctx, JSValueConst v);
static int qp_bool(JSContext *ctx, JSValueConst v);            /* truth value: 1/0/-1 */
/* identity (`is`): see the is_op subop. */
static BOOL qp_is(JSValueConst a, JSValueConst b);
static JSValue qp_richcompare(JSContext *ctx, JSValueConst a, JSValueConst b, int op);
/* PyObject_RichCompareBool (identity implies equality for EQ/NE). */
static int qp_richcompare_bool(JSContext *ctx, JSValueConst a, JSValueConst b, int op);
/* Binary operator QP_NB_* incl. in-place forms and reflected operands
   (binary_op1 / binary_iop1, "unsupported operand type(s) for %s: '%T'
   and '%T'"); QP_NB_SUBSCR = getitem. */
static JSValue qp_binop(JSContext *ctx, JSValueConst a, JSValueConst b, int nb);
/* pow(a, b, mod) with a 3rd argument (mod = None for 2-argument pow). */
static JSValue qp_power(JSContext *ctx, JSValueConst a, JSValueConst b, JSValueConst mod);
static JSValue qp_unop(JSContext *ctx, JSValueConst a, int unop);  /* QP_UNOP_* */
/* operator.index(v) -> exact int ("'%T' object cannot be interpreted as an
   integer"). */
static JSValue qp_index(JSContext *ctx, JSValueConst v);
/* PyNumber_AsSsize_t: index then clamp/raise (exc_type QPType* or NULL =
   clamp). */
static int qp_as_ssize(JSContext *ctx, JSValueConst v, QPType *exc_type, Py_ssize_t *out);
static JSValue qp_getitem(JSContext *ctx, JSValueConst c, JSValueConst key);
static int qp_setitem(JSContext *ctx, JSValueConst c, JSValueConst key, JSValueConst v);
static int qp_delitem(JSContext *ctx, JSValueConst c, JSValueConst key);
static JSValue qp_getslice(JSContext *ctx, JSValueConst c, JSValueConst start, JSValueConst stop);
static int qp_setslice(JSContext *ctx, JSValueConst c, JSValueConst start, JSValueConst stop,
                       JSValueConst v);
static Py_ssize_t qp_len(JSContext *ctx, JSValueConst v);      /* "object of type '%T' has no len()" */
/* operator.length_hint */
static Py_ssize_t qp_length_hint(JSContext *ctx, JSValueConst v, Py_ssize_t dflt);
/* `key in c` (PySequence_Contains): 1/0/-1. */
static int qp_contains(JSContext *ctx, JSValueConst c, JSValueConst key);
/* iter(v) ("'%T' object is not iterable"). */
static JSValue qp_iter(JSContext *ctx, JSValueConst v);
/* next(it): value, QP_NULL when exhausted (StopIteration consumed, no
   exception), JS_EXCEPTION on error. */
static JSValue qp_next(JSContext *ctx, JSValueConst it);
/* get_iter / for_iter subops (see opcode.h). */
static int qp_get_iter_virtual(JSContext *ctx, JSValueConst obj, int kind,
                               JSValue *piter, JSValue *pidx);
static int qp_iter_next_virtual(JSContext *ctx, JSValueConst iter, JSValue *pidx, JSValue *pitem);
/* isinstance/issubclass with __instancecheck__/__subclasscheck__ and
   tuples/unions. */
static int qp_isinstance(JSContext *ctx, JSValueConst inst, JSValueConst cls);
static int qp_issubclass(JSContext *ctx, JSValueConst derived, JSValueConst cls);
/* format(obj, spec) (PyObject_Format; spec QP_NULL = ''). */
static JSValue qp_format(JSContext *ctx, JSValueConst obj, JSValueConst spec);
/* dir(obj) (PyObject_Dir). */
static JSValue qp_dir(JSContext *ctx, JSValueConst obj);
/* match_class / match_keys subops (CPython match_class, match_keys). */
static JSValue qp_match_class(JSContext *ctx, JSValueConst subject, JSValueConst cls,
                              int nargs, JSValueConst kwnames);
static JSValue qp_match_keys(JSContext *ctx, JSValueConst subject, JSValueConst keys);

/* Descriptor / builtin function construction. */
static JSValue qp_descr_new_method(JSContext *ctx, QPType *t, const QPMethodDef *ml);
static JSValue qp_descr_new_getset(JSContext *ctx, QPType *t, const QPGetSetDef *gs);
/* D5 (CapiB) R2: member_descriptor whose access goes through gs (C-API
   PyMemberDef wrappers); gs->set NULL = "readonly attribute". */
static JSValue qp_descr_new_member_getset(JSContext *ctx, QPType *t, const QPGetSetDef *gs);
/* D5 (CapiB) R3: PyType_Ready core for C-API types: new type of metatype
   `meta` from spec (spec->base ignored); `bases` is the tuple of bases
   (solid base and MRO come from it); `dict` (QP_NULL or a dict) is merged
   before the descriptors; spec->slots are inherited per inherit_slots; no
   __init_subclass__ / __set_name__; __module__ from the dotted name unless
   dict sets it.  spec->flags may carry QP_TPFLAGS_HEAPTYPE.  New reference. */
static JSValue qp_type_new_from_spec_bases(JSContext *ctx, QPType *meta, const QPTypeSpec *spec,
                                           JSValueConst bases, JSValueConst dict);
/* builtin_function_or_method for module-level C functions (self = module
   or QP_NULL, module = __module__ name or None). */
static JSValue qp_cfunction_new(JSContext *ctx, const QPMethodDef *ml, JSValueConst self,
                                JSValueConst module);
static JSValue qp_classmethod_new(JSContext *ctx, JSValueConst callable);
static JSValue qp_staticmethod_new(JSContext *ctx, JSValueConst callable);

/* Recursive repr guard (Py_ReprEnter/Leave): 0 entered, 1 already being
   repr'd (caller prints "..."), -1 error. */
static int qp_repr_enter(JSContext *ctx, JSValueConst obj);
static void qp_repr_leave(JSContext *ctx, JSValueConst obj);

/* __del__ support: called by free_object (quickjs.c) when the reference
   count of a Python instance whose type has QP_TPFLAGS_HAS_DEL reaches
   zero outside cycle collection; resurrects it into
   QPState.pending_del and returns TRUE (the free is cancelled).  Runs at
   most once per object (JSObject.py_finalized). */
static BOOL qp_del_hook(JSRuntime *rt, JSObject *p);
/* Run queued __del__ methods (errors reported as unraisable).  Called by
   the interpreter at the drain points of m2-design.md. */
static void qp_run_pending_finalizers(JSContext *ctx);

/* ========================================================================= */
/* (i) EXCEPTIONS AND TRACEBACKS: python/exceptions.c, python/traceback.c,    */
/*     python/errors.c                                                        */
/* ========================================================================= */

static int qp_init_exceptions(JSContext *ctx); /* QP_EXCEPTIONS types, traceback type */

/* PyErr_Format: raise type(message) with message from qp_str_from_format.
   Sets __context__ from the handled exception.  Always JS_EXCEPTION. */
static JSValue qp_raise(JSContext *ctx, QPType *type, const char *fmt, ...);
/* PyErr_SetObject: raise type(value) (value None: no args; a tuple: *value;
   an instance of type: itself). */
static JSValue qp_raise_value(JSContext *ctx, QPType *type, JSValueConst value);
/* Raise an exception instance (consumes exc). */
static JSValue qp_raise_object(JSContext *ctx, JSValue exc);
#define qp_raise_type_error(ctx, ...)  qp_raise((ctx), QP_EXC(ctx, TypeError), __VA_ARGS__)
#define qp_raise_value_error(ctx, ...) qp_raise((ctx), QP_EXC(ctx, ValueError), __VA_ARGS__)
#define qp_raise_index_error(ctx, ...) qp_raise((ctx), QP_EXC(ctx, IndexError), __VA_ARGS__)
#define qp_raise_overflow_error(ctx, ...) qp_raise((ctx), QP_EXC(ctx, OverflowError), __VA_ARGS__)
#define qp_raise_runtime_error(ctx, ...) qp_raise((ctx), QP_EXC(ctx, RuntimeError), __VA_ARGS__)
#define qp_raise_system_error(ctx, ...) qp_raise((ctx), QP_EXC(ctx, SystemError), __VA_ARGS__)
#define qp_raise_zero_division(ctx, ...) qp_raise((ctx), QP_EXC(ctx, ZeroDivisionError), __VA_ARGS__)
/* KeyError(key) (args = (key,)). */
static JSValue qp_raise_key_error(JSContext *ctx, JSValueConst key);
/* AttributeError "'%T' object has no attribute '%U'" (or "type object
   '%N' has no attribute" for types) with name/obj set. */
static JSValue qp_raise_attribute_error(JSContext *ctx, JSValueConst obj, JSAtom name);
/* NameError "name '%U' is not defined" with name set. */
static JSValue qp_raise_name_error(JSContext *ctx, JSAtom name);
/* UnboundLocalError "cannot access local variable '%U' where it is not
   associated with a value" / NameError "cannot access free variable '%U'
   where it is not associated with a value in enclosing scope" (used by
   get_loc_check / get_var_ref_check when b->is_py). */
static JSValue qp_raise_unbound(JSContext *ctx, JSAtom name, BOOL is_free);
static JSValue qp_raise_stop_iteration(JSContext *ctx, JSValueConst value);
static JSValue qp_raise_memory_error(JSContext *ctx);
static JSValue qp_raise_recursion_error(JSContext *ctx);       /* "maximum recursion depth exceeded" */
/* OSError subclass chosen by errno (FileNotFoundError, ...), with
   errno/strerror/filename. */
static JSValue qp_raise_os_error(JSContext *ctx, int errnum, JSValueConst filename);
/* SyntaxError & co. from a front-end error (kind -> exception type, msg,
   filename, lineno, offset, text, end_lineno, end_offset). */
static JSValue qp_raise_qp_error(JSContext *ctx, const qp_error *err, const char *filename);
/* UnicodeDecodeError / UnicodeEncodeError with all fields. */
static JSValue qp_raise_unicode_decode(JSContext *ctx, const char *encoding, const char *data,
                                       Py_ssize_t len, Py_ssize_t start, Py_ssize_t end,
                                       const char *reason);
static JSValue qp_raise_unicode_encode(JSContext *ctx, const char *encoding, JSValueConst str,
                                       Py_ssize_t start, Py_ssize_t end, const char *reason);

static BOOL qp_exc_check(JSContext *ctx, JSValueConst v);      /* BaseException instance */
static BOOL qp_exc_type_check(JSValueConst v);                 /* BaseException subclass */
/* Instance of an exception type (fast path for builtin types). */
static JSValue qp_exc_new(JSContext *ctx, QPType *type, int nargs, JSValueConst *args);
/* Convert any thrown value to a Python exception (consumes thrown):
   Python exceptions unchanged; JS InternalError "stack overflow" ->
   RecursionError("maximum recursion depth exceeded"); out of memory ->
   MemoryError; anything else -> js.JsError holding the value (D3). */
static JSValue qp_exc_normalize(JSContext *ctx, JSValue thrown);
/* PyErr_GetRaisedException: take the pending exception, normalized. */
static JSValue qp_exc_fetch(JSContext *ctx);
/* PyErr_ExceptionMatches on the pending exception (normalizes it in place). */
static BOOL qp_exc_pending_matches(JSContext *ctx, QPType *type);
/* PyErr_GivenExceptionMatches (type or tuple of types; no validation). */
static BOOL qp_exc_given_matches(JSContext *ctx, JSValueConst exc, JSValueConst type_or_tuple);
/* check_exc_match subop (validates the classes, TypeError "catching
   classes that do not inherit from BaseException is not allowed"). */
static int qp_exc_check_match(JSContext *ctx, JSValueConst exc, JSValueConst type_or_tuple);
/* check_eg_match subop (CPython exception_group_match): *pmatch, *prest. */
static int qp_exc_group_match(JSContext *ctx, JSValueConst exc, JSValueConst match_type,
                              JSValue *pmatch, JSValue *prest);
/* INTRINSIC_PREP_RERAISE_STAR. */
static JSValue qp_exc_prep_reraise_star(JSContext *ctx, JSValueConst orig, JSValueConst excs);
/* raise subop (CPython do_raise): exc and cause may be QP_NULL (count < 2
   / bare raise).  Always returns -1 with the exception thrown. */
static int qp_do_raise(JSContext *ctx, JSValueConst exc, JSValueConst cause);

/* Handled-exception stack (sys.exception(), bare raise, __context__). */
static JSValue qp_exc_info_get(JSContext *ctx);                /* new ref, None if none */
/* push_exc_info: *prev = previous handled exception (new ref, may be None). */
static void qp_exc_info_push(JSContext *ctx, JSValueConst exc, JSValue *prev);
/* pop_except: restore (consumes prev). */
static void qp_exc_info_pop(JSContext *ctx, JSValue prev);

/* Interpreter hook at the `exception:` label of a Python frame (not for
   reraise-like subops, nor uncatchable/suspension exceptions): normalizes
   the pending exception, chains __context__ if it was just converted from
   a JS value, and appends the (func, pc) traceback entry. */
static void qp_exc_frame_unwind(JSContext *ctx, JSValueConst func, uint32_t pc);
/* __traceback__ (materialized from the recorded entries), None if none. */
static JSValue qp_exc_get_traceback(JSContext *ctx, JSValueConst exc);
static int qp_exc_set_traceback(JSContext *ctx, JSValueConst exc, JSValueConst tb);
/* StopIteration.value (new ref; None for other exceptions). */
static JSValue qp_exc_stopiteration_value(JSContext *ctx, JSValueConst exc);
/* Display an exception like CPython's C PyErr_Display (traceback with
   source lines and carets, chains, notes, groups) to file (QP_NULL =
   sys.stderr, or C stderr if unusable).  0 or -1. */
static int qp_exc_display(JSContext *ctx, JSValueConst exc, JSValueConst file);
/* sys.unraisablehook-style report ("Exception ignored in: %R"). */
static void qp_exc_write_unraisable(JSContext *ctx, JSValueConst exc, JSValueConst obj);
/* warnings.warn in C (CPython default filters; printed to stderr as
   "%s:%d: %s: %s"). */
static int qp_warn(JSContext *ctx, QPType *category, int stacklevel, const char *fmt, ...);

/* ========================================================================= */
/* (j) GENERATORS, COROUTINES, ASYNC GENERATORS: python/genobject.c           */
/* ========================================================================= */

static int qp_init_generators(JSContext *ctx);

/* Called by the interpreter instead of running a Python function whose
   code has CO_GENERATOR / CO_COROUTINE / CO_ASYNC_GENERATOR: creates the
   heap frame (async_func_init, locals unbound) from the bound arguments
   (arg_buf[0..b->arg_count), consumed) and returns the
   generator/coroutine/async generator; nothing runs until the first
   send/next (lazy start, D3). */
static JSValue qp_gen_new(JSContext *ctx, JSValueConst func, JSValue *arg_buf);
static BOOL qp_gen_check(JSValueConst v);
static BOOL qp_coro_check(JSValueConst v);
/* PyIter_Send: generators/coroutines resumed directly, else send()/next()
   (*presult new ref: yielded or returned value). */
static int qp_iter_send(JSContext *ctx, JSValueConst recv, JSValueConst v, JSValue *presult);
/* get_awaitable / get_aiter / get_anext subops (CPython 3.15 messages). */
static JSValue qp_get_awaitable(JSContext *ctx, JSValueConst obj, int where);
static JSValue qp_get_aiter(JSContext *ctx, JSValueConst obj);
static JSValue qp_get_anext(JSContext *ctx, JSValueConst aiter);
/* INTRINSIC_STOPITERATION_ERROR: StopIteration (and StopAsyncIteration in
   async generators) -> RuntimeError with __cause__ (PEP 479); other
   exceptions returned unchanged.  co_flags of the running code. */
static JSValue qp_gen_stopiteration_error(JSContext *ctx, JSValueConst exc, uint32_t co_flags);
/* INTRINSIC_ASYNC_GEN_WRAP. */
static JSValue qp_asyncgen_wrap_value(JSContext *ctx, JSValueConst v);
/* tp_finalize of generator/coroutine/async_generator (close the suspended
   frame, "coroutine '%S' was never awaited" RuntimeWarning, asyncgen
   finalizer hook) and of asend/athrow ("coroutine method 'asend' of '%S'
   was never awaited").  Called by qp_run_pending_finalizers (h) for
   objects of class ids PY_GENERATOR, PY_COROUTINE, PY_ASYNC_GEN,
   PY_GEN_AUX instead of a __del__ lookup (their types set
   QP_TPFLAGS_HAS_DEL); errors are reported as unraisable inside. */
static void qp_gen_finalize(JSContext *ctx, JSValueConst obj);

/* ========================================================================= */
/* (k) BUILTINS, SYS, ARGUMENT PARSING, TYPING OBJECTS: python/bltinmodule.c, */
/*     python/sysmodule.c, python/getargs.c, python/typevarobject.c,          */
/*     python/genericaliasobject.c                                            */
/* ========================================================================= */

static int qp_init_builtins(JSContext *ctx);  /* builtin types of k, builtins and sys modules */

/* sys.displayhook(v) (INTRINSIC_PRINT); 0 or -1. */
static int qp_displayhook(JSContext *ctx, JSValueConst v);
/* sys.<name> or QP_NULL if missing (PySys_GetOptionalAttr). */
static JSValue qp_sys_get(JSContext *ctx, JSAtom name);
/* Write formatted text to sys.stdout / sys.stderr (PySys_WriteStdout:
   falls back to C stdio when the attribute is unusable). */
static void qp_sys_write_stdout(JSContext *ctx, const char *fmt, ...);
static void qp_sys_write_stderr(JSContext *ctx, const char *fmt, ...);

/* Argument parsing for builtin C methods (Argument Clinic
   _PyArg_UnpackKeywords subset).  out[i] = borrowed argument or QP_NULL
   when absent and optional. */
typedef struct QPArgSpec {
    const char *fname;               /* "name" for messages: "%s() ..." */
    const char *const *keywords;     /* parameter names, NULL-terminated; "" = positional-only */
    int min_args;                    /* required parameters (positional or keyword) */
    int max_pos;                     /* parameters accepted positionally */
    int varargs;                     /* nonzero: extra positionals allowed (returned via the caller) */
} QPArgSpec;
static int qp_arg_parse(JSContext *ctx, const QPArgSpec *spec, JSValueConst *args, int nargs,
                        JSValueConst kwnames, JSValueConst *out);
/* "%s expected at least %d argument(s), got %d" / "at most". */
static int qp_arg_check_positional(JSContext *ctx, const char *fname, int nargs, int min, int max);
/* "%s() takes no keyword arguments". */
static int qp_arg_no_keywords(JSContext *ctx, const char *fname, JSValueConst kwnames);
/* Converters (Argument Clinic): Py_ssize_t via __index__ (OverflowError),
   bool via truth value, int with range check. */
static int qp_arg_ssize(JSContext *ctx, JSValueConst v, Py_ssize_t *out);
static int qp_arg_bool(JSContext *ctx, JSValueConst v, int *out);
static int qp_arg_int(JSContext *ctx, JSValueConst v, int *out);

/* PEP 695 intrinsics (TYPEVAR, PARAMSPEC, TYPEVARTUPLE, SUBSCRIPT_GENERIC,
   TYPEALIAS / TYPEVAR_WITH_BOUND, TYPEVAR_WITH_CONSTRAINTS,
   SET_TYPEPARAM_DEFAULT). */
static JSValue qp_typing_intrinsic1(JSContext *ctx, int id, JSValueConst v);
static JSValue qp_typing_intrinsic2(JSContext *ctx, int id, JSValueConst a, JSValueConst b);
/* types.GenericAlias(origin, args) (list[int], __class_getitem__). */
static JSValue qp_generic_alias_new(JSContext *ctx, JSValueConst origin, JSValueConst args);
/* X | Y for types (types.UnionType / typing.Union in 3.14+). */
static JSValue qp_union_new(JSContext *ctx, JSValueConst a, JSValueConst b);

/* ========================================================================= */
/* (l) LAUNCHER, SCRIPT RUNNING, MODULES, IMPORT: python/quickpy.c,           */
/*     python/quickpy.h, qpy.c, python/pythonrun.c, python/moduleobject.c,    */
/*     python/import.c                                                        */
/* ========================================================================= */

static int qp_init_modules(JSContext *ctx);   /* module, lazy_import types; sys.modules */

static JSValue qp_module_new(JSContext *ctx, JSValueConst name);
static BOOL qp_module_check(JSContext *ctx, JSValueConst v);
static JSValue qp_module_get_dict(JSValueConst module);        /* borrowed */
/* IMPORT_NAME (CPython import_name): honours an overridden
   builtins.__import__; mode = QP_IMPORT_*. */
static JSValue qp_import_name(JSContext *ctx, JSValueConst name, JSValueConst globals,
                              JSValueConst locals, JSValueConst fromlist, int level, int mode);
/* IMPORT_FROM ("cannot import name %R from %R (%S)", submodule fallback). */
static JSValue qp_import_from(JSContext *ctx, JSValueConst module, JSAtom name);
/* INTRINSIC_IMPORT_STAR into the locals mapping. */
static int qp_import_star(JSContext *ctx, JSValueConst module, JSValueConst locals);
/* PyImport_ImportModule("a.b"). */
static JSValue qp_import_module(JSContext *ctx, const char *name);
/* PEP 810: value of a global after reification if it is a lazy-import
   object (consumes v), else v. */
static JSValue qp_lazy_import_resolve(JSContext *ctx, JSValue v);
static BOOL qp_lazy_import_check(JSValueConst v);
/* Builtin (native) module definition, CPython PyModuleDef + Modules/config.c
   inittab.  exec runs after the module object is created and its methods
   are added (PyModule_ExecDef); returns 0 or -1 with exception pending.
   Each native module file defines `static const QPModuleDef
   qp_moddef_<name>`; python/config.c lists them. */
typedef struct QPModuleDef {
    const char *name;               /* "posix", "_io", "sys", ... */
    const char *doc;                /* NULL = None */
    const QPMethodDef *methods;     /* NULL-terminated or NULL */
    int (*exec)(JSContext *ctx, JSValueConst module);
} QPModuleDef;
/* python/config.c (orchestrator-owned, included last): the inittab. */
static const QPModuleDef *qp_inittab_find(const char *name, size_t len);
static int qp_inittab_count(void);
static const QPModuleDef *qp_inittab_get(int i);   /* sys.builtin_module_names */
/* python/moduleobject.c (unit l): create + exec a builtin module from its def
   (adds methods as builtin functions bound to the module, sets __name__/__doc__). */
static JSValue qp_module_from_def(JSContext *ctx, const QPModuleDef *def);
/* Run source text as __main__-like code in globals (mode as qp_compile_ast);
   result of eval mode or None. */
static JSValue qp_run_string(JSContext *ctx, const char *src, size_t len, const char *filename,
                             int mode, JSValueConst globals, JSValueConst locals);
/* Uncaught exception at top level: SystemExit -> exit status, else
   sys.excepthook (fallback qp_exc_display); returns the process exit code. */
static int qp_handle_uncaught(JSContext *ctx);

/* ========================================================================= */
/* M3 NATIVE-MODULE RUNTIME HOOKS (owner M3Misc: python/modstate.h,           */
/*     python/mod_weakref.c, python/mod_misc.c)                               */
/* ========================================================================= */

struct QPModState;
/* python/modstate.h: called by qp_state_mark / qp_state_free (unit a). */
static void qp_modstate_mark(JSRuntime *rt, QPState *s, JS_MarkFunc *mark_func);
/* In qp_state_free, before the context values are freed; never runs Python code. */
static void qp_modstate_free(JSContext *ctx);
/* python/mod_weakref.c: called by quickjs.c free_object (after the __del__
   block) when p->weakref_count != 0; clears Python weakrefs to p, queues
   their callbacks, never runs code. */
static void qp_weakref_object_freed(JSRuntime *rt, JSObject *p);
/* python/mod_misc.c (CPython _Py_HandlePending): runs tripped Python signal
   handlers and queued weakref callbacks; called from the interpreter's
   periodic poll; 0, or -1 with a (catchable) Python exception pending. */
static int qp_signal_check(JSContext *ctx);
/* python/mod_misc.c: run atexit callbacks (errors reported as unraisable);
   called by the runner at exit after threading._shutdown. */
static void qp_atexit_run(JSContext *ctx);
/* python/mod_misc.c (_warnings port; errors.c qp_warn calls it):
   PyErr_WarnEx / warn_unicode (category NULL = RuntimeWarning; source
   QP_NULL or the ResourceWarning source); 0 or -1. */
static int qp_warnings_warn(JSContext *ctx, QPType *category, JSValueConst message,
                            Py_ssize_t stack_level, JSValueConst source);
/* PyErr_WarnExplicitObject (module/registry/source QP_NULL if absent); 0 or -1. */
static int qp_warnings_warn_explicit(JSContext *ctx, QPType *category, JSValueConst message,
                                     JSValueConst filename, int lineno, JSValueConst module,
                                     JSValueConst registry, JSValueConst source);

#ifdef CONFIG_PYTHON_CAPI
/* ========================================================================= */
/* (D5) C-API hooks (python/capi_*.c, plan D5 capi-design R5/R7).  Same     */
/* prototypes as python/capi.h; repeated here because quickjs.c and the M2  */
/* files call them before capi.h is included.                                */
/* ========================================================================= */

/* (A) quickjs.c GC / lifetime hooks */
static void qp_capi_gc_sync(JSRuntime *rt);
static void qp_capi_mark_node(JSRuntime *rt, JSGCObjectHeader *gp, JS_MarkFunc *mark_func);
static void qp_capi_mark_object(JSRuntime *rt, JSObject *p, JS_MarkFunc *mark_func);
static void qp_capi_free_cycle_node(JSRuntime *rt, JSGCObjectHeader *gp);
static void qp_capi_object_freed(JSRuntime *rt, JSObject *p);
static void qp_capi_context_free(JSContext *ctx);
/* (B) called by qp_type_modified (typeobject.c): refresh tp_flags and the
   slot pointers of t's PyTypeObject, run type watchers. */
static void qp_capi_type_modified(JSContext *ctx, QPType *t);
/* (B) R4: TRUE when two types with instances of JS_CLASS_PY_NATIVE have
   the same C instance layout (tp_basicsize, tp_itemsize), used by the
   __class__ / __bases__ assignment compatibility checks. */
static BOOL qp_capi_native_same_layout(QPType *a, QPType *b);
#endif /* CONFIG_PYTHON_CAPI */

/* ========================================================================= */
/* STRUCTSEQ (python/structseq.c, owner M3Posix)                              */
/* ========================================================================= */

typedef struct QPStructSeqDesc {
    const char *name;                 /* dotted: "os.stat_result" */
    const char *doc;
    const char *const *fields;        /* NULL-terminated; QP_STRUCTSEQ_UNNAMED for unnamed */
    const char *const *field_docs;    /* may be NULL */
    int n_in_sequence;
    int flags;                        /* QP_STRUCTSEQ_* */
} QPStructSeqDesc;
#define QP_STRUCTSEQ_DISALLOW_INSTANTIATION 0x1
/* "unnamed field" marker, like PyStructSequence_UnnamedField (compare by
   pointer).  Defined here: a static array cannot be declared incomplete. */
static const char QP_STRUCTSEQ_UNNAMED[] = "unnamed field";
/* New structseq type (tuple subclass with named field getters). */
static JSValue qp_structseq_new_type(JSContext *ctx, const QPStructSeqDesc *desc);
/* Instance of structseq type t; steals the n_fields values of items. */
static JSValue qp_structseq_new(JSContext *ctx, QPType *t, JSValue *items);

#endif /* QP_OBJECT_H */
#endif /* QP_CLASS */
