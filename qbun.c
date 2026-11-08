/*
 * qbun: run TypeScript and JavaScript programs written for Bun and
 * Node.js on dquickts.
 *
 *   qbun [options] [run] FILE [args...]
 *   qbun [options] -e CODE [args...]
 *
 * The runtime (node/core.c) gives the program the Node.js modules
 * (node:fs, node:path...), the Bun APIs (the Bun global, bun:sqlite...)
 * and the web globals (fetch, URL...) of node/lib, a libuv event loop and
 * Bun's module resolution (node_modules, package.json "exports",
 * TypeScript sources, CommonJS).
 *
 * The library sources are read from $QBUN_LIB, else from the directory
 * given at build time (CONFIG_QBUN_LIB), else from node/lib next to the
 * executable.
 */
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "node/qbun.h"

#ifndef CONFIG_QBUN_LIB
#define CONFIG_QBUN_LIB ""
#endif

static int is_dir(const char *path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *find_lib_dir(const char *exec_path, char *buf, size_t size)
{
    const char *env = getenv("QBUN_LIB");
    const char *slash;

    if (is_dir(env))
        return env;
    if (is_dir(CONFIG_QBUN_LIB))
        return CONFIG_QBUN_LIB;
    slash = strrchr(exec_path, '/');
    if (slash) {
        snprintf(buf, size, "%.*s/node/lib", (int)(slash - exec_path), exec_path);
        if (is_dir(buf))
            return buf;
    }
    return NULL;
}

static void help(void)
{
    printf("usage: qbun [options] [run] FILE [args...]\n"
           "       qbun [options] -e CODE [args...]\n"
           "-e  --eval CODE   evaluate CODE (TypeScript) as a module\n"
           "-r  --preload MOD load the module MOD first (repeatable)\n"
           "-v  --version     print the Bun version qbun implements\n"
           "-h  --help        list options\n");
}

int main(int argc, char **argv)
{
    char exec_path[PATH_MAX], lib_buf[PATH_MAX];
    size_t exec_len = sizeof(exec_path);
    const char *lib_dir, *file = NULL, *eval = NULL;
    const char **preloads;
    QBOptions o;
    QBRuntime *qr;
    char **pargv;
    int i, optind, pargc, ret, exec_end, npreloads = 0;

    preloads = calloc(argc, sizeof(char *));

    signal(SIGPIPE, SIG_IGN);
    argv = uv_setup_args(argc, argv);
    if (uv_exepath(exec_path, &exec_len) != 0)
        snprintf(exec_path, sizeof(exec_path), "%s", argv[0]);

    for (optind = 1; optind < argc; optind++) {
        const char *a = argv[optind];
        if (a[0] != '-' || !strcmp(a, "-"))
            break;
        if (!strcmp(a, "--")) {
            optind++;
            break;
        }
        if (!strcmp(a, "-e") || !strcmp(a, "--eval")) {
            if (optind + 1 >= argc) {
                fprintf(stderr, "qbun: %s needs an argument\n", a);
                return 1;
            }
            eval = argv[++optind];
            optind++;
            break;
        }
        if (!strcmp(a, "-r") || !strcmp(a, "--preload") || !strcmp(a, "--require")) {
            if (optind + 1 >= argc) {
                fprintf(stderr, "qbun: %s needs an argument\n", a);
                return 1;
            }
            preloads[npreloads++] = argv[++optind];
            continue;
        }
        if (!strncmp(a, "--preload=", 10)) {
            preloads[npreloads++] = a + 10;
            continue;
        }
        if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("1.4.4\n");
            return 0;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            help();
            return 0;
        }
        /* Bun options without an effect here (--smol, --silent...) */
    }
    exec_end = optind;
    if (!eval) {
        if (optind < argc && !strcmp(argv[optind], "run"))
            optind++;
        if (optind >= argc) {
            help();
            return 1;
        }
        file = argv[optind++];
    }

    lib_dir = find_lib_dir(exec_path, lib_buf, sizeof(lib_buf));
    if (!lib_dir) {
        fprintf(stderr, "qbun: cannot find the library directory (set QBUN_LIB)\n");
        return 1;
    }

    /* process.argv: [execPath, script, args...] ([execPath, args...]
       with -e) */
    pargv = calloc(argc + 2, sizeof(char *));
    pargc = 0;
    pargv[pargc++] = exec_path;
    if (file) {
        static char abs[PATH_MAX];
        if (!realpath(file, abs))
            snprintf(abs, sizeof(abs), "%s", file);
        pargv[pargc++] = abs;
    }
    for (i = optind; i < argc; i++)
        pargv[pargc++] = argv[i];

    memset(&o, 0, sizeof(o));
    o.lib_dir = lib_dir;
    o.exec_path = exec_path;
    o.argc = pargc;
    o.argv = pargv;
    o.exec_argc = exec_end - 1;
    o.exec_argv = argv + 1;
    o.loop = uv_default_loop();
    o.is_main_thread = 1;

    qr = qb_runtime_new(&o);
    if (!qr) {
        fprintf(stderr, "qbun: cannot create the runtime\n");
        return 1;
    }
    if (qb_bootstrap(qr) < 0)
        return 1;
    for (i = 0; i < npreloads; i++) {
        if (qb_preload(qr, preloads[i]) < 0)
            return 1;
    }
    free(preloads);
    if (file)
        ret = qb_run_main(qr, file);
    else
        ret = qb_run_eval(qr, eval, 1);
    if (ret < 0)
        return 1;
    qb_run_loop(qr);
    ret = qb_runtime_exit(qr);
    fflush(stdout);
    fflush(stderr);
    return ret;
}
