/*
 * dquickts Python: argument parsing for builtin C functions and methods
 * (object.h section (k)).
 *
 * Derived from CPython Python/getargs.c (_PyArg_UnpackKeywords,
 * _PyArg_CheckPositional, _PyArg_NoKeywords) and Python/suggestions.c
 * (_Py_CalculateSuggestions) (PSF-2.0).
 *
 * Every function returns 0 on success and -1 with a pending exception.
 */

/* ---- keyword suggestions (Python/suggestions.c) ------------------------- */

#define QP_SUGGEST_MAX_CANDIDATE_ITEMS 750
#define QP_SUGGEST_MAX_STRING_SIZE 40
#define QP_SUGGEST_MOVE_COST 2
#define QP_SUGGEST_CASE_COST 1
#define QP_SUGGEST_LEAST_FIVE_BITS(n) ((n) & 31)

static inline int qp_suggest_substitution_cost(char a, char b)
{
    if (QP_SUGGEST_LEAST_FIVE_BITS(a) != QP_SUGGEST_LEAST_FIVE_BITS(b)) {
        /* Not the same, not a case flip. */
        return QP_SUGGEST_MOVE_COST;
    }
    if (a == b)
        return 0;
    if ('A' <= a && a <= 'Z')
        a += ('a' - 'A');
    if ('A' <= b && b <= 'Z')
        b += ('a' - 'A');
    if (a == b)
        return QP_SUGGEST_CASE_COST;
    return QP_SUGGEST_MOVE_COST;
}

/* Levenshtein distance between a and b with CPython's costs; returns
   max_cost + 1 as soon as the distance is known to exceed max_cost. */
static Py_ssize_t qp_suggest_levenshtein(const char *a, size_t a_size,
                                         const char *b, size_t b_size,
                                         size_t max_cost, size_t *buffer)
{
    size_t i, b_index, index, tmp, result;

    if (a == b && a_size == b_size)
        return 0;
    /* Trim away common affixes. */
    while (a_size && b_size && a[0] == b[0]) {
        a++; a_size--;
        b++; b_size--;
    }
    while (a_size && b_size && a[a_size - 1] == b[b_size - 1]) {
        a_size--;
        b_size--;
    }
    if (a_size == 0 || b_size == 0)
        return (Py_ssize_t)((a_size + b_size) * QP_SUGGEST_MOVE_COST);
    if (a_size > QP_SUGGEST_MAX_STRING_SIZE || b_size > QP_SUGGEST_MAX_STRING_SIZE)
        return (Py_ssize_t)(max_cost + 1);
    /* Prefer shorter buffer */
    if (b_size < a_size) {
        const char *t = a; a = b; b = t;
        size_t t_size = a_size; a_size = b_size; b_size = t_size;
    }
    /* quick fail when a match is impossible. */
    if ((b_size - a_size) * QP_SUGGEST_MOVE_COST > max_cost)
        return (Py_ssize_t)(max_cost + 1);

    tmp = QP_SUGGEST_MOVE_COST;
    for (i = 0; i < a_size; i++) {
        buffer[i] = tmp;
        tmp += QP_SUGGEST_MOVE_COST;
    }
    result = 0;
    for (b_index = 0; b_index < b_size; b_index++) {
        char code = b[b_index];
        size_t distance = result = b_index * QP_SUGGEST_MOVE_COST;
        size_t minimum = SIZE_MAX;
        for (index = 0; index < a_size; index++) {
            size_t substitute = distance + qp_suggest_substitution_cost(code, a[index]);
            size_t insert_delete;
            distance = buffer[index];
            insert_delete = (result < distance ? result : distance) + QP_SUGGEST_MOVE_COST;
            result = insert_delete < substitute ? insert_delete : substitute;
            buffer[index] = result;
            if (result < minimum)
                minimum = result;
        }
        if (minimum > max_cost) {
            /* Everything in this row is too big, so bail early. */
            return (Py_ssize_t)(max_cost + 1);
        }
    }
    return (Py_ssize_t)result;
}

/* _Py_CalculateSuggestions over C strings: index of the best candidate or
   -1 when none is close enough. */
static int qp_suggest_keyword(const char *name, const char *const *cands, int ncands)
{
    size_t buffer[QP_SUGGEST_MAX_STRING_SIZE];
    Py_ssize_t suggestion_distance = PY_SSIZE_T_MAX;
    size_t name_size = strlen(name);
    int i, suggestion = -1;

    if (ncands >= QP_SUGGEST_MAX_CANDIDATE_ITEMS)
        return -1;
    for (i = 0; i < ncands; i++) {
        const char *item = cands[i];
        size_t item_size = strlen(item);
        Py_ssize_t max_distance, current_distance;
        if (strcmp(name, item) == 0)
            continue;
        /* No more than 1/3 of the involved characters should need changed. */
        max_distance = (Py_ssize_t)(name_size + item_size + 3) * QP_SUGGEST_MOVE_COST / 6;
        /* Don't take matches we've already beaten. */
        if (suggestion_distance - 1 < max_distance)
            max_distance = suggestion_distance - 1;
        if (max_distance < 0)
            continue;
        current_distance = qp_suggest_levenshtein(name, name_size, item, item_size,
                                                  (size_t)max_distance, buffer);
        if (current_distance > max_distance)
            continue;
        if (suggestion < 0 || current_distance < suggestion_distance) {
            suggestion = i;
            suggestion_distance = current_distance;
        }
    }
    return suggestion;
}

/* ---- positional / keyword checks ---------------------------------------- */

static int qp_arg_check_positional(JSContext *ctx, const char *fname, int nargs, int min, int max)
{
    if (nargs < min) {
        qp_raise_type_error(ctx, "%.200s expected %s%d argument%s, got %d",
                            fname, (min == max ? "" : "at least "), min,
                            min == 1 ? "" : "s", nargs);
        return -1;
    }
    if (nargs == 0)
        return 0;
    if (nargs > max) {
        qp_raise_type_error(ctx, "%.200s expected %s%d argument%s, got %d",
                            fname, (min == max ? "" : "at most "), max,
                            max == 1 ? "" : "s", nargs);
        return -1;
    }
    return 0;
}

static int qp_arg_no_keywords(JSContext *ctx, const char *fname, JSValueConst kwnames)
{
    if (QP_IS_NULL(kwnames) || JS_IsUndefined(kwnames) || QP_TUPLE_SIZE(kwnames) == 0)
        return 0;
    qp_raise_type_error(ctx, "%s() takes no keyword arguments", fname);
    return -1;
}

/* Index of the parameter named kw among spec->keywords[posonly..total), or
   -1. */
static int qp_arg_find_param(const QPArgSpec *spec, int posonly, int total, const char *kw)
{
    int i;
    for (i = posonly; i < total; i++) {
        if (strcmp(spec->keywords[i], kw) == 0)
            return i;
    }
    return -1;
}

/* error_unexpected_keyword_arg(): the first keyword that names no
   (non-positional-only) parameter. */
static void qp_arg_unexpected_keyword(JSContext *ctx, const QPArgSpec *spec, int posonly,
                                      int total, const char **kwstrs, JSValueConst kwnames, int nkw)
{
    int j;
    for (j = 0; j < nkw; j++) {
        JSValueConst name = QP_TUPLE_ITEMS(kwnames)[j];
        if (!qp_str_check(ctx, name)) {
            qp_raise_type_error(ctx, "keywords must be strings");
            return;
        }
        if (qp_arg_find_param(spec, posonly, total, kwstrs[j]) < 0) {
            int s = qp_suggest_keyword(kwstrs[j], spec->keywords + posonly, total - posonly);
            if (s >= 0) {
                qp_raise_type_error(ctx, "%.200s() got an unexpected keyword argument '%U'."
                                    " Did you mean '%s'?", spec->fname, name,
                                    spec->keywords[posonly + s]);
            } else {
                qp_raise_type_error(ctx, "%.200s() got an unexpected keyword argument '%U'",
                                    spec->fname, name);
            }
            return;
        }
    }
    /* Something wrong happened: extraneous keyword arguments that all
       match (duplicates cannot reach here from the interpreter). */
    qp_raise_type_error(ctx, "invalid keyword argument for %.200s()", spec->fname);
}

/* _PyArg_UnpackKeywords: out[0..total) gets the borrowed argument of each
   parameter or QP_NULL. */
static int qp_arg_parse(JSContext *ctx, const QPArgSpec *spec, JSValueConst *args, int nargs,
                        JSValueConst kwnames, JSValueConst *out)
{
    const char *kwbuf[8];
    const char **kwstrs = kwbuf;
    JSValueConst *kwstack = args + nargs;
    int total, posonly, maxpos, minpos, minkw, minposonly, reqlimit;
    int nkw, nkwargs, i, ret = -1;
    int varpos = spec->varargs != 0;

    total = 0;
    while (spec->keywords[total])
        total++;
    posonly = 0;
    while (posonly < total && spec->keywords[posonly][0] == '\0')
        posonly++;
    maxpos = spec->max_pos;
    minpos = spec->min_args < maxpos ? spec->min_args : maxpos;
    minkw = spec->min_args - minpos;
    minposonly = posonly < minpos ? posonly : minpos;
    reqlimit = minkw ? maxpos + minkw : minpos;
    nkw = (QP_IS_NULL(kwnames) || JS_IsUndefined(kwnames)) ? 0 : (int)QP_TUPLE_SIZE(kwnames);
    nkwargs = nkw;

    for (i = 0; i < total; i++)
        out[i] = QP_NULL;

    if (nkwargs == 0 && minkw == 0 && minpos <= nargs && (varpos || nargs <= maxpos)) {
        /* Fast path. */
        int n = nargs < total ? nargs : total;
        if (varpos && n > maxpos)
            n = maxpos;
        for (i = 0; i < n; i++)
            out[i] = args[i];
        return 0;
    }
    if (!varpos && nargs + nkwargs > total) {
        /* Adding "keyword" (when nargs == 0) prevents producing wrong error
           messages in some special cases (see bpo-31229). */
        qp_raise_type_error(ctx, "%.200s() takes at most %d %sargument%s (%d given)",
                            spec->fname, total, (nargs == 0) ? "keyword " : "",
                            (total == 1) ? "" : "s", nargs + nkwargs);
        return -1;
    }
    if (!varpos && nargs > maxpos) {
        if (maxpos == 0) {
            qp_raise_type_error(ctx, "%.200s() takes no positional arguments", spec->fname);
        } else {
            qp_raise_type_error(ctx, "%.200s() takes %s %d positional argument%s (%d given)",
                                spec->fname, (minpos < maxpos) ? "at most" : "exactly",
                                maxpos, (maxpos == 1) ? "" : "s", nargs);
        }
        return -1;
    }
    if (nargs < minposonly) {
        qp_raise_type_error(ctx, "%.200s() takes %s %d positional argument%s (%d given)",
                            spec->fname,
                            (varpos || minposonly < maxpos) ? "at least" : "exactly",
                            minposonly, minposonly == 1 ? "" : "s", nargs);
        return -1;
    }
    if (varpos && nargs > maxpos)
        nargs = maxpos;
    for (i = 0; i < nargs; i++)
        out[i] = args[i];

    /* keyword names as C strings */
    if (nkw > (int)countof(kwbuf)) {
        kwstrs = js_mallocz(ctx, sizeof(kwstrs[0]) * nkw);
        if (!kwstrs)
            return -1;
    }
    for (i = 0; i < nkw; i++) {
        kwstrs[i] = JS_ToCString(ctx, QP_TUPLE_ITEMS(kwnames)[i]);
        if (!kwstrs[i])
            goto done;
    }

    /* copy keyword args using the parameter list to drive the process */
    for (i = nargs > posonly ? nargs : posonly; i < total; i++) {
        JSValueConst current_arg = QP_NULL;
        if (nkwargs) {
            int j;
            for (j = 0; j < nkw; j++) {
                if (strcmp(kwstrs[j], spec->keywords[i]) == 0) {
                    current_arg = kwstack[j];
                    break;
                }
            }
        }
        out[i] = current_arg;
        if (!QP_IS_NULL(current_arg)) {
            --nkwargs;
        } else if (i < minpos || (maxpos <= i && i < reqlimit)) {
            /* Less arguments than required */
            qp_raise_type_error(ctx, "%.200s() missing required argument '%s' (pos %d)",
                                spec->fname, spec->keywords[i], i + 1);
            goto done;
        }
    }

    if (nkwargs > 0) {
        /* make sure there are no arguments given by name and position */
        for (i = posonly; i < nargs; i++) {
            int j;
            for (j = 0; j < nkw; j++) {
                if (strcmp(kwstrs[j], spec->keywords[i]) == 0) {
                    qp_raise_type_error(ctx, "argument for %.200s() given by name ('%s') "
                                        "and position (%d)", spec->fname,
                                        spec->keywords[i], i + 1);
                    goto done;
                }
            }
        }
        qp_arg_unexpected_keyword(ctx, spec, posonly, total, kwstrs, kwnames, nkw);
        goto done;
    }
    ret = 0;
done:
    for (i = 0; i < nkw; i++) {
        if (kwstrs[i])
            JS_FreeCString(ctx, kwstrs[i]);
    }
    if (kwstrs != kwbuf)
        js_free(ctx, kwstrs);
    return ret;
}

/* ---- converters --------------------------------------------------------- */

/* Argument Clinic Py_ssize_t converter: _PyNumber_Index + PyLong_AsSsize_t. */
static int qp_arg_ssize(JSContext *ctx, JSValueConst v, Py_ssize_t *out)
{
    JSValue iobj = qp_index(ctx, v);
    int r;
    if (JS_IsException(iobj))
        return -1;
    r = qp_long_as_ssize(ctx, iobj, out);
    JS_FreeValue(ctx, iobj);
    return r < 0 ? -1 : 0;
}

/* Argument Clinic bool converter: PyObject_IsTrue. */
static int qp_arg_bool(JSContext *ctx, JSValueConst v, int *out)
{
    int r = qp_bool(ctx, v);
    if (r < 0)
        return -1;
    *out = r;
    return 0;
}

/* Argument Clinic int converter (PyLong_AsInt: __index__, OverflowError
   "Python int too large to convert to C int"). */
static int qp_arg_int(JSContext *ctx, JSValueConst v, int *out)
{
    JSValue iobj;
    int64_t x;
    int overflow;

    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) {
        *out = JS_VALUE_GET_INT(v);
        return 0;
    }
    iobj = qp_index(ctx, v);
    if (JS_IsException(iobj))
        return -1;
    overflow = 0;
    if (qp_long_as_int64_overflow(iobj, &x, &overflow) < 0) {
        JS_FreeValue(ctx, iobj);
        return -1;
    }
    JS_FreeValue(ctx, iobj);
    if (overflow || x > INT_MAX || x < INT_MIN) {
        qp_raise_overflow_error(ctx, "Python int too large to convert to C int");
        return -1;
    }
    *out = (int)x;
    return 0;
}
