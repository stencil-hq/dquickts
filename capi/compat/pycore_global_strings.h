/*
 * dquickts C API compat shim (plan D5; capi-design.md §3 and §10.2): the
 * subset of CPython 3.15 Include/internal/pycore_global_strings.h used by the vendored
 * modules (capi/modules).  Identifiers and string singletons are
 * immortal str objects of the runtime (python/capi_object.c, unit A), looked
 * up by name instead of CPython's static _PyRuntime tables.
 * Owner of this file: unit F.  PSF-2.0 for the declarations copied from
 * CPython.
 */
#ifndef Py_INTERNAL_GLOBAL_STRINGS_H
#define Py_INTERNAL_GLOBAL_STRINGS_H
#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

/* _Py_ID(NAME) is an lvalue of type PyObject, as in CPython: modules write
   &_Py_ID(name).  The object is interned and immortal. */
PyAPI_FUNC(PyObject *) _PyQp_InternId(const char *name);
PyAPI_FUNC(PyObject *) _PyQp_InternStr(const char *literal);
PyAPI_FUNC(PyObject *) _PyQp_Latin1Char(int ch);
PyAPI_FUNC(PyObject *) _PyQp_Singleton(int which);

#define _Py_ID(NAME) (*_PyQp_InternId(#NAME))
#define _Py_STR(NAME) (*_PyQp_InternStr(_Py_QP_STR_##NAME))
/* Marker for CPython's code generator (Tools/build/generate_global_objects.py). */
#define _Py_DECLARE_STR(name, str)
/* One-character str singletons U+0000..U+00FF (PyObject *). */
#define _Py_LATIN1_CHR(CH) _PyQp_Latin1Char((unsigned char)(CH))

/* _Py_SINGLETON(NAME): the forms the modules use.  Indexes match
   QP_CAPI_SINGLETON_* of python/capi.h. */
#define _Py_QP_SINGLETON_tuple_empty 0
#define _Py_QP_SINGLETON_bytes_empty 1
#define _Py_QP_SINGLETON_str_empty   2
#define _Py_SINGLETON(NAME) (*_PyQp_Singleton(_Py_QP_SINGLETON_##NAME))

/* Literals of CPython's STRUCT_FOR_STR table (pycore_global_strings.h). */
#define _Py_QP_STR_anon_dictcomp "<dictcomp>"
#define _Py_QP_STR_anon_genexpr "<genexpr>"
#define _Py_QP_STR_anon_lambda "<lambda>"
#define _Py_QP_STR_anon_listcomp "<listcomp>"
#define _Py_QP_STR_anon_module "<module>"
#define _Py_QP_STR_anon_null "<NULL>"
#define _Py_QP_STR_anon_setcomp "<setcomp>"
#define _Py_QP_STR_anon_string "<string>"
#define _Py_QP_STR_anon_unknown "<unknown>"
#define _Py_QP_STR_dbl_close_br "}}"
#define _Py_QP_STR_dbl_open_br "{{"
#define _Py_QP_STR_dbl_percent "%%"
#define _Py_QP_STR_defaults ".defaults"
#define _Py_QP_STR_dot_locals ".<locals>"
#define _Py_QP_STR_empty ""
#define _Py_QP_STR_format ".format"
#define _Py_QP_STR_gc "<GC>"
#define _Py_QP_STR_generic_base ".generic_base"
#define _Py_QP_STR_json_decoder "json.decoder"
#define _Py_QP_STR_kwdefaults ".kwdefaults"
#define _Py_QP_STR_list_err "list index out of range"
#define _Py_QP_STR_native "<native>"
#define _Py_QP_STR_str_replace_inf "1e309"
#define _Py_QP_STR_type_params ".type_params"
#define _Py_QP_STR_utf_8 "utf-8"

#endif /* !Py_INTERNAL_GLOBAL_STRINGS_H */
