/*
 * _types (CPython Modules/_typesmodule.c): the built-in types that
 * Lib/types.py re-exports, and the _sysconfigdata_<abiflags>_<platform>_
 * <multiarch> module (CPython generates it at build time from the Makefile;
 * Lib/sysconfig reads its build_time_vars on POSIX).
 */

static int qp_types_add(JSContext *ctx, JSValueConst mod, const char *name, JSValueConst v)
{
    JSAtom a = JS_NewAtom(ctx, name);
    int r;

    if (a == JS_ATOM_NULL)
        return -1;
    r = qp_setattr(ctx, mod, a, v);
    JS_FreeAtom(ctx, a);
    return r;
}

static int qp_types_exec(JSContext *ctx, JSValueConst mod)
{
#define QP_TYPES_ADD(name, id)                                                \
    if (qp_types_add(ctx, mod, name, QP_TYPE_VALUE(QP_TYPE(ctx, id))) < 0)   \
        return -1;
    QP_TYPES_ADD("AsyncGeneratorType", async_generator)
    QP_TYPES_ADD("BuiltinFunctionType", builtin_function_or_method)
    QP_TYPES_ADD("BuiltinMethodType", builtin_function_or_method)
    QP_TYPES_ADD("CellType", cell)
    QP_TYPES_ADD("ClassMethodDescriptorType", classmethod_descriptor)
    QP_TYPES_ADD("CodeType", code)
    QP_TYPES_ADD("CoroutineType", coroutine)
    QP_TYPES_ADD("EllipsisType", ellipsis)
    QP_TYPES_ADD("FrameType", frame)
    /* frame.f_locals of an optimized frame is a dict snapshot (there is no
       FrameLocalsProxy type) */
    QP_TYPES_ADD("FrameLocalsProxyType", dict)
    QP_TYPES_ADD("FunctionType", function)
    QP_TYPES_ADD("GeneratorType", generator)
    QP_TYPES_ADD("GenericAlias", GenericAlias)
    QP_TYPES_ADD("GetSetDescriptorType", getset_descriptor)
    QP_TYPES_ADD("LambdaType", function)
    QP_TYPES_ADD("LazyImportType", lazy_import)
    QP_TYPES_ADD("MappingProxyType", mappingproxy)
    QP_TYPES_ADD("MemberDescriptorType", member_descriptor)
    QP_TYPES_ADD("MethodDescriptorType", method_descriptor)
    QP_TYPES_ADD("MethodType", method)
    QP_TYPES_ADD("MethodWrapperType", method_wrapper)
    QP_TYPES_ADD("ModuleType", module)
    QP_TYPES_ADD("NoneType", NoneType)
    QP_TYPES_ADD("NotImplementedType", NotImplementedType)
    QP_TYPES_ADD("SimpleNamespace", SimpleNamespace)
    QP_TYPES_ADD("TracebackType", traceback)
    QP_TYPES_ADD("UnionType", Union)
    QP_TYPES_ADD("WrapperDescriptorType", wrapper_descriptor)
#undef QP_TYPES_ADD
    /* CapsuleType is not exported: capsules belong to the C API layer,
       which is initialised only when a C extension module is first
       imported (CPython's types.py fallback has no CapsuleType either). */
    return 0;
}

static const QPModuleDef qp_moddef__types = {
    "_types",
    "Define names for built-in types.",
    NULL,
    qp_types_exec,
};

/* ---- _sysconfigdata ------------------------------------------------------ */

#ifdef QP_MULTIARCH
#define QP_SYSCONFIGDATA_NAME "_sysconfigdata__" QP_PLATFORM "_" QP_MULTIARCH
#else
#define QP_SYSCONFIGDATA_NAME "_sysconfigdata__" QP_PLATFORM "_"
#endif

/* Extension modules: SOABI / EXT_SUFFIX name the first entry of
   _imp.extension_suffixes(), the Tier S ABI of source builds against
   capi/include (QP_SOABI, python/sysmodule.c; loaded by
   python/capi_dynload.c besides abi3/abi3t), as CPython's name its
   version-specific first entry.  INCLUDEPY is <prefix>/capi/include (set
   at exec time).  SHLIB_SUFFIX is ".so" on macOS too (CPython's configure:
   the suffix of loadable modules, not of dylibs). */
#define QP_SHLIB_SUFFIX ".so"
#if defined(__APPLE__)
#define QP_LDSHARED_FLAGS " -bundle -undefined dynamic_lookup"
#else
#define QP_LDSHARED_FLAGS " -shared"
#endif

#if defined(__clang__)
#define QP_CC "clang"
#define QP_CXX "clang++"
#elif defined(__GNUC__)
#define QP_CC "gcc"
#define QP_CXX "g++"
#else
#define QP_CC "cc"
#define QP_CXX "c++"
#endif

#ifndef __has_feature
#define __has_feature(x) 0
#endif
/* sanitizer flags of this build (test.support.check_sanitizer reads them
   from CFLAGS / PY_CFLAGS / CONFIG_ARGS) */
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#define QP_SAN_ASAN " -fsanitize=address"
#else
#define QP_SAN_ASAN ""
#endif
#if __has_feature(memory_sanitizer)
#define QP_SAN_MSAN " -fsanitize=memory"
#else
#define QP_SAN_MSAN ""
#endif
#if __has_feature(thread_sanitizer) || defined(__SANITIZE_THREAD__)
#define QP_SAN_TSAN " -fsanitize=thread"
#else
#define QP_SAN_TSAN ""
#endif
#define QP_SAN_FLAGS QP_SAN_ASAN QP_SAN_MSAN QP_SAN_TSAN

static int qp_sysconfigdata_exec(JSContext *ctx, JSValueConst mod)
{
    static const struct { const char *name, *value; } strs[] = {
        { "ABIFLAGS", "" },
        /* setuptools' customize_compiler requires AR, ARFLAGS, CXX and
           LDCXXSHARED besides CC and LDSHARED */
        { "AR", "ar" },
        { "ARFLAGS", "rcs" },
        { "CC", QP_CC },
        { "CXX", QP_CXX },
        { "CCSHARED", "-fPIC" },
        { "CFLAGS", QP_SAN_FLAGS },
        { "CONFIG_ARGS", "" },
        { "CONFIGURE_CFLAGS", QP_SAN_FLAGS },
        { "EXE", "" },
        { "EXT_SUFFIX", "." QP_SOABI QP_SHLIB_SUFFIX },
        /* extensions are not linked against a libpython: they resolve the C
           API from the qpy executable (-undefined dynamic_lookup) */
        { "LIBPYTHON", "" },
        { "BLDSHARED", QP_CC QP_LDSHARED_FLAGS QP_SAN_FLAGS },
        { "LDSHARED", QP_CC QP_LDSHARED_FLAGS QP_SAN_FLAGS },
        { "LDCXXSHARED", QP_CXX QP_LDSHARED_FLAGS QP_SAN_FLAGS },
        { "LDFLAGS", QP_SAN_FLAGS },
        { "MACHDEP", QP_PLATFORM },
#ifdef QP_MULTIARCH
        { "MULTIARCH", QP_MULTIARCH },
#endif
        { "OPT", "" },
        { "PY_CFLAGS", QP_SAN_FLAGS },
        { "PY_CFLAGS_NODIST", "" },
        { "PY_CORE_CFLAGS", QP_SAN_FLAGS },
        { "PY_LDFLAGS", QP_SAN_FLAGS },
        { "PY_STDMODULE_CFLAGS", QP_SAN_FLAGS },
        { "SHLIB_SUFFIX", QP_SHLIB_SUFFIX },
        { "SOABI", QP_SOABI },
#ifdef QP_MULTIARCH
        { "SOABI_PLATFORM", QP_MULTIARCH },
#endif
        { "TZPATH", "/usr/share/zoneinfo:/usr/lib/zoneinfo:/usr/share/lib/zoneinfo:/etc/zoneinfo" },
        { "VERSION", "3.15" },
    };
    static const struct { const char *name; int value; } ints[] = {
        { "HAVE_FORK", 1 },
        { "Py_DEBUG", 0 },
        { "Py_ENABLE_SHARED", 0 },
        { "Py_GIL_DISABLED", 0 },
        { "Py_STATS", 0 },
        { "Py_TRACE_REFS", 0 },
        { "WITH_DOC_STRINGS", 1 },
        { "WITH_PYMALLOC", 0 },
    };
    JSValue d, v;
    JSAtom a;
    size_t i;
    int r;

    d = qp_dict_new(ctx);
    if (JS_IsException(d))
        return -1;
    for (i = 0; i < countof(strs); i++) {
        v = qp_str_from_cstr(ctx, strs[i].value);
        if (JS_IsException(v))
            goto fail;
        a = JS_NewAtom(ctx, strs[i].name);
        r = a == JS_ATOM_NULL ? -1 : qp_dict_set_item_atom(ctx, d, a, v);
        if (a != JS_ATOM_NULL)
            JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, v);
        if (r < 0)
            goto fail;
    }
    for (i = 0; i < countof(ints); i++) {
        a = JS_NewAtom(ctx, ints[i].name);
        r = a == JS_ATOM_NULL ? -1 :
            qp_dict_set_item_atom(ctx, d, a, JS_NewInt32(ctx, ints[i].value));
        if (a != JS_ATOM_NULL)
            JS_FreeAtom(ctx, a);
        if (r < 0)
            goto fail;
    }
    /* INCLUDEPY: the Tier S headers of this installation */
    a = JS_NewAtom(ctx, "base_prefix");
    if (a == JS_ATOM_NULL)
        goto fail;
    v = qp_sys_get(ctx, a);
    JS_FreeAtom(ctx, a);
    if (qp_str_check(ctx, v)) {
        JSValue sub = qp_str_from_cstr(ctx, "/capi/include"), inc;
        if (JS_IsException(sub)) {
            JS_FreeValue(ctx, v);
            goto fail;
        }
        inc = qp_str_concat(ctx, v, sub);
        JS_FreeValue(ctx, sub);
        JS_FreeValue(ctx, v);
        if (JS_IsException(inc))
            goto fail;
        a = JS_NewAtom(ctx, "INCLUDEPY");
        r = a == JS_ATOM_NULL ? -1 : qp_dict_set_item_atom(ctx, d, a, inc);
        if (a != JS_ATOM_NULL)
            JS_FreeAtom(ctx, a);
        JS_FreeValue(ctx, inc);
        if (r < 0)
            goto fail;
    } else {
        JS_FreeValue(ctx, v);
    }
    r = qp_types_add(ctx, mod, "build_time_vars", d);
    JS_FreeValue(ctx, d);
    return r;
 fail:
    JS_FreeValue(ctx, d);
    return -1;
}

static const QPModuleDef qp_moddef__sysconfigdata = {
    QP_SYSCONFIGDATA_NAME,
    NULL,
    NULL,
    qp_sysconfigdata_exec,
};
