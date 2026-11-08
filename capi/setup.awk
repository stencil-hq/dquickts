# Generate the build inputs of the C API modules from capi/Setup.qp
# (format: see the header of that file).  Used by the Makefile:
#
#   awk -v mode=mk          -v conds="zlib ssl ..." -f capi/setup.awk capi/Setup.qp
#       make fragment: the flag variables of Setup.qp, CAPI_CORE_MODULES,
#       CAPI_CORE_MODULE_OBJS (libquickjs), CAPI_EXT_MODULES,
#       CAPI_EXT_MODULE_OBJS (libqpcapi.a) and one target-specific
#       CAPI_SRC_CFLAGS assignment per object ($(CAPI_OBJDIR)/modules/...)
#   awk -v mode=core_inittab -v conds="..."     -f capi/setup.awk capi/Setup.qp
#       capi_core_inittab.c: _PyQp_CoreInittab[] (the self-contained
#       modules, registered by the Python runtime itself)
#   awk -v mode=ext_inittab  -v conds="..."     -f capi/setup.awk capi/Setup.qp
#       capi_ext_inittab.c: _PyQp_ExtInittab[] (the modules that link a
#       system library, registered by the launchers)
#
# A module is "ext" when one of its enabled lines carries a condition named
# by a `*external*` line; an object goes to libquickjs when a core module
# uses it.  Deterministic: modules and objects keep the order of Setup.qp.

function enabled(c) {
    return (c in cond_on)
}

BEGIN {
    n = split(conds, tmp, /[ \t]+/)
    for (i = 1; i <= n; i++)
        if (tmp[i] != "")
            cond_on[tmp[i]] = 1
    nmod = 0
    nobj = 0
    nvar = 0
    skip_module = 0
}

{
    sub(/#.*/, "")
    if (NF == 0)
        next
    if ($1 == "*external*") {
        for (i = 2; i <= NF; i++)
            external[$i] = 1
        next
    }
    if ($2 == "=") {
        if ($1 !~ /^[A-Z_][A-Z_0-9]*$/) {
            printf("capi/Setup.qp:%d: bad variable name %s\n", NR, $1) > "/dev/stderr"
            exit 1
        }
        v = $0
        sub(/^[ \t]*[A-Z_0-9]+[ \t]*=[ \t]*/, "", v)
        sub(/[ \t]+$/, "", v)
        vars[++nvar] = $1 " = " v
        next
    }
    name = $1
    if (name != "+") {
        if (name !~ /^[A-Za-z_][A-Za-z_0-9]*$/) {
            printf("capi/Setup.qp:%d: bad module name %s\n", NR, name) > "/dev/stderr"
            exit 1
        }
        cur = name
        skip_module = 0
    } else if (cur == "") {
        printf("capi/Setup.qp:%d: '+' before any module\n", NR) > "/dev/stderr"
        exit 1
    }
    keep = 1
    ext = 0
    flags = ""
    nsrc = 0
    for (i = 2; i <= NF; i++) {
        t = $i
        if (t ~ /^@/) {
            if (!enabled(substr(t, 2)))
                keep = 0
            if (substr(t, 2) in external)
                ext = 1
        } else if (t ~ /\.c$/) {
            src[++nsrc] = t
        } else {
            flags = flags " " t
        }
    }
    if (name != "+") {
        skip_module = !keep
        if (keep) {
            if (cur in mod_seen) {
                printf("capi/Setup.qp:%d: module %s enabled twice\n", NR, cur) > "/dev/stderr"
                exit 1
            }
            mod_seen[cur] = 1
            mods[++nmod] = cur
            mod_ext[cur] = 0
        }
    }
    if (!keep || skip_module)
        next
    if (ext)
        mod_ext[cur] = 1
    for (i = 1; i <= nsrc; i++) {
        o = src[i]
        sub(/\.c$/, ".o", o)
        users[o] = users[o] " " cur
        if (o in seen)
            continue
        seen[o] = 1
        # ar(1) members are base names: they must be unique
        b = o
        sub(/.*\//, "", b)
        if (b in base_seen) {
            printf("capi/Setup.qp:%d: object base name %s used twice\n", NR, b) > "/dev/stderr"
            exit 1
        }
        base_seen[b] = 1
        objs[++nobj] = o
        objflags[o] = flags
    }
}

function obj_is_core(o,    k, n, u) {
    n = split(users[o], u, " ")
    for (k = 1; k <= n; k++)
        if (u[k] != "" && !mod_ext[u[k]])
            return 1
    return 0
}

function print_inittab(want_ext, sym,    i) {
    print "/* Generated from capi/Setup.qp by capi/setup.awk; do not edit. */"
    print "#include \"Python.h\""
    print "#include \"qpcapi.h\""
    print ""
    for (i = 1; i <= nmod; i++)
        if (mod_ext[mods[i]] == want_ext)
            printf("PyMODINIT_FUNC PyInit_%s(void);\n", mods[i])
    print ""
    printf("struct _inittab %s[] = {\n", sym)
    for (i = 1; i <= nmod; i++)
        if (mod_ext[mods[i]] == want_ext)
            printf("    {\"%s\", PyInit_%s},\n", mods[i], mods[i])
    print "    {NULL, NULL}"
    print "};"
}

END {
    if (mode == "mk") {
        print "# Generated from capi/Setup.qp by capi/setup.awk; do not edit."
        for (i = 1; i <= nvar; i++)
            print vars[i]
        for (pass = 0; pass <= 1; pass++) {
            kind = pass ? "EXT" : "CORE"
            printf("CAPI_%s_MODULES =", kind)
            for (i = 1; i <= nmod; i++)
                if (mod_ext[mods[i]] == pass)
                    printf(" %s", mods[i])
            printf("\nCAPI_%s_MODULE_OBJS =", kind)
            for (i = 1; i <= nobj; i++)
                if (obj_is_core(objs[i]) == !pass)
                    printf(" \\\n\t$(CAPI_OBJDIR)/modules/%s", objs[i])
            printf("\n")
        }
        for (i = 1; i <= nobj; i++)
            printf("$(CAPI_OBJDIR)/modules/%s: CAPI_SRC_CFLAGS =%s\n", objs[i], objflags[objs[i]])
    } else if (mode == "core_inittab") {
        print_inittab(0, "_PyQp_CoreInittab")
    } else if (mode == "ext_inittab") {
        print_inittab(1, "_PyQp_ExtInittab")
    } else {
        print "capi/setup.awk: mode must be mk, core_inittab or ext_inittab" > "/dev/stderr"
        exit 1
    }
}
