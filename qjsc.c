/*
 * QuickJS command line compiler
 *
 * Copyright (c) 2018-2021 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "cutils.h"
#include "quickjs-libc.h"

typedef struct {
    char *name;
    char *short_name;
    int flags;
} namelist_entry_t;

typedef struct namelist_t {
    namelist_entry_t *array;
    int count;
    int size;
} namelist_t;

typedef struct {
    const char *option_name;
    const char *init_func; /* NULL if nothing is added to the context */
    /* space separated names whose use keeps the feature with -fno-unused
       (identifiers, property names and the C module names) */
    const char *names;
} FeatureEntry;

static namelist_t cname_list;
/* flags: CMODULE_SYSTEM for std, os and bjson, whose needs are given in
   feature_list, CMODULE_USED if statically imported */
#define CMODULE_SYSTEM 1
#define CMODULE_USED   2
static namelist_t cmodule_list;
static uint64_t feature_bitmap;
static FILE *outfile;
static BOOL byte_swap;
static BOOL dynamic_export;
#ifdef CONFIG_TEMPORAL_INTL
static BOOL enable_experimental;
#endif
static const char *c_ident_prefix = "qjsc_";

#define FE_ALL (-1)

static const FeatureEntry feature_list[] = {
    { "date", "JS_AddIntrinsicDate", "Date bjson" },
    /* the code compiled at run time can use any feature */
#define FE_EVAL 1
    { "eval", "JS_AddIntrinsicEval",
      "eval Function evalScript loadScript __loadScript Worker import" },
    { "string-normalize", "JS_AddIntrinsicStringNormalize", "normalize" },
    { "regexp", "JS_AddIntrinsicRegExp",
      "RegExp match matchAll search bjson" },
    { "json", "JS_AddIntrinsicJSON", "JSON" },
    { "proxy", "JS_AddIntrinsicProxy", "Proxy" },
    { "map", "JS_AddIntrinsicMapSet", "Map Set WeakMap WeakSet bjson" },
    { "typedarray", "JS_AddIntrinsicTypedArrays",
      "ArrayBuffer SharedArrayBuffer DataView Atomics "
      "Int8Array Uint8Array Uint8ClampedArray Int16Array Uint16Array "
      "Int32Array Uint32Array BigInt64Array BigUint64Array "
      "Float16Array Float32Array Float64Array std os bjson" },
    { "promise", "JS_AddIntrinsicPromise",
      "Promise fromAsync waitAsync import os" },
#define FE_MODULE_LOADER 9
    { "module-loader", NULL, "import Worker" },
    { "weakref", "JS_AddIntrinsicWeakRef", "WeakRef FinalizationRegistry" },
    { "domexception", "JS_AddIntrinsicDOMException",
      "DOMException atob btoa" },
    { "atob", "JS_AddIntrinsicAToB", "atob btoa" },
    { "performance", "JS_AddPerformance", "performance" },
};

#ifdef CONFIG_TEMPORAL_INTL
/* names keeping Temporal and Intl (--experimental) with -fno-unused */
static const char experimental_names[] =
    "Temporal Intl toTemporalInstant toLocaleString toLocaleDateString "
    "toLocaleTimeString toLocaleUpperCase toLocaleLowerCase localeCompare";
#endif

/* -fno-unused: omit the features not referenced by the compiled code */
static BOOL no_unused;
static uint64_t used_feature_bitmap;
static BOOL used_experimental;
/* the code may import modules at run time (import(), eval, Worker...):
   all the declared C modules are initialized */
static BOOL may_import;

/* return TRUE if 'name' is a word of the space separated list 'names' */
static BOOL has_name(const char *names, const char *name)
{
    size_t len = strlen(name);
    const char *p, *end;

    for(p = names;; p = end + 1) {
        end = strchr(p, ' ');
        if (!end)
            end = p + strlen(p);
        if ((size_t)(end - p) == len && !memcmp(p, name, len))
            return TRUE;
        if (*end == '\0')
            return FALSE;
    }
}

static void mark_all_used(void)
{
    used_feature_bitmap = FE_ALL;
    used_experimental = TRUE;
}

static void mark_used_name(const char *name)
{
    int i;

    for(i = 0; i < countof(feature_list); i++) {
        if (has_name(feature_list[i].names, name))
            used_feature_bitmap |= (uint64_t)1 << i;
    }
    if (has_name(feature_list[FE_EVAL].names, name))
        may_import = TRUE;
#ifdef CONFIG_TEMPORAL_INTL
    if (has_name(experimental_names, name))
        used_experimental = TRUE;
#endif
}

static void mark_used_atom(JSContext *ctx, JSAtom name, void *opaque)
{
    const char *str;

    str = JS_AtomToCString(ctx, name);
    if (!str) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        mark_all_used();
        may_import = TRUE;
        return;
    }
    mark_used_name(str);
    JS_FreeCString(ctx, str);
}

void namelist_add(namelist_t *lp, const char *name, const char *short_name,
                  int flags)
{
    namelist_entry_t *e;
    if (lp->count == lp->size) {
        size_t newsize = lp->size + (lp->size >> 1) + 4;
        namelist_entry_t *a =
            realloc(lp->array, sizeof(lp->array[0]) * newsize);
        /* XXX: check for realloc failure */
        lp->array = a;
        lp->size = newsize;
    }
    e =  &lp->array[lp->count++];
    e->name = strdup(name);
    if (short_name)
        e->short_name = strdup(short_name);
    else
        e->short_name = NULL;
    e->flags = flags;
}

void namelist_free(namelist_t *lp)
{
    while (lp->count > 0) {
        namelist_entry_t *e = &lp->array[--lp->count];
        free(e->name);
        free(e->short_name);
    }
    free(lp->array);
    lp->array = NULL;
    lp->size = 0;
}

namelist_entry_t *namelist_find(namelist_t *lp, const char *name)
{
    int i;
    for(i = 0; i < lp->count; i++) {
        namelist_entry_t *e = &lp->array[i];
        if (!strcmp(e->name, name))
            return e;
    }
    return NULL;
}

static void get_c_name(char *buf, size_t buf_size, const char *file)
{
    const char *p, *r;
    size_t len, i;
    int c;
    char *q;

    p = strrchr(file, '/');
    if (!p)
        p = file;
    else
        p++;
    r = strrchr(p, '.');
    if (!r)
        len = strlen(p);
    else
        len = r - p;
    pstrcpy(buf, buf_size, c_ident_prefix);
    q = buf + strlen(buf);
    for(i = 0; i < len; i++) {
        c = p[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z'))) {
            c = '_';
        }
        if ((q - buf) < buf_size - 1)
            *q++ = c;
    }
    *q = '\0';
}

static void dump_hex(FILE *f, const uint8_t *buf, size_t len)
{
    size_t i, col;
    col = 0;
    for(i = 0; i < len; i++) {
        fprintf(f, " 0x%02x,", buf[i]);
        if (++col == 8) {
            fprintf(f, "\n");
            col = 0;
        }
    }
    if (col != 0)
        fprintf(f, "\n");
}

typedef enum {
    CNAME_TYPE_SCRIPT,
    CNAME_TYPE_MODULE,
    CNAME_TYPE_JSON_MODULE,
} CNameTypeEnum;

/* 'module_type': 'type' import attribute of a JSON module (NULL if none) */
static void output_object_code(JSContext *ctx,
                               FILE *fo, JSValueConst obj, const char *c_name,
                               CNameTypeEnum c_name_type,
                               const char *module_type)
{
    uint8_t *out_buf;
    size_t out_buf_len;
    int flags;

    if (c_name_type == CNAME_TYPE_JSON_MODULE) {
        flags = 0;
    } else {
        flags = JS_WRITE_OBJ_BYTECODE;
        /* also needed without -fno-unused to know if the code may
           import modules at run time */
        if (JS_EnumCodeNames(ctx, obj, mark_used_atom, NULL)) {
            js_std_dump_error(ctx);
            exit(1);
        }
    }
    if (byte_swap)
        flags |= JS_WRITE_OBJ_BSWAP;
    out_buf = JS_WriteObject(ctx, &out_buf_len, obj, flags);
    if (!out_buf) {
        js_std_dump_error(ctx);
        exit(1);
    }

    namelist_add(&cname_list, c_name, module_type, c_name_type);

    fprintf(fo, "const uint32_t %s_size = %u;\n\n",
            c_name, (unsigned int)out_buf_len);
    fprintf(fo, "const uint8_t %s[%u] = {\n",
            c_name, (unsigned int)out_buf_len);
    dump_hex(fo, out_buf, out_buf_len);
    fprintf(fo, "};\n\n");

    js_free(ctx, out_buf);
}

static int js_module_dummy_init(JSContext *ctx, JSModuleDef *m)
{
    /* should never be called when compiling JS code */
    abort();
}

static void find_unique_cname(char *cname, size_t cname_size)
{
    char cname1[1024];
    int suffix_num;
    size_t len, max_len;
    assert(cname_size >= 32);
    /* find a C name not matching an existing module C name by
       adding a numeric suffix */
    len = strlen(cname);
    max_len = cname_size - 16;
    if (len > max_len)
        cname[max_len] = '\0';
    suffix_num = 1;
    for(;;) {
        snprintf(cname1, sizeof(cname1), "%s_%d", cname, suffix_num);
        if (!namelist_find(&cname_list, cname1))
            break;
        suffix_num++;
    }
    pstrcpy(cname, cname_size, cname1);
}

JSModuleDef *jsc_module_loader(JSContext *ctx,
                               const char *module_name, void *opaque,
                               JSValueConst attributes)
{
    JSModuleDef *m;
    namelist_entry_t *e;

    /* check if it is a declared C or system module */
    e = namelist_find(&cmodule_list, module_name);
    if (e) {
        if (e->flags & CMODULE_SYSTEM)
            mark_used_name(e->name);
        else
            mark_all_used(); /* unknown needs */
        e->flags |= CMODULE_USED;
        /* create a dummy module */
        m = JS_NewCModule(ctx, module_name, js_module_dummy_init);
    } else if (has_suffix(module_name, ".so")) {
        fprintf(stderr, "Warning: binary module '%s' will be dynamically loaded\n", module_name);
        /* create a dummy module */
        m = JS_NewCModule(ctx, module_name, js_module_dummy_init);
        /* the resulting executable will export its symbols for the
           dynamic library */
        dynamic_export = TRUE;
        mark_all_used();
    } else {
        static const char * const module_type_names[] = {
            [JS_MODULE_TYPE_JSON] = "json",
            [JS_MODULE_TYPE_JSON5] = "json5",
            [JS_MODULE_TYPE_TEXT] = "text",
            [JS_MODULE_TYPE_BYTES] = "bytes",
        };
        size_t buf_len;
        uint8_t *buf;
        char cname[1024];
        int type, attr_type;

        attr_type = js_module_get_type(ctx, attributes);
        if (attr_type < 0)
            return NULL;
        type = attr_type;
        if (type == JS_MODULE_TYPE_JS && has_suffix(module_name, ".json"))
            type = JS_MODULE_TYPE_JSON;
        if (type == JS_MODULE_TYPE_BYTES)
            mark_used_name("Uint8Array");
        buf = js_load_file(ctx, &buf_len, module_name);
        if (!buf) {
            JS_ThrowReferenceError(ctx, "could not load module filename '%s'",
                                   module_name);
            return NULL;
        }

        if (type != JS_MODULE_TYPE_JS) {
            /* JSON, text or bytes: the value of the "default" export is
               serialized */
            JSValue val;

            val = js_module_new_value(ctx, type, buf, buf_len, module_name);
            if (JS_IsException(val))
                return NULL;
            /* create a dummy module */
            m = JS_NewCModule(ctx, module_name, js_module_dummy_init);
            if (!m) {
                JS_FreeValue(ctx, val);
                return NULL;
            }

            get_c_name(cname, sizeof(cname), module_name);
            if (namelist_find(&cname_list, cname)) {
                find_unique_cname(cname, sizeof(cname));
            }

            /* output the module name */
            fprintf(outfile, "static const uint8_t %s_module_name[] = {\n",
                    cname);
            dump_hex(outfile, (const uint8_t *)module_name, strlen(module_name) + 1);
            fprintf(outfile, "};\n\n");

            /* the 'type' attribute is given to the embedded module so
               that the imports with this attribute find it */
            output_object_code(ctx, outfile, val, cname, CNAME_TYPE_JSON_MODULE,
                               module_type_names[attr_type]);
            JS_FreeValue(ctx, val);
        } else {
            JSValue func_val;
            int ts_flag;

            ts_flag = js_std_get_typescript_flag(ctx, module_name);
            if (ts_flag < 0) {
                js_free(ctx, buf);
                return NULL;
            }
            /* compile the module */
            func_val = JS_Eval(ctx, (char *)buf, buf_len, module_name,
                               JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY |
                               ts_flag);
            js_free(ctx, buf);
            if (JS_IsException(func_val))
                return NULL;
            get_c_name(cname, sizeof(cname), module_name);
            if (namelist_find(&cname_list, cname)) {
                find_unique_cname(cname, sizeof(cname));
            }
            output_object_code(ctx, outfile, func_val, cname, CNAME_TYPE_MODULE,
                               NULL);
            
            /* the module is already referenced, so we must free it */
            m = JS_VALUE_GET_PTR(func_val);
            JS_FreeValue(ctx, func_val);
        }
    }
    return m;
}

static void compile_file(JSContext *ctx, FILE *fo,
                         const char *filename,
                         const char *c_name1,
                         int module)
{
    uint8_t *buf;
    char c_name[1024];
    int eval_flags, ret;
    JSValue obj;
    size_t buf_len;

    buf = js_load_file(ctx, &buf_len, filename);
    if (!buf) {
        fprintf(stderr, "Could not load '%s'\n", filename);
        exit(1);
    }
    eval_flags = JS_EVAL_FLAG_COMPILE_ONLY;
    ret = js_std_get_typescript_flag(ctx, filename);
    if (ret < 0) {
        js_std_dump_error(ctx);
        exit(1);
    }
    eval_flags |= ret;
    if (module < 0) {
        module = (has_suffix(filename, ".mjs") || has_suffix(filename, ".mts") ||
                  (!has_suffix(filename, ".cts") &&
                   JS_DetectModule((const char *)buf, buf_len)));
    }
    if (module)
        eval_flags |= JS_EVAL_TYPE_MODULE;
    else
        eval_flags |= JS_EVAL_TYPE_GLOBAL;
    obj = JS_Eval(ctx, (const char *)buf, buf_len, filename, eval_flags);
    if (JS_IsException(obj)) {
        js_std_dump_error(ctx);
        exit(1);
    }
    js_free(ctx, buf);
    if (c_name1) {
        pstrcpy(c_name, sizeof(c_name), c_name1);
    } else {
        get_c_name(c_name, sizeof(c_name), filename);
        if (namelist_find(&cname_list, c_name)) {
            find_unique_cname(c_name, sizeof(c_name));
        }
    }
    output_object_code(ctx, fo, obj, c_name, CNAME_TYPE_SCRIPT, NULL);
    JS_FreeValue(ctx, obj);
}

static const char main_c_template1[] =
    "int main(int argc, char **argv)\n"
    "{\n"
    "  JSRuntime *rt;\n"
    "  JSContext *ctx;\n"
    "  rt = JS_NewRuntime();\n"
    "  js_std_set_worker_new_context_func(JS_NewCustomContext);\n"
    "  js_std_init_handlers(rt);\n"
    ;

static const char main_c_template2[] =
    "  js_std_loop(ctx);\n"
    "  js_std_free_handlers(rt);\n"
    "  JS_FreeContext(ctx);\n"
    "  JS_FreeRuntime(rt);\n"
    "  return 0;\n"
    "}\n";

#define PROG_NAME "qjsc"

void help(void)
{
    printf("QuickJS Compiler version " CONFIG_VERSION "\n"
           "usage: " PROG_NAME " [options] [files]\n"
           "\n"
           "options are:\n"
           "-c          only output bytecode to a C file\n"
           "-e          output main() and bytecode to a C file (default = executable output)\n"
           "-o output   set the output filename\n"
           "-N cname    set the C name of the generated data\n"
           "-m          compile as Javascript module (default=autodetect)\n"
           "-D module_name         compile a dynamically loaded module or worker\n"
           "-M module_name[,cname] add initialization code for an external C module\n"
           "-x          byte swapped output\n"
           "-p prefix   set the prefix of the generated C names\n"
           "-S n        set the maximum stack size to 'n' bytes (default=%d)\n"
           "-s             strip all the debug info\n"
           "--keep-source  keep the source code\n"
#ifdef CONFIG_TEMPORAL_INTL
           "--experimental enable experimental Temporal and Intl support\n"
#endif
           , JS_DEFAULT_STACK_SIZE);
#ifdef CONFIG_LTO
    printf("-flto       use link time optimization\n");
#endif
    {
        int i;
        printf("-fno-[");
        for(i = 0; i < countof(feature_list); i++) {
            if (i != 0)
                printf("|");
            printf("%s", feature_list[i].option_name);
        }
        printf("]\n"
               "            disable selected language features (smaller code size)\n"
               "-fno-unused disable the features the compiled code does not reference\n");
    }
    exit(1);
}

#if defined(CONFIG_CC) && !defined(_WIN32)

int exec_cmd(char **argv)
{
    int pid, status, ret;

    pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        exit(1);
    }

    for(;;) {
        ret = waitpid(pid, &status, 0);
        if (ret == pid && WIFEXITED(status))
            break;
    }
    return WEXITSTATUS(status);
}

static int output_executable(const char *out_filename, const char *cfilename,
                             BOOL use_lto, BOOL verbose, const char *exename)
{
    const char *argv[64];
    const char **arg, *bn_suffix, *lto_suffix;
    char libjsname[1024];
    char exe_dir[1024], inc_dir[1024], lib_dir[1024], buf[1024], *p;
    int ret;

    /* get the directory of the executable */
    pstrcpy(exe_dir, sizeof(exe_dir), exename);
    p = strrchr(exe_dir, '/');
    if (p) {
        *p = '\0';
    } else {
        pstrcpy(exe_dir, sizeof(exe_dir), ".");
    }

    /* if 'quickjs.h' is present at the same path as the executable, we
       use it as include and lib directory */
    snprintf(buf, sizeof(buf), "%s/quickjs.h", exe_dir);
    if (access(buf, R_OK) == 0) {
        pstrcpy(inc_dir, sizeof(inc_dir), exe_dir);
        pstrcpy(lib_dir, sizeof(lib_dir), exe_dir);
    } else {
        snprintf(inc_dir, sizeof(inc_dir), "%s/include/quickjs", CONFIG_PREFIX);
        snprintf(lib_dir, sizeof(lib_dir), "%s/lib/quickjs", CONFIG_PREFIX);
    }

    lto_suffix = "";
    bn_suffix = "";

    arg = argv;
    *arg++ = CONFIG_CC;
    *arg++ = "-O2";
#ifdef CONFIG_LTO
    if (use_lto) {
        *arg++ = "-flto";
        lto_suffix = ".lto";
    }
#endif
    /* XXX: use the executable path to find the includes files and
       libraries */
    *arg++ = "-D";
    *arg++ = "_GNU_SOURCE";
#ifdef CONFIG_TEMPORAL_INTL
    if (enable_experimental) {
        *arg++ = "-D";
        *arg++ = "CONFIG_TEMPORAL_INTL";
    }
#endif
    *arg++ = "-I";
    *arg++ = inc_dir;
    *arg++ = "-o";
    *arg++ = out_filename;
    if (dynamic_export) {
        *arg++ = "-rdynamic";
    } else {
        /* drop the code of the disabled features (the library is
           compiled with -ffunction-sections -fdata-sections) */
#if defined(__APPLE__)
        *arg++ = "-Wl,-dead_strip";
#else
        *arg++ = "-Wl,--gc-sections";
#endif
    }
    *arg++ = cfilename;
    snprintf(libjsname, sizeof(libjsname), "%s/libquickjs%s%s.a",
             lib_dir, bn_suffix, lto_suffix);
    *arg++ = libjsname;
    *arg++ = "-lm";
    *arg++ = "-ldl";
    *arg++ = "-lpthread";
#if defined(CONFIG_PYTHON) && (defined(__linux__) || defined(__FreeBSD__))
    /* os.openpty / os.forkpty / os.login_tty (as LIBS in the Makefile) */
    *arg++ = "-lutil";
#endif
    *arg = NULL;

    if (verbose) {
        for(arg = argv; *arg != NULL; arg++)
            printf("%s ", *arg);
        printf("\n");
    }

    ret = exec_cmd((char **)argv);
    unlink(cfilename);
    return ret;
}
#else
static int output_executable(const char *out_filename, const char *cfilename,
                             BOOL use_lto, BOOL verbose, const char *exename)
{
    fprintf(stderr, "Executable output is not supported for this target\n");
    exit(1);
    return 0;
}
#endif

static size_t get_suffixed_size(const char *str)
{
    char *p;
    size_t v;
    v = (size_t)strtod(str, &p);
    switch(*p) {
    case 'G':
        v <<= 30;
        break;
    case 'M':
        v <<= 20;
        break;
    case 'k':
    case 'K':
        v <<= 10;
        break;
    default:
        if (*p != '\0') {
            fprintf(stderr, "qjs: invalid suffix: %s\n", p);
            exit(1);
        }
        break;
    }
    return v;
}

typedef enum {
    OUTPUT_C,
    OUTPUT_C_MAIN,
    OUTPUT_EXECUTABLE,
} OutputTypeEnum;

static const char *get_short_optarg(int *poptind, int opt,
                                    const char *arg, int argc, char **argv)
{
    const char *optarg;
    if (*arg) {
        optarg = arg;
    } else if (*poptind < argc) {
        optarg = argv[(*poptind)++];
    } else {
        fprintf(stderr, "qjsc: expecting parameter for -%c\n", opt);
        exit(1);
    }
    return optarg;
}

int main(int argc, char **argv)
{
    int i, verbose, strip_flags;
    const char *out_filename, *cname;
    char cfilename[1024];
    FILE *fo;
    JSRuntime *rt;
    JSContext *ctx;
    BOOL use_lto;
    int module;
    OutputTypeEnum output_type;
    size_t stack_size;
    namelist_t dynamic_module_list;

    out_filename = NULL;
    output_type = OUTPUT_EXECUTABLE;
    cname = NULL;
    feature_bitmap = FE_ALL;
    module = -1;
    byte_swap = FALSE;
    verbose = 0;
    strip_flags = JS_STRIP_SOURCE;
    use_lto = FALSE;
    stack_size = 0;
    memset(&dynamic_module_list, 0, sizeof(dynamic_module_list));

    /* add system modules */
    namelist_add(&cmodule_list, "std", "std", CMODULE_SYSTEM);
    namelist_add(&cmodule_list, "os", "os", CMODULE_SYSTEM);
    namelist_add(&cmodule_list, "bjson", "bjson", CMODULE_SYSTEM);

    optind = 1;
    while (optind < argc && *argv[optind] == '-') {
        char *arg = argv[optind] + 1;
        const char *longopt = "";
        const char *optarg;
        /* a single - is not an option, it also stops argument scanning */
        if (!*arg)
            break;
        optind++;
        if (*arg == '-') {
            longopt = arg + 1;
            arg += strlen(arg);
            /* -- stops argument scanning */
            if (!*longopt)
                break;
        }
        for (; *arg || *longopt; longopt = "") {
            char opt = *arg;
            if (opt)
                arg++;
            if (opt == 'h' || opt == '?' || !strcmp(longopt, "help")) {
                help();
                continue;
            }
            if (opt == 'o') {
                out_filename = get_short_optarg(&optind, opt, arg, argc, argv);
                break;
            }
            if (opt == 'c') {
                output_type = OUTPUT_C;
                continue;
            }
            if (opt == 'e') {
                output_type = OUTPUT_C_MAIN;
                continue;
            }
            if (opt == 'N') {
                cname = get_short_optarg(&optind, opt, arg, argc, argv);
                break;
            }
            if (opt == 'f') {
                const char *p;
                optarg = get_short_optarg(&optind, opt, arg, argc, argv);
                p = optarg;
                if (!strcmp(p, "lto")) {
                    use_lto = TRUE;
                } else if (!strcmp(p, "no-unused")) {
                    use_lto = TRUE;
                    no_unused = TRUE;
                } else if (strstart(p, "no-", &p)) {
                    use_lto = TRUE;
                    for(i = 0; i < countof(feature_list); i++) {
                        if (!strcmp(p, feature_list[i].option_name)) {
                            feature_bitmap &= ~((uint64_t)1 << i);
                            break;
                        }
                    }
                    if (i == countof(feature_list))
                        goto bad_feature;
                } else {
                bad_feature:
                    fprintf(stderr, "unsupported feature: %s\n", optarg);
                    exit(1);
                }
                break;
            }
            if (opt == 'm') {
                module = 1;
                continue;
            }
            if (opt == 'M') {
                char *p;
                char path[1024];
                char cname[1024];

                optarg = get_short_optarg(&optind, opt, arg, argc, argv);
                pstrcpy(path, sizeof(path), optarg);
                p = strchr(path, ',');
                if (p) {
                    *p = '\0';
                    pstrcpy(cname, sizeof(cname), p + 1);
                } else {
                    get_c_name(cname, sizeof(cname), path);
                }
                namelist_add(&cmodule_list, path, cname, 0);
                break;
            }
            if (opt == 'D') {
                optarg = get_short_optarg(&optind, opt, arg, argc, argv);
                namelist_add(&dynamic_module_list, optarg, NULL, 0);
                break;
            }
            if (opt == 'x') {
                byte_swap = 1;
                continue;
            }
            if (opt == 'v') {
                verbose++;
                continue;
            }
            if (opt == 'p') {
                c_ident_prefix = get_short_optarg(&optind, opt, arg, argc, argv);
                break;
            }
            if (opt == 'S') {
                optarg = get_short_optarg(&optind, opt, arg, argc, argv);
                stack_size = get_suffixed_size(optarg);
                break;
            }
            if (opt == 's') {
                strip_flags = JS_STRIP_DEBUG;
                continue;
            }
            if (!strcmp(longopt, "keep-source")) {
                strip_flags = 0;
                continue;
            }
#ifdef CONFIG_TEMPORAL_INTL
            if (!strcmp(longopt, "experimental")) {
                enable_experimental = TRUE;
                continue;
            }
#endif            
            if (opt) {
                fprintf(stderr, "qjsc: unknown option '-%c'\n", opt);
            } else {
                fprintf(stderr, "qjsc: unknown option '--%s'\n", longopt);
            }
            help();
        }
    }

    if (optind >= argc)
        help();

    if (!out_filename) {
        if (output_type == OUTPUT_EXECUTABLE) {
            out_filename = "a.out";
        } else {
            out_filename = "out.c";
        }
    }

    if (output_type == OUTPUT_EXECUTABLE) {
#if defined(_WIN32) || defined(__ANDROID__)
        /* XXX: find a /tmp directory ? */
        snprintf(cfilename, sizeof(cfilename), "out%d.c", getpid());
#else
        snprintf(cfilename, sizeof(cfilename), "/tmp/out%d.c", getpid());
#endif
    } else {
        pstrcpy(cfilename, sizeof(cfilename), out_filename);
    }

    fo = fopen(cfilename, "w");
    if (!fo) {
        perror(cfilename);
        exit(1);
    }
    outfile = fo;

    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);

    JS_SetStripInfo(rt, strip_flags);

    /* loader for ES6 modules */
    JS_SetModuleLoaderFunc2(rt, js_module_normalize, jsc_module_loader, NULL, NULL);

    fprintf(fo, "/* File generated automatically by the QuickJS compiler. */\n"
            "\n"
            );

    if (output_type != OUTPUT_C) {
        fprintf(fo, "#include \"quickjs-libc.h\"\n"
                "\n"
                );
    } else {
        fprintf(fo, "#include <inttypes.h>\n"
                "\n"
                );
    }

    for(i = optind; i < argc; i++) {
        const char *filename = argv[i];
        compile_file(ctx, fo, filename, cname, module);
        cname = NULL;
    }

    for(i = 0; i < dynamic_module_list.count; i++) {
        if (!jsc_module_loader(ctx, dynamic_module_list.array[i].name, NULL, JS_UNDEFINED)) {
            fprintf(stderr, "Could not load dynamic module '%s'\n",
                    dynamic_module_list.array[i].name);
            exit(1);
        }
    }

    if (output_type != OUTPUT_C) {
        fprintf(fo,
                "static JSContext *JS_NewCustomContext(JSRuntime *rt)\n"
                "{\n"
                "  JSContext *ctx = JS_NewContextRaw(rt);\n"
                "  if (!ctx)\n"
                "    return NULL;\n");
        if (no_unused) {
            if (used_feature_bitmap & ((uint64_t)1 << FE_EVAL))
                mark_all_used();
            feature_bitmap &= used_feature_bitmap;
        }
        /* add the basic objects */
        fprintf(fo, "  JS_AddIntrinsicBaseObjects(ctx);\n");
        for(i = 0; i < countof(feature_list); i++) {
            if ((feature_bitmap & ((uint64_t)1 << i)) &&
                feature_list[i].init_func) {
                fprintf(fo, "  %s(ctx);\n", feature_list[i].init_func);
            }
        }

#ifdef CONFIG_TEMPORAL_INTL
        if (enable_experimental && (!no_unused || used_experimental)) {
            fprintf(fo, "  JS_AddIntrinsicTemporal(ctx);\n");
            fprintf(fo, "  JS_AddIntrinsicIntl(ctx);\n");
        }
#endif
        /* add the precompiled modules (XXX: could modify the module
           loader instead) */
        for(i = 0; i < cmodule_list.count; i++) {
            namelist_entry_t *e = &cmodule_list.array[i];
            /* initialize the static C modules which are imported, or
               all of them if a dynamic import may need them */
            if (!(e->flags & CMODULE_USED) && !may_import)
                continue;
            fprintf(fo,
                    "  {\n"
                    "    extern JSModuleDef *js_init_module_%s(JSContext *ctx, const char *name);\n"
                    "    js_init_module_%s(ctx, \"%s\");\n"
                    "  }\n",
                    e->short_name, e->short_name, e->name);
        }
        for(i = 0; i < cname_list.count; i++) {
            namelist_entry_t *e = &cname_list.array[i];
            if (e->flags == CNAME_TYPE_MODULE) {
                fprintf(fo, "  js_std_eval_binary(ctx, %s, %s_size, 1);\n",
                        e->name, e->name);
            } else if (e->flags == CNAME_TYPE_JSON_MODULE) {
                fprintf(fo, "  js_std_eval_binary_json_module(ctx, %s, %s_size, (const char *)%s_module_name, ",
                        e->name, e->name, e->name);
                if (e->short_name)
                    fprintf(fo, "\"%s\");\n", e->short_name);
                else
                    fprintf(fo, "NULL);\n");
            }
        }
        fprintf(fo,
                "  return ctx;\n"
                "}\n\n");

        fputs(main_c_template1, fo);

        if (stack_size != 0) {
            fprintf(fo, "  JS_SetMaxStackSize(rt, %u);\n",
                    (unsigned int)stack_size);
        }

        /* add the module loader if necessary */
        if (feature_bitmap & (1 << FE_MODULE_LOADER)) {
            fprintf(fo, "  JS_SetModuleLoaderFunc2(rt, js_module_normalize, js_module_loader, js_module_check_attributes, NULL);\n");
        }

        fprintf(fo,
                "  ctx = JS_NewCustomContext(rt);\n"
                "  js_std_add_helpers(ctx, argc, argv);\n");

        for(i = 0; i < cname_list.count; i++) {
            namelist_entry_t *e = &cname_list.array[i];
            if (e->flags == CNAME_TYPE_SCRIPT) {
                fprintf(fo, "  js_std_eval_binary(ctx, %s, %s_size, 0);\n",
                        e->name, e->name);
            }
        }
        fputs(main_c_template2, fo);
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    fclose(fo);

    if (output_type == OUTPUT_EXECUTABLE) {
        return output_executable(out_filename, cfilename, use_lto, verbose,
                                 argv[0]);
    }
    namelist_free(&cname_list);
    namelist_free(&cmodule_list);
    return 0;
}
