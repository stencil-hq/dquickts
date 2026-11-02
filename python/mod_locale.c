/* _locale: port of CPython 3.15 Modules/_localemodule.c (POSIX only). */

#include <locale.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <wchar.h>
#include <langinfo.h>
#if defined(__has_include)
#  if __has_include(<libintl.h>)
#    include <libintl.h>
#    define QP_LOCALE_HAVE_LIBINTL 1
#  endif
#endif

#define QP_LOCALE_ERROR_PROP "\x01qp_locale_error"

/* PyUnicode_DecodeLocale(s, NULL): the locale codec is UTF-8 with
   surrogateescape here. */
static JSValue qp_locale_decode(JSContext *ctx, const char *s)
{
    return qp_str_decode(ctx, s, (Py_ssize_t)strlen(s), "utf-8", "surrogateescape");
}

/* _PyArg_BadArgument */
static JSValue qp_locale_bad_arg(JSContext *ctx, const char *fname, const char *argname,
                                 const char *expected, JSValueConst v)
{
    if (JS_IsUndefined(v) || JS_IsNull(v))
        return qp_raise_type_error(ctx, "%.200s() %.200s must be %.50s, not None",
                                   fname, argname, expected);
    return qp_raise_type_error(ctx, "%.200s() %.200s must be %.50s, not %T",
                               fname, argname, expected, v);
}

/* Clinic str converter (PyUnicode_AsUTF8AndSize + embedded NUL check).
   *out is freed with JS_FreeCString.  Returns 0 or -1. */
static int qp_locale_arg_str(JSContext *ctx, const char *fname, const char *argname,
                             JSValueConst v, BOOL accept_none, const char **out)
{
    size_t len;
    const char *s;
    if (accept_none && JS_IsUndefined(v)) {
        *out = NULL;
        return 0;
    }
    if (!qp_str_check(ctx, v)) {
        qp_locale_bad_arg(ctx, fname, argname, accept_none ? "str or None" : "str", v);
        return -1;
    }
    s = qp_str_utf8(ctx, v, &len);
    if (!s)
        return -1;
    if (strlen(s) != len) {
        JS_FreeCString(ctx, s);
        qp_raise_value_error(ctx, "embedded null character");
        return -1;
    }
    *out = s;
    return 0;
}

static JSValue qp_locale_raise_error(JSContext *ctx, JSValueConst module, const char *msg)
{
    JSValue tv = JS_GetPropertyStr(ctx, module, QP_LOCALE_ERROR_PROP);
    QPType *t;
    JSValue r;
    if (JS_IsException(tv))
        return JS_EXCEPTION;
    t = qp_type_from_value(tv);
    if (!t) {
        JS_FreeValue(ctx, tv);
        return qp_raise_system_error(ctx, "_locale module state is missing");
    }
    r = qp_raise(ctx, t, "%s", msg);
    JS_FreeValue(ctx, tv);
    return r;
}

static int qp_locale_dict_set(JSContext *ctx, JSValueConst d, const char *key, JSValue v)
{
    JSAtom at;
    int r;
    if (JS_IsException(v))
        return -1;
    at = JS_NewAtom(ctx, key);
    if (at == JS_ATOM_NULL) {
        JS_FreeValue(ctx, v);
        return -1;
    }
    r = qp_dict_set_item_atom(ctx, d, at, v);
    JS_FreeAtom(ctx, at);
    JS_FreeValue(ctx, v);
    return r;
}

/* the grouping is terminated by either 0 or CHAR_MAX */
static JSValue qp_locale_copy_grouping(JSContext *ctx, const char *s)
{
    JSValue result;
    int i;
    if (s[0] == '\0')
        return qp_list_new(ctx, 0);
    result = qp_list_new(ctx, 0);
    if (JS_IsException(result))
        return result;
    i = -1;
    do {
        JSValue val;
        i++;
        val = qp_long_from_int64(ctx, (long)s[i]);
        if (JS_IsException(val) || qp_list_append(ctx, result, val) < 0) {
            JS_FreeValue(ctx, val);
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
        JS_FreeValue(ctx, val);
    } while (s[i] != '\0' && s[i] != CHAR_MAX);
    return result;
}

static JSValue qp_locale_setlocale(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    int category;
    const char *locale = NULL;
    char *result;
    JSValue r;

    if (qp_arg_check_positional(ctx, "setlocale", nargs, 1, 2) < 0)
        return JS_EXCEPTION;
    if (qp_arg_int(ctx, args[0], &category) < 0)
        return JS_EXCEPTION;
    if (nargs >= 2 &&
        qp_locale_arg_str(ctx, "setlocale", "argument 2", args[1], TRUE, &locale) < 0)
        return JS_EXCEPTION;

    if (locale) {
        /* set locale */
        result = setlocale(category, locale);
        JS_FreeCString(ctx, locale);
        if (!result) {
            /* operation failed, no setting was changed */
            return qp_locale_raise_error(ctx, self, "unsupported locale setting");
        }
        r = qp_locale_decode(ctx, result);
    } else {
        /* get locale */
        result = setlocale(category, NULL);
        if (!result)
            return qp_locale_raise_error(ctx, self, "locale query failed");
        r = qp_locale_decode(ctx, result);
    }
    return r;
}

static int qp_locale_is_ascii1(const char *str)
{
    return (strlen(str) == 1 && ((unsigned char)str[0]) <= 127);
}

static int qp_locale_is_all_ascii(const char *str)
{
    for (; *str; str++) {
        if ((unsigned char)*str > 127)
            return 0;
    }
    return 1;
}

static char *qp_locale_strdup(JSContext *ctx, const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = js_malloc(ctx, n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/* Set LC_CTYPE temporarily to the locale of `category` when `need`
   (and the locales differ).  *oldloc: saved LC_CTYPE to restore, or NULL.
   rterr: exception type for the "failed to get" error. */
static int qp_locale_switch_ctype(JSContext *ctx, int category, int need, QPType *rterr,
                                  char **oldloc)
{
    char *loc;
    *oldloc = NULL;
    if (!need)
        return 0;
    loc = setlocale(LC_CTYPE, NULL);
    if (!loc) {
        qp_raise(ctx, rterr, "failed to get LC_CTYPE locale");
        return -1;
    }
    *oldloc = qp_locale_strdup(ctx, loc);
    if (!*oldloc) {
        qp_raise_memory_error(ctx);
        return -1;
    }
    loc = setlocale(category, NULL);
    if (loc == NULL || strcmp(loc, *oldloc) == 0) {
        js_free(ctx, *oldloc);
        *oldloc = NULL;
        return 0;
    }
    setlocale(LC_CTYPE, loc);
    return 1;
}

static void qp_locale_restore_ctype(JSContext *ctx, char *oldloc)
{
    if (oldloc != NULL) {
        setlocale(LC_CTYPE, oldloc);
        js_free(ctx, oldloc);
    }
}

static JSValue qp_locale_localeconv(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    JSValue result;
    struct lconv *lc;
    char *oldloc;
    int need;

    result = qp_dict_new(ctx);
    if (JS_IsException(result))
        return result;

    lc = localeconv();

#define QP_LC_STR(ATTR) \
    if (qp_locale_dict_set(ctx, result, #ATTR, qp_locale_decode(ctx, lc->ATTR)) < 0) goto fail_restore
#define QP_LC_INT(ATTR) \
    if (qp_locale_dict_set(ctx, result, #ATTR, qp_long_from_int64(ctx, (long)lc->ATTR)) < 0) goto failed

    /* Monetary information: LC_MONETARY encoding */
    need = (!qp_locale_is_ascii1(lc->int_curr_symbol)
            || !qp_locale_is_ascii1(lc->currency_symbol)
            || !qp_locale_is_ascii1(lc->mon_decimal_point)
            || !qp_locale_is_ascii1(lc->mon_thousands_sep));
    if (qp_locale_switch_ctype(ctx, LC_MONETARY, need, QP_EXC(ctx, RuntimeWarning), &oldloc) < 0)
        goto failed;
    QP_LC_STR(int_curr_symbol);
    QP_LC_STR(currency_symbol);
    QP_LC_STR(mon_decimal_point);
    QP_LC_STR(mon_thousands_sep);
    qp_locale_restore_ctype(ctx, oldloc);

    if (qp_locale_dict_set(ctx, result, "mon_grouping",
                           qp_locale_copy_grouping(ctx, lc->mon_grouping)) < 0)
        goto failed;

    oldloc = NULL;
    QP_LC_STR(positive_sign);
    QP_LC_STR(negative_sign);
    QP_LC_INT(int_frac_digits);
    QP_LC_INT(frac_digits);
    QP_LC_INT(p_cs_precedes);
    QP_LC_INT(p_sep_by_space);
    QP_LC_INT(n_cs_precedes);
    QP_LC_INT(n_sep_by_space);
    QP_LC_INT(p_sign_posn);
    QP_LC_INT(n_sign_posn);

    /* Numeric information: LC_NUMERIC encoding (_Py_GetLocaleconvNumeric) */
    need = (strlen(lc->decimal_point) > 1 || ((unsigned char)lc->decimal_point[0]) > 127
            || strlen(lc->thousands_sep) > 1 || ((unsigned char)lc->thousands_sep[0]) > 127);
    if (qp_locale_switch_ctype(ctx, LC_NUMERIC, need, QP_EXC(ctx, RuntimeWarning), &oldloc) < 0)
        goto failed;
    QP_LC_STR(decimal_point);
    QP_LC_STR(thousands_sep);
    qp_locale_restore_ctype(ctx, oldloc);

    if (qp_locale_dict_set(ctx, result, "grouping",
                           qp_locale_copy_grouping(ctx, lc->grouping)) < 0)
        goto failed;
    return result;

fail_restore:
    qp_locale_restore_ctype(ctx, oldloc);
failed:
    JS_FreeValue(ctx, result);
    return JS_EXCEPTION;
#undef QP_LC_STR
#undef QP_LC_INT
}

/* PyUnicode_AsWideCharString (wchar_t is UCS-4 on the POSIX targets). */
static wchar_t *qp_locale_to_wchar(JSContext *ctx, JSValueConst s, Py_ssize_t *plen)
{
    qp_strview v;
    wchar_t *w;
    Py_ssize_t i;
    if (qp_str_view_acquire(ctx, s, &v) < 0)
        return NULL;
    w = js_malloc(ctx, sizeof(wchar_t) * (size_t)(v.len + 1));
    if (!w) {
        qp_str_view_release(ctx, &v);
        return NULL;
    }
    for (i = 0; i < v.len; i++) {
        uint32_t c;
        if (v.kind == 1)
            c = ((const uint8_t *)v.data)[i];
        else if (v.kind == 2)
            c = ((const uint16_t *)v.data)[i];
        else
            c = ((const uint32_t *)v.data)[i];
        w[i] = (wchar_t)c;
    }
    w[v.len] = 0;
    if (plen)
        *plen = v.len;
    qp_str_view_release(ctx, &v);
    return w;
}

static JSValue qp_locale_strcoll(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    wchar_t *ws1, *ws2;
    JSValue r;
    if (qp_arg_check_positional(ctx, "strcoll", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (!qp_str_check(ctx, args[0]))
        return qp_locale_bad_arg(ctx, "strcoll", "argument 1", "str", args[0]);
    if (!qp_str_check(ctx, args[1]))
        return qp_locale_bad_arg(ctx, "strcoll", "argument 2", "str", args[1]);
    ws1 = qp_locale_to_wchar(ctx, args[0], NULL);
    if (!ws1)
        return JS_EXCEPTION;
    ws2 = qp_locale_to_wchar(ctx, args[1], NULL);
    if (!ws2) {
        js_free(ctx, ws1);
        return JS_EXCEPTION;
    }
    r = qp_long_from_int64(ctx, wcscoll(ws1, ws2));
    js_free(ctx, ws1);
    js_free(ctx, ws2);
    return r;
}

static JSValue qp_locale_strxfrm(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    JSValueConst str = args[0];
    Py_ssize_t n1;
    wchar_t *s, *buf = NULL;
    wchar_t dummy[1];
    uint32_t *buf2 = NULL;
    size_t n2, n3, i, j;
    JSValue result = JS_EXCEPTION;

    if (!qp_str_check(ctx, str))
        return qp_locale_bad_arg(ctx, "strxfrm", "argument", "str", str);
    s = qp_locale_to_wchar(ctx, str, &n1);
    if (!s)
        return JS_EXCEPTION;
    if (wcslen(s) != (size_t)n1) {
        qp_raise_value_error(ctx, "embedded null character");
        goto exit;
    }
    errno = 0;
    n2 = wcsxfrm(dummy, s, 1);
    if (errno && errno != ERANGE) {
        qp_raise_os_error(ctx, errno, QP_NULL);
        goto exit;
    }
    buf = js_malloc(ctx, sizeof(wchar_t) * (n2 + 1));
    if (!buf)
        goto exit;
    errno = 0;
    n2 = wcsxfrm(buf, s, n2 + 1);
    if (errno) {
        qp_raise_os_error(ctx, errno, QP_NULL);
        goto exit;
    }
    /* Codes larger than 0x10000 are encoded as a pair (highest 16 bits
       | 0x10000, lowest 16 bits) to preserve lexicographical order. */
    n3 = 0;
    for (i = 0; i < n2; i++) {
        if ((uint32_t)buf[i] > 0x10000u)
            n3++;
    }
    n3 += n2;
    buf2 = js_malloc(ctx, sizeof(uint32_t) * (n3 ? n3 : 1));
    if (!buf2)
        goto exit;
    j = 0;
    for (i = 0; i < n2; i++) {
        uint32_t c = (uint32_t)buf[i];
        if (c > 0x10000u) {
            buf2[j++] = (c >> 16) | 0x10000u;
            buf2[j++] = c & 0xFFFFu;
        } else {
            buf2[j++] = c;
        }
    }
    result = qp_str_from_ucs4(ctx, buf2, n3);
exit:
    js_free(ctx, buf2);
    js_free(ctx, buf);
    js_free(ctx, s);
    return result;
}

#define QP_LANGINFO(X, Y) { #X, X, Y }
static const struct qp_langinfo_constant {
    const char *name;
    int value;
    int category;
} qp_langinfo_constants[] = {
    /* These constants should exist on any langinfo implementation */
    QP_LANGINFO(DAY_1, LC_TIME),
    QP_LANGINFO(DAY_2, LC_TIME),
    QP_LANGINFO(DAY_3, LC_TIME),
    QP_LANGINFO(DAY_4, LC_TIME),
    QP_LANGINFO(DAY_5, LC_TIME),
    QP_LANGINFO(DAY_6, LC_TIME),
    QP_LANGINFO(DAY_7, LC_TIME),

    QP_LANGINFO(ABDAY_1, LC_TIME),
    QP_LANGINFO(ABDAY_2, LC_TIME),
    QP_LANGINFO(ABDAY_3, LC_TIME),
    QP_LANGINFO(ABDAY_4, LC_TIME),
    QP_LANGINFO(ABDAY_5, LC_TIME),
    QP_LANGINFO(ABDAY_6, LC_TIME),
    QP_LANGINFO(ABDAY_7, LC_TIME),

    QP_LANGINFO(MON_1, LC_TIME),
    QP_LANGINFO(MON_2, LC_TIME),
    QP_LANGINFO(MON_3, LC_TIME),
    QP_LANGINFO(MON_4, LC_TIME),
    QP_LANGINFO(MON_5, LC_TIME),
    QP_LANGINFO(MON_6, LC_TIME),
    QP_LANGINFO(MON_7, LC_TIME),
    QP_LANGINFO(MON_8, LC_TIME),
    QP_LANGINFO(MON_9, LC_TIME),
    QP_LANGINFO(MON_10, LC_TIME),
    QP_LANGINFO(MON_11, LC_TIME),
    QP_LANGINFO(MON_12, LC_TIME),

    QP_LANGINFO(ABMON_1, LC_TIME),
    QP_LANGINFO(ABMON_2, LC_TIME),
    QP_LANGINFO(ABMON_3, LC_TIME),
    QP_LANGINFO(ABMON_4, LC_TIME),
    QP_LANGINFO(ABMON_5, LC_TIME),
    QP_LANGINFO(ABMON_6, LC_TIME),
    QP_LANGINFO(ABMON_7, LC_TIME),
    QP_LANGINFO(ABMON_8, LC_TIME),
    QP_LANGINFO(ABMON_9, LC_TIME),
    QP_LANGINFO(ABMON_10, LC_TIME),
    QP_LANGINFO(ABMON_11, LC_TIME),
    QP_LANGINFO(ABMON_12, LC_TIME),

#ifdef RADIXCHAR
    /* The following are not available with glibc 2.0 */
    QP_LANGINFO(RADIXCHAR, LC_NUMERIC),
    QP_LANGINFO(THOUSEP, LC_NUMERIC),
    QP_LANGINFO(CRNCYSTR, LC_MONETARY),
#endif

    QP_LANGINFO(D_T_FMT, LC_TIME),
    QP_LANGINFO(D_FMT, LC_TIME),
    QP_LANGINFO(T_FMT, LC_TIME),
    QP_LANGINFO(AM_STR, LC_TIME),
    QP_LANGINFO(PM_STR, LC_TIME),

#ifdef CODESET
    QP_LANGINFO(CODESET, LC_CTYPE),
#endif
#ifdef T_FMT_AMPM
    QP_LANGINFO(T_FMT_AMPM, LC_TIME),
#endif
#ifdef ERA
    QP_LANGINFO(ERA, LC_TIME),
#endif
#ifdef ERA_D_FMT
    QP_LANGINFO(ERA_D_FMT, LC_TIME),
#endif
#ifdef ERA_D_T_FMT
    QP_LANGINFO(ERA_D_T_FMT, LC_TIME),
#endif
#ifdef ERA_T_FMT
    QP_LANGINFO(ERA_T_FMT, LC_TIME),
#endif
#ifdef ALT_DIGITS
    QP_LANGINFO(ALT_DIGITS, LC_TIME),
#endif
#ifdef YESEXPR
    QP_LANGINFO(YESEXPR, LC_MESSAGES),
#endif
#ifdef NOEXPR
    QP_LANGINFO(NOEXPR, LC_MESSAGES),
#endif
#ifdef _DATE_FMT
    /* This is not available in all glibc versions that have CODESET. */
    QP_LANGINFO(_DATE_FMT, LC_TIME),
#endif
    { 0, 0, 0 }
};
#undef QP_LANGINFO

#if defined(__GLIBC__) && (defined(ALT_DIGITS) || defined(ERA))
/* Convert a sequence of NUL-separated C strings to a str containing
   semicolon separated items. */
static JSValue qp_locale_decode_strings(JSContext *ctx, const char *result, size_t max_count)
{
    size_t i = 0, count = 0;
    char *buf;
    JSValue r;
    for (; count < max_count && result[i]; count++)
        i += strlen(result + i) + 1;
    buf = js_malloc(ctx, i);
    if (!buf)
        return JS_EXCEPTION;
    memcpy(buf, result, i);
    i = 0;
    while (--count) {
        i += strlen(buf + i);
        buf[i++] = ';';
    }
    r = qp_locale_decode(ctx, buf);
    js_free(ctx, buf);
    return r;
}
#endif

static JSValue qp_locale_nl_langinfo(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    int item, i;
    if (qp_arg_int(ctx, args[0], &item) < 0)
        return JS_EXCEPTION;
    for (i = 0; qp_langinfo_constants[i].name; i++) {
        if (qp_langinfo_constants[i].value == item) {
            /* Check NULL as a workaround for GNU libc's returning NULL
               instead of an empty string for nl_langinfo(ERA).  */
            const char *result = nl_langinfo(item);
            char *oldloc = NULL;
            JSValue pyresult;
            int need;
            result = result != NULL ? result : "";
            need = qp_langinfo_constants[i].category != LC_CTYPE && *result && (
#ifdef __GLIBC__
#  ifdef ALT_DIGITS
                item == ALT_DIGITS ||
#  endif
#  ifdef ERA
                item == ERA ||
#  endif
#endif
                !qp_locale_is_all_ascii(result));
            if (qp_locale_switch_ctype(ctx, qp_langinfo_constants[i].category, need,
                                       QP_EXC(ctx, RuntimeError), &oldloc) < 0)
                return JS_EXCEPTION;
#ifdef __GLIBC__
#ifdef ALT_DIGITS
            if (item == ALT_DIGITS && *result) {
                pyresult = qp_locale_decode_strings(ctx, result, 100);
            }
            else
#endif
#ifdef ERA
            if (item == ERA && *result) {
                pyresult = qp_locale_decode_strings(ctx, result, SIZE_MAX);
            }
            else
#endif
#endif
            {
                pyresult = qp_locale_decode(ctx, result);
            }
            qp_locale_restore_ctype(ctx, oldloc);
            return pyresult;
        }
    }
    return qp_raise_value_error(ctx, "unsupported langinfo constant");
}

#ifdef QP_LOCALE_HAVE_LIBINTL

static JSValue qp_locale_gettext(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                 int nargs, JSValueConst kwnames, int magic)
{
    const char *in;
    JSValue r;
    if (qp_locale_arg_str(ctx, "gettext", "argument", args[0], FALSE, &in) < 0)
        return JS_EXCEPTION;
    r = qp_locale_decode(ctx, gettext(in));
    JS_FreeCString(ctx, in);
    return r;
}

static JSValue qp_locale_dgettext(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                  int nargs, JSValueConst kwnames, int magic)
{
    const char *domain, *in;
    JSValue r;
    if (qp_arg_check_positional(ctx, "dgettext", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (qp_locale_arg_str(ctx, "dgettext", "argument 1", args[0], TRUE, &domain) < 0)
        return JS_EXCEPTION;
    if (qp_locale_arg_str(ctx, "dgettext", "argument 2", args[1], FALSE, &in) < 0) {
        if (domain)
            JS_FreeCString(ctx, domain);
        return JS_EXCEPTION;
    }
    r = qp_locale_decode(ctx, dgettext(domain, in));
    if (domain)
        JS_FreeCString(ctx, domain);
    JS_FreeCString(ctx, in);
    return r;
}

static JSValue qp_locale_dcgettext(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                   int nargs, JSValueConst kwnames, int magic)
{
    const char *domain, *msgid;
    int category;
    JSValue r;
    if (qp_arg_check_positional(ctx, "dcgettext", nargs, 3, 3) < 0)
        return JS_EXCEPTION;
    if (qp_locale_arg_str(ctx, "dcgettext", "argument 1", args[0], TRUE, &domain) < 0)
        return JS_EXCEPTION;
    if (qp_locale_arg_str(ctx, "dcgettext", "argument 2", args[1], FALSE, &msgid) < 0) {
        if (domain)
            JS_FreeCString(ctx, domain);
        return JS_EXCEPTION;
    }
    if (qp_arg_int(ctx, args[2], &category) < 0) {
        r = JS_EXCEPTION;
    } else {
        r = qp_locale_decode(ctx, dcgettext(domain, msgid, category));
    }
    if (domain)
        JS_FreeCString(ctx, domain);
    JS_FreeCString(ctx, msgid);
    return r;
}

static JSValue qp_locale_textdomain(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                    int nargs, JSValueConst kwnames, int magic)
{
    const char *domain, *res;
    if (qp_locale_arg_str(ctx, "textdomain", "argument", args[0], TRUE, &domain) < 0)
        return JS_EXCEPTION;
    res = textdomain(domain);
    if (domain)
        JS_FreeCString(ctx, domain);
    if (!res)
        return qp_raise_os_error(ctx, errno, QP_NULL);
    return qp_locale_decode(ctx, res);
}

/* PyUnicode_FSConverter for str / bytes (os.PathLike via __fspath__). */
static JSValue qp_locale_fsconvert(JSContext *ctx, JSValueConst o)
{
    JSValue b;
    const uint8_t *data;
    Py_ssize_t len;
    if (qp_str_check(ctx, o)) {
        b = qp_str_encode(ctx, o, "utf-8", "surrogateescape");
    } else if (qp_bytes_check(ctx, o)) {
        b = JS_DupValue(ctx, o);
    } else {
        return qp_raise_type_error(ctx, "expected str, bytes or os.PathLike object, not %T", o);
    }
    if (JS_IsException(b))
        return b;
    data = qp_bytes_data(b, &len);
    if (memchr(data, 0, (size_t)len)) {
        JS_FreeValue(ctx, b);
        return qp_raise_value_error(ctx, "embedded null byte");
    }
    return b;
}

static JSValue qp_locale_bindtextdomain(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                        int nargs, JSValueConst kwnames, int magic)
{
    const char *domain, *dirname = NULL, *current;
    JSValue dirname_bytes = JS_UNDEFINED, r;
    if (qp_arg_check_positional(ctx, "bindtextdomain", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (qp_locale_arg_str(ctx, "bindtextdomain", "argument 1", args[0], FALSE, &domain) < 0)
        return JS_EXCEPTION;
    if (!strlen(domain)) {
        JS_FreeCString(ctx, domain);
        return qp_locale_raise_error(ctx, self, "domain must be a non-empty string");
    }
    if (!JS_IsUndefined(args[1])) {
        Py_ssize_t len;
        dirname_bytes = qp_locale_fsconvert(ctx, args[1]);
        if (JS_IsException(dirname_bytes)) {
            JS_FreeCString(ctx, domain);
            return JS_EXCEPTION;
        }
        dirname = (const char *)qp_bytes_data(dirname_bytes, &len);
    }
    current = bindtextdomain(domain, dirname);
    if (current == NULL)
        r = qp_raise_os_error(ctx, errno, QP_NULL);
    else
        r = qp_locale_decode(ctx, current);
    JS_FreeValue(ctx, dirname_bytes);
    JS_FreeCString(ctx, domain);
    return r;
}

static JSValue qp_locale_bind_textdomain_codeset(JSContext *ctx, JSValueConst self,
                                                 JSValueConst *args, int nargs,
                                                 JSValueConst kwnames, int magic)
{
    const char *domain, *codeset, *res;
    JSValue r;
    if (qp_arg_check_positional(ctx, "bind_textdomain_codeset", nargs, 2, 2) < 0)
        return JS_EXCEPTION;
    if (qp_locale_arg_str(ctx, "bind_textdomain_codeset", "argument 1", args[0], FALSE,
                          &domain) < 0)
        return JS_EXCEPTION;
    if (qp_locale_arg_str(ctx, "bind_textdomain_codeset", "argument 2", args[1], TRUE,
                          &codeset) < 0) {
        JS_FreeCString(ctx, domain);
        return JS_EXCEPTION;
    }
    res = bind_textdomain_codeset(domain, codeset);
    r = res ? qp_locale_decode(ctx, res) : JS_UNDEFINED;
    if (codeset)
        JS_FreeCString(ctx, codeset);
    JS_FreeCString(ctx, domain);
    return r;
}

#endif /* QP_LOCALE_HAVE_LIBINTL */

/* _Py_GetLocaleEncodingObject */
static JSValue qp_locale_getencoding(JSContext *ctx, JSValueConst self, JSValueConst *args,
                                     int nargs, JSValueConst kwnames, int magic)
{
    const char *encoding = nl_langinfo(CODESET);
    if (!encoding || encoding[0] == '\0') {
        /* Use UTF-8 if nl_langinfo() returns an empty string. It can happen
           on macOS if the LC_CTYPE locale is not supported. */
        return qp_str_from_cstr(ctx, "utf-8");
    }
    return qp_locale_decode(ctx, encoding);
}

static const QPMethodDef qp_locale_methods[] = {
    { "setlocale", qp_locale_setlocale, QP_METH_FASTCALL, 0,
      "setlocale($module, category, locale=None, /)\n"
      "--\n"
      "\n"
      "Activates/queries locale processing." },
    { "localeconv", qp_locale_localeconv, QP_METH_NOARGS, 0,
      "localeconv($module, /)\n"
      "--\n"
      "\n"
      "Returns numeric and monetary locale-specific parameters." },
    { "strcoll", qp_locale_strcoll, QP_METH_FASTCALL, 0,
      "strcoll($module, os1, os2, /)\n"
      "--\n"
      "\n"
      "Compares two strings according to the locale." },
    { "strxfrm", qp_locale_strxfrm, QP_METH_O, 0,
      "strxfrm($module, string, /)\n"
      "--\n"
      "\n"
      "Return a string that can be used as a key for locale-aware comparisons." },
    { "nl_langinfo", qp_locale_nl_langinfo, QP_METH_O, 0,
      "nl_langinfo($module, key, /)\n"
      "--\n"
      "\n"
      "Return the value for the locale information associated with key." },
#ifdef QP_LOCALE_HAVE_LIBINTL
    { "gettext", qp_locale_gettext, QP_METH_O, 0,
      "gettext($module, msg, /)\n"
      "--\n"
      "\n"
      "gettext(msg) -> string\n"
      "\n"
      "Return translation of msg." },
    { "dgettext", qp_locale_dgettext, QP_METH_FASTCALL, 0,
      "dgettext($module, domain, msg, /)\n"
      "--\n"
      "\n"
      "dgettext(domain, msg) -> string\n"
      "\n"
      "Return translation of msg in domain." },
    { "dcgettext", qp_locale_dcgettext, QP_METH_FASTCALL, 0,
      "dcgettext($module, domain, msg, category, /)\n"
      "--\n"
      "\n"
      "Return translation of msg in domain and category." },
    { "textdomain", qp_locale_textdomain, QP_METH_O, 0,
      "textdomain($module, domain, /)\n"
      "--\n"
      "\n"
      "Set the C library\'s textdmain to domain, returning the new domain." },
    { "bindtextdomain", qp_locale_bindtextdomain, QP_METH_FASTCALL, 0,
      "bindtextdomain($module, domain, dir, /)\n"
      "--\n"
      "\n"
      "Bind the C library\'s domain to dir." },
    { "bind_textdomain_codeset", qp_locale_bind_textdomain_codeset, QP_METH_FASTCALL, 0,
      "bind_textdomain_codeset($module, domain, codeset, /)\n"
      "--\n"
      "\n"
      "Bind the C library\'s domain to codeset." },
#endif
    { "getencoding", qp_locale_getencoding, QP_METH_NOARGS, 0,
      "getencoding($module, /)\n"
      "--\n"
      "\n"
      "Get the current locale encoding." },
    { NULL }
};

/* PyErr_NewException("locale.Error", NULL, NULL): subclass of Exception
   with __module__ "locale". */
static JSValue qp_locale_new_error_type(JSContext *ctx)
{
    JSValue name, bases, ns, mod, cls;
    JSValue b[1];

    b[0] = JS_DupValue(ctx, QP_TYPE_VALUE(QP_EXC(ctx, Exception)));
    bases = qp_tuple_new_steal(ctx, 1, b);
    if (JS_IsException(bases))
        return bases;
    ns = qp_dict_new(ctx);
    if (JS_IsException(ns)) {
        JS_FreeValue(ctx, bases);
        return ns;
    }
    mod = qp_str_from_cstr(ctx, "locale");
    name = qp_str_from_cstr(ctx, "Error");
    if (JS_IsException(mod) || JS_IsException(name) ||
        qp_dict_set_item_atom(ctx, ns, QP_ATOM(ctx, __module__), mod) < 0)
        cls = JS_EXCEPTION;
    else
        cls = qp_type_new(ctx, QP_TYPE(ctx, type), name, bases, ns, QP_NULL);
    JS_FreeValue(ctx, mod);
    JS_FreeValue(ctx, name);
    JS_FreeValue(ctx, ns);
    JS_FreeValue(ctx, bases);
    return cls;
}

static int qp_locale_exec(JSContext *ctx, JSValueConst module)
{
    JSValue err;
    int i;
#define QP_LOCALE_INT(NAME, VAL) \
    do { if (qp_locale_dict_set(ctx, qp_module_get_dict(module), (NAME), \
                                JS_NewInt32(ctx, (VAL))) < 0) return -1; } while (0)
    QP_LOCALE_INT("LC_CTYPE", LC_CTYPE);
    QP_LOCALE_INT("LC_TIME", LC_TIME);
    QP_LOCALE_INT("LC_COLLATE", LC_COLLATE);
    QP_LOCALE_INT("LC_MONETARY", LC_MONETARY);
#ifdef LC_MESSAGES
    QP_LOCALE_INT("LC_MESSAGES", LC_MESSAGES);
#endif
    QP_LOCALE_INT("LC_NUMERIC", LC_NUMERIC);
    QP_LOCALE_INT("LC_ALL", LC_ALL);
    QP_LOCALE_INT("CHAR_MAX", CHAR_MAX);

    err = qp_locale_new_error_type(ctx);
    if (JS_IsException(err))
        return -1;
    if (qp_locale_dict_set(ctx, qp_module_get_dict(module), "Error", JS_DupValue(ctx, err)) < 0) {
        JS_FreeValue(ctx, err);
        return -1;
    }
    if (JS_DefinePropertyValueStr(ctx, module, QP_LOCALE_ERROR_PROP, err, 0) < 0)
        return -1;

    for (i = 0; qp_langinfo_constants[i].name; i++)
        QP_LOCALE_INT(qp_langinfo_constants[i].name, qp_langinfo_constants[i].value);
#undef QP_LOCALE_INT
    return 0;
}

static const QPModuleDef qp_moddef__locale = {
    "_locale",
    "Support for POSIX locales.",
    qp_locale_methods,
    qp_locale_exec,
};
