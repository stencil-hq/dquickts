#
# QuickJS Javascript Engine
#
# Copyright (c) 2017-2021 Fabrice Bellard
# Copyright (c) 2017-2021 Charlie Gordon
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

ifeq ($(shell uname -s),Darwin)
CONFIG_DARWIN=y
endif
ifeq ($(shell uname -s),FreeBSD)
CONFIG_FREEBSD=y
endif
# Windows cross compilation from Linux
# May need to have libwinpthread*.dll alongside the executable
# (On Ubuntu/Debian may be installed with mingw-w64-x86-64-dev
# to /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll)
#CONFIG_WIN32=y
# use link time optimization (smaller and faster executables but slower build)
#CONFIG_LTO=y
# consider warnings as errors (for development)
#CONFIG_WERROR=y
# force 32 bit build on x86_64
#CONFIG_M32=y
# cosmopolitan build (see https://github.com/jart/cosmopolitan)
#CONFIG_COSMO=y
# experimental Temporal and Intl support (disable if your C compiler
# does not support 128 bit integers)
ifndef CONFIG_M32
CONFIG_TEMPORAL_INTL=y
endif
# Python 3.15 front end (python/) and the qpy program
CONFIG_PYTHON=y
# Python C API and the CPython accelerator modules linked into qpy
# (capi/, plan D5; 64-bit POSIX hosts, turned off automatically elsewhere).
# Optional dependencies, found with pkg-config (Homebrew prefixes are
# searched on macOS); each one can be skipped:
#   CONFIG_PYTHON_CAPI_NO_ZLIB=y CONFIG_PYTHON_CAPI_NO_BZ2=y
#   CONFIG_PYTHON_CAPI_NO_LZMA=y CONFIG_PYTHON_CAPI_NO_SQLITE3=y
#   CONFIG_PYTHON_CAPI_NO_SSL=y (OpenSSL >= 1.1.1: _ssl and _hashlib)
#   CONFIG_PYTHON_CAPI_SYSTEM_EXPAT=y (pyexpat with -lexpat instead of the
#   bundled capi/modules/expat)
CONFIG_PYTHON_CAPI=y

# installation directory
PREFIX?=/usr/local

# use the gprof profiler
#CONFIG_PROFILE=y
# use address sanitizer
#CONFIG_ASAN=y
# use memory sanitizer
#CONFIG_MSAN=y
# use UB sanitizer
#CONFIG_UBSAN=y
# use thread sanitizer
#CONFIG_TSAN=y
# compile the debug dumps selected with JS_SetDumpFlags() / qjs -D
#CONFIG_DUMPS=y
# abort when a variable read without TDZ check is uninitialized (checks the
# TDZ checks removed by the compiler, see lexical_var_needs_check())
#CONFIG_CHECK_TDZ=y

# TEST262 bootstrap config: commit id and shallow "since" parameter
TEST262_COMMIT?=7ab7fafa0003f73fc85c1b95d88094d33f7eb8bd
TEST262_SINCE?=2025-09-01

OBJDIR=.obj

ifdef CONFIG_ASAN
OBJDIR:=$(OBJDIR)/asan
endif
ifdef CONFIG_MSAN
OBJDIR:=$(OBJDIR)/msan
endif
ifdef CONFIG_UBSAN
OBJDIR:=$(OBJDIR)/ubsan
endif

ifdef CONFIG_DARWIN
# use clang instead of gcc
CONFIG_CLANG=y
CONFIG_DEFAULT_AR=y
endif
ifdef CONFIG_FREEBSD
# use clang instead of gcc
CONFIG_CLANG=y
CONFIG_DEFAULT_AR=y
CONFIG_LTO=
endif

ifdef CONFIG_WIN32
  ifdef CONFIG_M32
    CROSS_PREFIX?=i686-w64-mingw32-
  else
    CROSS_PREFIX?=x86_64-w64-mingw32-
  endif
  EXE=.exe
else ifdef MSYSTEM
  CONFIG_WIN32=y
  CROSS_PREFIX?=
  EXE=.exe
else
  CROSS_PREFIX?=
  EXE=
endif

ifdef CONFIG_CLANG
  HOST_CC=clang
  CC=$(CROSS_PREFIX)clang
  CFLAGS+=-g -Wall -MMD -MF $(OBJDIR)/$(@F).d
  CFLAGS += -Wextra
  CFLAGS += -Wno-sign-compare
  CFLAGS += -Wno-missing-field-initializers
  CFLAGS += -Wundef -Wuninitialized
  CFLAGS += -Wunused -Wno-unused-parameter
  CFLAGS += -Wwrite-strings
  CFLAGS += -Wchar-subscripts -funsigned-char
  CFLAGS += -MMD -MF $(OBJDIR)/$(@F).d
  ifdef CONFIG_DEFAULT_AR
    AR=$(CROSS_PREFIX)ar
  else
    ifdef CONFIG_LTO
      AR=$(CROSS_PREFIX)llvm-ar
    else
      AR=$(CROSS_PREFIX)ar
    endif
  endif
  LIB_FUZZING_ENGINE ?= "-fsanitize=fuzzer"
else ifdef CONFIG_COSMO
  CONFIG_LTO=
  HOST_CC=gcc
  CC=cosmocc
  # cosmocc does not correct support -MF
  CFLAGS=-g -Wall #-MMD -MF $(OBJDIR)/$(@F).d
  CFLAGS += -Wno-array-bounds -Wno-format-truncation
  AR=cosmoar
else
  HOST_CC=gcc
  CC=$(CROSS_PREFIX)gcc
  CFLAGS+=-g -Wall -MMD -MF $(OBJDIR)/$(@F).d
  CFLAGS += -Wno-array-bounds -Wno-format-truncation -Wno-infinite-recursion
  ifdef CONFIG_LTO
    AR=$(CROSS_PREFIX)gcc-ar
  else
    AR=$(CROSS_PREFIX)ar
  endif
endif
STRIP?=$(CROSS_PREFIX)strip
ifdef CONFIG_M32
CFLAGS+=-msse2 -mfpmath=sse # use SSE math for correct FP rounding
ifndef CONFIG_WIN32
CFLAGS+=-m32
LDFLAGS+=-m32
endif
endif
CFLAGS+=-fwrapv # ensure that signed overflows behave as expected
# lets the executables output by qjsc drop the unused code
CFLAGS+=-ffunction-sections -fdata-sections
ifdef CONFIG_WERROR
CFLAGS+=-Werror
endif
DEFINES:=-D_GNU_SOURCE -DCONFIG_VERSION=\"$(shell cat VERSION)\"
ifdef CONFIG_WIN32
DEFINES+=-D__USE_MINGW_ANSI_STDIO # for standard snprintf behavior
endif
ifndef CONFIG_WIN32
ifeq ($(shell $(CC) -o /dev/null compat/test-closefrom.c 2>/dev/null && echo 1),1)
DEFINES+=-DHAVE_CLOSEFROM
endif
endif
ifdef CONFIG_TEMPORAL_INTL
DEFINES+=-DCONFIG_TEMPORAL_INTL
endif
ifdef CONFIG_CHECK_TDZ
DEFINES+=-DCONFIG_CHECK_TDZ
endif
ifdef CONFIG_DUMPS
DEFINES+=-DENABLE_DUMPS
endif
ifdef CONFIG_PYTHON
DEFINES+=-DCONFIG_PYTHON
endif
# the C API needs Python, a 64-bit host and the POSIX pyconfig.h
ifndef CONFIG_PYTHON
CONFIG_PYTHON_CAPI=
endif
ifdef CONFIG_M32
CONFIG_PYTHON_CAPI=
endif
ifdef CONFIG_WIN32
CONFIG_PYTHON_CAPI=
endif
ifdef CONFIG_COSMO
CONFIG_PYTHON_CAPI=
endif
ifdef CONFIG_PYTHON_CAPI
DEFINES+=-DCONFIG_PYTHON_CAPI
endif
# qpy embeds the base snapshot of its startup (python/pythonrun.c,
# doc/python-plan.md section 1), written at build time by running qpy:
# CONFIG_QPY_NO_BASE=y builds a qpy without it (cross builds have none)
ifdef CONFIG_WIN32
CONFIG_QPY_NO_BASE=y
endif
ifdef CONFIG_COSMO
CONFIG_QPY_NO_BASE=y
endif

CFLAGS+=$(DEFINES)
CFLAGS_DEBUG=$(CFLAGS) -O0
CFLAGS_SMALL=$(CFLAGS) -Os
CFLAGS_OPT=$(CFLAGS) -O2
CFLAGS_NOLTO:=$(CFLAGS_OPT)
ifdef CONFIG_COSMO
LDFLAGS+=-s # better to strip by default
else
LDFLAGS+=-g
endif
ifdef CONFIG_LTO
CFLAGS_SMALL+=-flto
CFLAGS_OPT+=-flto
LDFLAGS+=-flto
endif
ifdef CONFIG_PROFILE
CFLAGS+=-p
LDFLAGS+=-p
endif
ifdef CONFIG_ASAN
CFLAGS+=-fsanitize=address -fno-omit-frame-pointer
LDFLAGS+=-fsanitize=address -fno-omit-frame-pointer
endif
ifdef CONFIG_MSAN
CFLAGS+=-fsanitize=memory -fno-omit-frame-pointer
LDFLAGS+=-fsanitize=memory -fno-omit-frame-pointer
endif
ifdef CONFIG_UBSAN
CFLAGS+=-fsanitize=undefined -fno-omit-frame-pointer
LDFLAGS+=-fsanitize=undefined -fno-omit-frame-pointer
endif
ifdef CONFIG_TSAN
CFLAGS+=-fsanitize=thread -fno-omit-frame-pointer
LDFLAGS+=-fsanitize=thread -fno-omit-frame-pointer
endif
ifdef CONFIG_WIN32
LDEXPORT=
else
LDEXPORT=-rdynamic
endif

ifndef CONFIG_COSMO
ifndef CONFIG_DARWIN
ifndef CONFIG_WIN32
CONFIG_SHARED_LIBS=y # building shared libraries is supported
endif
endif
endif

PROGS=qjs$(EXE) qjsc$(EXE) run-test262$(EXE)
ifdef CONFIG_PYTHON
PROGS+=qpy$(EXE)
endif

ifneq ($(CROSS_PREFIX),)
QJSC_CC=gcc
QJSC=./host-qjsc
PROGS+=$(QJSC)
else
QJSC_CC=$(CC)
QJSC=./qjsc$(EXE)
endif
PROGS+=libquickjs.a
ifdef CONFIG_LTO
PROGS+=libquickjs.lto.a
endif

# examples
ifeq ($(CROSS_PREFIX),)
ifndef CONFIG_ASAN
ifndef CONFIG_MSAN
ifndef CONFIG_UBSAN
PROGS+=examples/hello examples/test_fib
# no -m32 option in qjsc
ifndef CONFIG_M32
ifndef CONFIG_WIN32
PROGS+=examples/hello_module
QJSC_TESTS=tests/test_qjsc_unused tests/test_qjsc_modules
endif
endif
ifdef CONFIG_SHARED_LIBS
PROGS+=examples/fib.so examples/point.so
endif
endif
endif
endif
endif

all: $(OBJDIR) $(OBJDIR)/quickjs.check.o $(OBJDIR)/qjs.check.o $(PROGS)

QJS_LIB_OBJS=$(OBJDIR)/quickjs.o $(OBJDIR)/dtoa.o $(OBJDIR)/libregexp.o $(OBJDIR)/libunicode.o $(OBJDIR)/cutils.o $(OBJDIR)/quickjs-libc.o
ifdef CONFIG_TEMPORAL_INTL
QJS_LIB_OBJS+=$(addprefix $(OBJDIR)/intl/, intl.o locale.o locale-data.o numberformat.o collator.o datetimeformat.o pluralrules.o listformat.o relativetimeformat.o displaynames.o segmenter.o durationformat.o)
QJS_LIB_OBJS+=$(addprefix $(OBJDIR)/temporal/, temporal.o instant.o duration.o plaintime.o now.o plaindate.o plaindatetime.o plainyearmonth.o plainmonthday.o zoneddatetime.o calendar.o timezone.o tzdata_generated.o)
endif

QJS_OBJS=$(OBJDIR)/qjs.o $(OBJDIR)/repl.o $(QJS_LIB_OBJS)

HOST_LIBS=-lm -ldl -lpthread
LIBS=-lm -lpthread
ifndef CONFIG_WIN32
LIBS+=-ldl
endif
ifeq ($(shell uname -s),Linux)
# os.openpty / os.forkpty / os.login_tty (in libc since glibc 2.34)
LIBS+=-lutil
endif
LIBS+=$(EXTRA_LIBS)

# all object directories: the parse-time generators below create $(OBJDIR)
# itself, so the order-only $(OBJDIR) target alone cannot be relied on
OBJDIRS=$(OBJDIR) $(OBJDIR)/intl $(OBJDIR)/temporal $(OBJDIR)/examples $(OBJDIR)/tests

$(OBJDIR):
	mkdir -p $(OBJDIRS)

ifdef CONFIG_PYTHON_CAPI
# --------------------------------------------------------------------------
# Python C API (plan D5).  The API itself is implemented in the quickjs.c
# translation unit (python/capi_*.c); the CPython modules of capi/Setup.qp
# (vendored by tools/capi_sync.py) are separate translation units:
# - the self-contained ("core") modules are part of the engine library:
#   $(QJS_CAPI_OBJS) is linked wherever quickjs.o is (and archived in
#   libquickjs.a); the runtime registers them itself (_PyQp_CoreInittab);
# - the modules that need a system library are archived in $(CAPI_LIB);
#   qpy and qjs link it with $(CAPI_LIBS) and register them with
#   PyImport_ExtendInittab(_PyQp_ExtInittab) (capi/qpcapi.h).
CAPI_OBJDIR=$(OBJDIR)/capi
CAPI_LIB=$(CAPI_OBJDIR)/libqpcapi.a
PKG_CONFIG?=pkg-config
ifdef CONFIG_DARWIN
# keg-only Homebrew formulas are not on the default pkg-config path
CAPI_EMPTY:=
CAPI_SPACE:=$(CAPI_EMPTY) $(CAPI_EMPTY)
CAPI_BREW_PREFIXES:=$(shell brew --prefix openssl xz sqlite zlib bzip2 zstd 2>/dev/null)
CAPI_PKG_CONFIG=PKG_CONFIG_PATH="$(subst $(CAPI_SPACE),:,$(addsuffix /lib/pkgconfig,$(CAPI_BREW_PREFIXES)))$${PKG_CONFIG_PATH:+:$$PKG_CONFIG_PATH}" $(PKG_CONFIG)
else
CAPI_PKG_CONFIG=$(PKG_CONFIG)
endif
# conditions of capi/Setup.qp and the flags of each dependency
CAPI_CONDS:=
CAPI_LIBS:=
ifndef CONFIG_PYTHON_CAPI_NO_ZLIB
ifeq ($(shell $(CAPI_PKG_CONFIG) --exists zlib && echo y),y)
CAPI_CONDS+=zlib
CAPI_ZLIB_CFLAGS:=$(shell $(CAPI_PKG_CONFIG) --cflags zlib)
CAPI_LIBS+=$(shell $(CAPI_PKG_CONFIG) --libs zlib)
endif
endif
ifndef CONFIG_PYTHON_CAPI_NO_BZ2
ifeq ($(shell $(CAPI_PKG_CONFIG) --exists bzip2 && echo y),y)
CAPI_CONDS+=bz2
CAPI_BZ2_CFLAGS:=$(shell $(CAPI_PKG_CONFIG) --cflags bzip2)
CAPI_LIBS+=$(shell $(CAPI_PKG_CONFIG) --libs bzip2)
else ifeq ($(shell printf '\043include <bzlib.h>\n' | $(CC) -E -x c - >/dev/null 2>&1 && echo y),y)
# no bzip2.pc on some distributions
CAPI_CONDS+=bz2
CAPI_LIBS+=-lbz2
endif
endif
ifndef CONFIG_PYTHON_CAPI_NO_LZMA
ifeq ($(shell $(CAPI_PKG_CONFIG) --exists liblzma && echo y),y)
CAPI_CONDS+=lzma
CAPI_LZMA_CFLAGS:=$(shell $(CAPI_PKG_CONFIG) --cflags liblzma)
CAPI_LIBS+=$(shell $(CAPI_PKG_CONFIG) --libs liblzma)
endif
endif
ifndef CONFIG_PYTHON_CAPI_NO_SQLITE3
ifeq ($(shell $(CAPI_PKG_CONFIG) --atleast-version=3.15.2 sqlite3 && echo y),y)
CAPI_CONDS+=sqlite3
CAPI_SQLITE3_CFLAGS:=$(shell $(CAPI_PKG_CONFIG) --cflags sqlite3)
CAPI_LIBS+=$(shell $(CAPI_PKG_CONFIG) --libs sqlite3)
endif
endif
ifndef CONFIG_PYTHON_CAPI_NO_SSL
ifeq ($(shell $(CAPI_PKG_CONFIG) --atleast-version=1.1.1 openssl && echo y),y)
CAPI_CONDS+=ssl
CAPI_SSL_CFLAGS:=$(shell $(CAPI_PKG_CONFIG) --cflags openssl)
CAPI_LIBS+=$(shell $(CAPI_PKG_CONFIG) --libs openssl)
endif
endif
ifndef CONFIG_PYTHON_CAPI_NO_ZSTD
ifeq ($(shell $(CAPI_PKG_CONFIG) --atleast-version=1.4.5 libzstd && echo y),y)
CAPI_CONDS+=zstd
CAPI_ZSTD_CFLAGS:=$(shell $(CAPI_PKG_CONFIG) --cflags libzstd)
CAPI_LIBS+=$(shell $(CAPI_PKG_CONFIG) --libs libzstd)
endif
endif
ifndef CONFIG_PYTHON_CAPI_NO_FFI
ifeq ($(shell $(CAPI_PKG_CONFIG) --exists libffi && echo y),y)
CAPI_CONDS+=ffi
CAPI_FFI_CFLAGS:=$(shell $(CAPI_PKG_CONFIG) --cflags libffi)
CAPI_LIBS+=$(shell $(CAPI_PKG_CONFIG) --libs libffi)
endif
endif
# readline: GNU readline, else libedit (CPython's --with-readline=editline)
ifndef CONFIG_PYTHON_CAPI_NO_READLINE
ifeq ($(shell $(CAPI_PKG_CONFIG) --exists readline && echo y),y)
CAPI_CONDS+=readline
CAPI_READLINE_CFLAGS:=$(shell $(CAPI_PKG_CONFIG) --cflags readline)
CAPI_LIBS+=$(shell $(CAPI_PKG_CONFIG) --libs readline)
else ifeq ($(shell $(CAPI_PKG_CONFIG) --exists libedit && echo y),y)
CAPI_CONDS+=readline
CAPI_READLINE_CFLAGS:=-DWITH_EDITLINE $(shell $(CAPI_PKG_CONFIG) --cflags libedit)
CAPI_LIBS+=$(shell $(CAPI_PKG_CONFIG) --libs libedit)
endif
endif
# _uuid: libSystem on macOS, libuuid elsewhere
ifndef CONFIG_DARWIN
ifeq ($(shell $(CAPI_PKG_CONFIG) --exists uuid && echo y),y)
CAPI_CONDS+=libuuid
CAPI_LIBUUID_CFLAGS:=$(shell $(CAPI_PKG_CONFIG) --cflags uuid)
CAPI_LIBS+=$(shell $(CAPI_PKG_CONFIG) --libs uuid)
endif
endif
ifdef CONFIG_PYTHON_CAPI_SYSTEM_EXPAT
CAPI_CONDS+=system_expat
CAPI_SYSTEM_EXPAT_CFLAGS:=$(shell $(CAPI_PKG_CONFIG) --cflags expat 2>/dev/null)
CAPI_LIBS+=$(or $(shell $(CAPI_PKG_CONFIG) --libs expat 2>/dev/null),-lexpat)
else
CAPI_CONDS+=expat
endif
ifneq ($(findstring x86_64,$(shell $(CC) -dumpmachine)),)
CAPI_CONDS+=x86_64
endif
ifdef CONFIG_DARWIN
CAPI_CONDS+=darwin
# _scproxy (urllib.request's proxy settings on macOS)
CAPI_CONDS+=scproxy
CAPI_LIBS+=-framework SystemConfiguration -framework CoreFoundation
endif

# CPython's module build flags (BASECFLAGS/CFLAGS_NODIST of a release
# build), independent of the warning set of the quickjs sources
CAPI_CFLAGS=-std=c11 -O2 -g -DNDEBUG -fno-strict-overflow -fvisibility=hidden \
	-Wall -Werror=implicit-function-declaration \
	-Icapi/include -Icapi/compat -Icapi/modules -DPy_BUILD_CORE_MODULE \
	$(filter -fsanitize=% -fno-omit-frame-pointer,$(CFLAGS)) -MMD -MP

# The object lists and the two inittabs follow Setup.qp and CAPI_CONDS;
# they are regenerated at parse time and rewritten only when they change.
ifeq ($(filter clean,$(MAKECMDGOALS)),)
CAPI_GEN:=$(shell mkdir -p $(OBJDIRS) $(CAPI_OBJDIR) && \
  for m in mk core_inittab ext_inittab; do \
    if [ $$m = mk ]; then f=$(CAPI_OBJDIR)/Setup.mk; else f=$(CAPI_OBJDIR)/capi_$$m.c; fi; \
    awk -v mode=$$m -v conds="$(strip $(CAPI_CONDS))" -f capi/setup.awk capi/Setup.qp > $$f.new || exit 1; \
    if cmp -s $$f.new $$f; then rm -f $$f.new; else mv -f $$f.new $$f; fi; \
  done && echo ok)
ifneq ($(CAPI_GEN),ok)
$(error capi/setup.awk failed on capi/Setup.qp)
endif
include $(CAPI_OBJDIR)/Setup.mk
endif

# engine library part (with quickjs.o) and launcher part ($(CAPI_LIB))
QJS_CAPI_OBJS=$(CAPI_CORE_MODULE_OBJS) $(CAPI_OBJDIR)/qp_internals.o $(CAPI_OBJDIR)/capi_core_inittab.o
CAPI_EXT_OBJS=$(CAPI_EXT_MODULE_OBJS) $(CAPI_OBJDIR)/capi_ext_inittab.o
CAPI_OBJS=$(QJS_CAPI_OBJS) $(CAPI_EXT_OBJS)

# qpy and qjs export the C API to dlopen'ed abi3 modules (M5)
ifdef CONFIG_DARWIN
QPY_LDEXPORT=-Wl,-export_dynamic
else
QPY_LDEXPORT=-rdynamic
endif
# the launchers (qpy, qjs) link the external modules
LAUNCHER_CAPI_LIB=$(CAPI_LIB)
LAUNCHER_CAPI_LIBS=$(CAPI_LIBS)
endif

qjs$(EXE): $(QJS_OBJS) $(QJS_CAPI_OBJS) $(LAUNCHER_CAPI_LIB)
	$(CC) $(LDFLAGS) $(or $(QPY_LDEXPORT),$(LDEXPORT)) -o $@ $^ $(LIBS) $(LAUNCHER_CAPI_LIBS)

qjs-debug$(EXE): $(patsubst %.o, %.debug.o, $(QJS_OBJS)) $(QJS_CAPI_OBJS) $(LAUNCHER_CAPI_LIB)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS) $(LAUNCHER_CAPI_LIBS)

qjsc$(EXE): $(OBJDIR)/qjsc.o $(QJS_LIB_OBJS) $(QJS_CAPI_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)


# qpy-base.c: the base snapshot and the frozen modules of qpy, none for
# qpy-nobase, the qpy that writes them (qpy --write-frozen-modules, then
# qpy --write-base-snapshot with them; FILE.d lists the stdlib sources
# each was made from)
ifdef CONFIG_QPY_NO_BASE
QPY_BASE_OBJ=$(OBJDIR)/qpy-nobase.o
else
QPY_BASE=$(OBJDIR)/qpy-base.bin
QPY_FROZEN=$(OBJDIR)/qpy-frozen.bin
QPY_BASE_OBJ=$(OBJDIR)/qpy-base.o
endif

qpy$(EXE): $(OBJDIR)/qpy.o $(QJS_LIB_OBJS) $(QJS_CAPI_OBJS) $(LAUNCHER_CAPI_LIB) $(QPY_BASE_OBJ)
	$(CC) $(LDFLAGS) $(QPY_LDEXPORT) -o $@ $^ $(LIBS) $(LAUNCHER_CAPI_LIBS)

$(OBJDIR)/qpy-nobase$(EXE): $(OBJDIR)/qpy.o $(QJS_LIB_OBJS) $(QJS_CAPI_OBJS) $(LAUNCHER_CAPI_LIB) $(OBJDIR)/qpy-nobase.o
	$(CC) $(LDFLAGS) $(QPY_LDEXPORT) -o $@ $^ $(LIBS) $(LAUNCHER_CAPI_LIBS)

$(OBJDIR)/qpy-nobase.o: qpy-base.c | $(OBJDIR)
	$(CC) $(CFLAGS_OPT) -c -o $@ $<

ifndef CONFIG_QPY_NO_BASE
$(OBJDIR)/qpy-base.o: qpy-base.c $(QPY_BASE) $(QPY_FROZEN) | $(OBJDIR)
	$(CC) $(CFLAGS_OPT) -DQPY_BASE_FILE='"$(abspath $(QPY_BASE))"' \
	    -DQPY_FROZEN_FILE='"$(abspath $(QPY_FROZEN))"' -c -o $@ $<

$(QPY_FROZEN): $(OBJDIR)/qpy-nobase$(EXE)
	$(OBJDIR)/qpy-nobase$(EXE) --write-frozen-modules $@ $(CURDIR)

$(QPY_BASE): $(OBJDIR)/qpy-nobase$(EXE) $(QPY_FROZEN)
	$(OBJDIR)/qpy-nobase$(EXE) --write-base-snapshot $@ $(CURDIR) $(QPY_FROZEN)

-include $(QPY_FROZEN).d
-include $(QPY_BASE).d
endif

ifdef CONFIG_PYTHON
# python/*.c are #included by quickjs.c (see python/quickpy.c)
PY_SRCS=$(wildcard python/*.c python/*.h)
ifdef CONFIG_PYTHON_CAPI
PY_SRCS+=$(wildcard capi/include/*.h capi/include/cpython/*.h) capi/compat/pycore_qp_tstate.h
endif
$(OBJDIR)/quickjs.o $(OBJDIR)/quickjs.nolto.o $(OBJDIR)/quickjs.debug.o \
$(OBJDIR)/quickjs.check.o $(OBJDIR)/quickjs.fuzz.o $(OBJDIR)/quickjs.host.o \
$(OBJDIR)/quickjs.pic.o: $(PY_SRCS) $(OBJDIR)/capi-config.stamp
$(OBJDIR)/qpy.o: python/quickpy.h $(OBJDIR)/capi-config.stamp
# toggling CONFIG_PYTHON_CAPI rebuilds the objects that depend on it: the
# stamp is rewritten at parse time only when the setting changes
ifeq ($(filter clean,$(MAKECMDGOALS)),)
CAPI_STAMP_GEN:=$(shell mkdir -p $(OBJDIRS) && f=$(OBJDIR)/capi-config.stamp && \
  echo "CONFIG_PYTHON_CAPI=$(CONFIG_PYTHON_CAPI)" > $$f.new && \
  if cmp -s $$f.new $$f; then rm -f $$f.new; else mv -f $$f.new $$f; fi)
endif
$(OBJDIR)/capi-config.stamp: ;
endif

ifdef CONFIG_PYTHON_CAPI
$(OBJDIR)/qpy.o $(OBJDIR)/qjs.o: capi/qpcapi.h

$(CAPI_OBJDIR)/modules/%.o: capi/modules/%.c
	@mkdir -p $(@D)
	$(CC) $(CAPI_CFLAGS) $(CAPI_SRC_CFLAGS) -c -o $@ $<

$(CAPI_OBJDIR)/qp_internals.o: capi/compat/qp_internals.c
	@mkdir -p $(@D)
	$(CC) $(CAPI_CFLAGS) -c -o $@ $<

$(CAPI_OBJDIR)/capi_%_inittab.o: $(CAPI_OBJDIR)/capi_%_inittab.c capi/qpcapi.h
	$(CC) $(CAPI_CFLAGS) -Icapi -c -o $@ $<

$(CAPI_LIB): $(CAPI_EXT_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# the CPython modules only (does not need the quickjs.c TU)
.PHONY: capi-modules
capi-modules: $(CAPI_OBJS) $(CAPI_LIB)

-include $(CAPI_OBJS:.o=.d)
endif

# capi/include, capi/compat copies and capi/modules from cpython/ at
# $(CPYTHON_TAG) + the recorded PATCHES (`--record` after editing them)
.PHONY: capi-sync capi-check
capi-sync:
	$(HOST_PYTHON) tools/capi_sync.py

capi-check:
	$(HOST_PYTHON) tools/capi_sync.py --check

# regenerate the generated Python front-end sources (needs CPython 3.15;
# HOST_PYTHON is defined with the Python test targets below)
.PHONY: py-regen
py-regen:
	export HOST_PYTHON="$(HOST_PYTHON)"; \
	sh python/gen_ast.sh && sh python/gen_token.sh && sh python/gen_parser.sh

fuzz_eval: $(OBJDIR)/fuzz_eval.o $(OBJDIR)/fuzz_common.o libquickjs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_eval $(LIB_FUZZING_ENGINE)

fuzz_compile: $(OBJDIR)/fuzz_compile.o $(OBJDIR)/fuzz_common.o libquickjs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_compile $(LIB_FUZZING_ENGINE)

fuzz_regexp: $(OBJDIR)/fuzz_regexp.o $(OBJDIR)/libregexp.fuzz.o $(OBJDIR)/cutils.fuzz.o $(OBJDIR)/libunicode.fuzz.o
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_regexp $(LIB_FUZZING_ENGINE)

fuzz_py_compile: $(OBJDIR)/fuzz_py_compile.o libquickjs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_py_compile $(LIB_FUZZING_ENGINE)

libfuzzer: fuzz_eval fuzz_compile fuzz_regexp
ifdef CONFIG_PYTHON
libfuzzer: fuzz_py_compile
endif

ifneq ($(CROSS_PREFIX),)

$(QJSC): $(OBJDIR)/qjsc.host.o \
    $(patsubst %.o, %.host.o, $(QJS_LIB_OBJS))
	$(HOST_CC) $(LDFLAGS) -o $@ $^ $(HOST_LIBS)

endif #CROSS_PREFIX

QJSC_DEFINES:=-DCONFIG_CC=\"$(QJSC_CC)\" -DCONFIG_PREFIX=\"$(PREFIX)\"
ifdef CONFIG_LTO
QJSC_DEFINES+=-DCONFIG_LTO
endif
QJSC_HOST_DEFINES:=-DCONFIG_CC=\"$(HOST_CC)\" -DCONFIG_PREFIX=\"$(PREFIX)\"

$(OBJDIR)/qjsc.o: CFLAGS+=$(QJSC_DEFINES)
$(OBJDIR)/qjsc.host.o: CFLAGS+=$(QJSC_HOST_DEFINES)

ifdef CONFIG_LTO
LTOEXT=.lto
else
LTOEXT=
endif

libquickjs$(LTOEXT).a: $(QJS_LIB_OBJS) $(QJS_CAPI_OBJS)
	$(AR) rcs $@ $^

ifdef CONFIG_LTO
libquickjs.a: $(patsubst %.o, %.nolto.o, $(QJS_LIB_OBJS)) $(QJS_CAPI_OBJS)
	$(AR) rcs $@ $^
endif # CONFIG_LTO

libquickjs.fuzz.a: $(patsubst %.o, %.fuzz.o, $(QJS_LIB_OBJS)) $(QJS_CAPI_OBJS)
	$(AR) rcs $@ $^

repl.c: $(QJSC) repl.js
	$(QJSC) -s -c -o $@ -m repl.js

ifneq ($(wildcard unicode/UnicodeData.txt),)
$(OBJDIR)/libunicode.o $(OBJDIR)/libunicode.nolto.o: libunicode-table.h

libunicode-table.h: unicode_gen
	./unicode_gen unicode $@
endif

run-test262$(EXE): $(OBJDIR)/run-test262.o $(QJS_LIB_OBJS) $(QJS_CAPI_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

run-test262-debug: $(patsubst %.o, %.debug.o, $(OBJDIR)/run-test262.o $(QJS_LIB_OBJS)) $(QJS_CAPI_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

tests/test_std_api$(EXE): $(OBJDIR)/tests/test_std_api.o $(QJS_LIB_OBJS) $(QJS_CAPI_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

tests/test_api$(EXE): $(OBJDIR)/tests/test_api.o $(QJS_LIB_OBJS) $(QJS_CAPI_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# object suffix order: nolto

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS_OPT) -c -o $@ $<

$(OBJDIR)/fuzz_%.o: fuzz/fuzz_%.c | $(OBJDIR)
	$(CC) $(CFLAGS_OPT) -c -I. -o $@ $<

# host tools (qjsc, unicode_gen) do not embed the C modules
$(OBJDIR)/%.host.o: %.c | $(OBJDIR)
	$(HOST_CC) $(CFLAGS_OPT) -UCONFIG_PYTHON_CAPI -c -o $@ $<

$(OBJDIR)/%.pic.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS_OPT) -fPIC -DJS_SHARED_LIBRARY -c -o $@ $<

$(OBJDIR)/%.nolto.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS_NOLTO) -c -o $@ $<

$(OBJDIR)/%.debug.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS_DEBUG) -c -o $@ $<

$(OBJDIR)/%.fuzz.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS_OPT) -fsanitize=fuzzer-no-link -c -o $@ $<

$(OBJDIR)/%.check.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -DCONFIG_CHECK_JSVALUE -c -o $@ $<

regexp_test: libregexp.c libunicode.c cutils.c
	$(CC) $(LDFLAGS) $(CFLAGS) -DTEST -o $@ libregexp.c libunicode.c cutils.c $(LIBS)

unicode_gen: $(OBJDIR)/unicode_gen.host.o $(OBJDIR)/cutils.host.o libunicode.c unicode_gen_def.h
	$(HOST_CC) $(LDFLAGS) $(CFLAGS) -o $@ $(OBJDIR)/unicode_gen.host.o $(OBJDIR)/cutils.host.o

clean:
	rm -f repl.c out.c
	rm -f *.a *.o *.d *~ unicode_gen regexp_test fuzz_eval fuzz_compile fuzz_regexp $(PROGS)
	rm -f qpy$(EXE) fuzz_py_compile
	rm -rf build/testpy/capi-testmods
	rm -f hello.c test_fib.c
	rm -f examples/*.so tests/*.so tests/test_std_api$(EXE) tests/test_api$(EXE)
	rm -f tests/test_qjsc_unused tests/test_qjsc_modules tests/test_qjsc_modules.c
	rm -rf $(OBJDIR)/ *.dSYM/ qjs-debug$(EXE)
	rm -rf run-test262-debug$(EXE)
	rm -f run_octane run_sunspider_like

install: all
	mkdir -p "$(DESTDIR)$(PREFIX)/bin"
	$(STRIP) qjs$(EXE) qjsc$(EXE)
	install -m755 qjs$(EXE) qjsc$(EXE) "$(DESTDIR)$(PREFIX)/bin"
	mkdir -p "$(DESTDIR)$(PREFIX)/lib/quickjs"
	install -m644 libquickjs.a "$(DESTDIR)$(PREFIX)/lib/quickjs"
ifdef CONFIG_LTO
	install -m644 libquickjs.lto.a "$(DESTDIR)$(PREFIX)/lib/quickjs"
endif
	mkdir -p "$(DESTDIR)$(PREFIX)/include/quickjs"
	install -m644 quickjs.h quickjs-libc.h "$(DESTDIR)$(PREFIX)/include/quickjs"

###############################################################################
# examples

# example of static JS compilation
HELLO_SRCS=examples/hello.js
HELLO_OPTS=-fno-string-normalize -fno-map -fno-promise -fno-typedarray \
           -fno-typedarray -fno-regexp -fno-json -fno-eval -fno-proxy \
           -fno-date -fno-module-loader

hello.c: $(QJSC) $(HELLO_SRCS)
	$(QJSC) -e $(HELLO_OPTS) -o $@ $(HELLO_SRCS)

examples/hello: $(OBJDIR)/hello.o $(QJS_LIB_OBJS) $(QJS_CAPI_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# example of static JS compilation with modules
HELLO_MODULE_SRCS=examples/hello_module.js
HELLO_MODULE_OPTS=-fno-string-normalize -fno-map -fno-typedarray \
           -fno-typedarray -fno-regexp -fno-json -fno-eval -fno-proxy \
           -fno-date -m
examples/hello_module: $(QJSC) libquickjs$(LTOEXT).a $(HELLO_MODULE_SRCS)
	$(QJSC) $(HELLO_MODULE_OPTS) -o $@ $(HELLO_MODULE_SRCS)

# use of an external C module (static compilation)

test_fib.c: $(QJSC) examples/test_fib.js
	$(QJSC) -e -M examples/fib.so,fib -m -o $@ examples/test_fib.js

examples/test_fib: $(OBJDIR)/test_fib.o $(OBJDIR)/examples/fib.o libquickjs$(LTOEXT).a
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

examples/fib.so: $(OBJDIR)/examples/fib.pic.o
	$(CC) $(LDFLAGS) -shared -o $@ $^

examples/point.so: $(OBJDIR)/examples/point.pic.o
	$(CC) $(LDFLAGS) -shared -o $@ $^

###############################################################################
# documentation

DOCS=doc/quickjs.pdf doc/quickjs.html

build_doc: $(DOCS)

clean_doc:
	rm -f $(DOCS)

doc/version.texi: VERSION
	@echo "@set VERSION `cat $<`" > $@

doc/%.pdf: doc/%.texi doc/version.texi
	texi2pdf --clean -o $@ -q $<

doc/%.html.pre: doc/%.texi doc/version.texi
	makeinfo --html --no-headers --no-split --number-sections -o $@ $<

doc/%.html: doc/%.html.pre
	sed -e 's|</style>|</style>\n<meta name="viewport" content="width=device-width, initial-scale=1.0">|' < $< > $@

###############################################################################
# tests

ifdef CONFIG_SHARED_LIBS
test: examples/point.so
endif

test: qjs$(EXE) tests/test_std_api$(EXE) tests/test_api$(EXE) $(QJSC_TESTS)
	$(WINE) ./tests/test_api$(EXE)
	$(WINE) ./qjs$(EXE) tests/test_closure.js
	$(WINE) ./qjs$(EXE) tests/test_language.js
	$(WINE) ./qjs$(EXE) --std tests/test_builtin.js
	$(WINE) ./qjs$(EXE) tests/test_stack_trace.js
	$(WINE) ./qjs$(EXE) tests/test_loop.js
	$(WINE) ./qjs$(EXE) tests/test_bigint.js
	$(WINE) ./qjs$(EXE) tests/test_cyclic_import.js
	$(WINE) ./qjs$(EXE) tests/test_destructured_export.js
	$(WINE) ./qjs$(EXE) tests/test_typescript.ts
	$(WINE) ./qjs$(EXE) --typescript -e 'let x: number = 1; if (x !== 1) throw Error("--typescript")'
	$(WINE) ./qjs$(EXE) tests/test_decorators.js
	$(WINE) ./qjs$(EXE) --std --emit-decorator-metadata tests/test_decorators_legacy.ts
	$(WINE) ./qjs$(EXE) tests/test_ng_fixes.js
	$(WINE) ./qjs$(EXE) tests/test_dynamic_import_line_numbers.js
	$(WINE) ./qjs$(EXE) tests/test_detect_module.js
	$(WINE) ./qjs$(EXE) tests/test_using.js
	$(WINE) ./qjs$(EXE) tests/test_worker.js
	$(WINE) ./qjs$(EXE) tests/test_bjson.js
ifndef CONFIG_WIN32
	$(WINE) ./qjs$(EXE) tests/test_std.js
	$(WINE) ./tests/test_std_api$(EXE)
	$(WINE) ./qjs$(EXE) tests/test_rw_handler.js
endif
ifdef CONFIG_SHARED_LIBS
	$(WINE) ./qjs$(EXE) examples/test_point.js
endif
ifdef QJSC_TESTS
	./tests/test_qjsc_unused
	./tests/test_qjsc_modules
endif
ifdef CONFIG_TEMPORAL_INTL
	$(WINE) ./qjs$(EXE) --experimental tests/test_intl.js
endif
ifdef CONFIG_PYTHON
ifndef CONFIG_WIN32
	./tests/test_drepl_python.sh ./qjs$(EXE)
endif
endif

tests/test_qjsc_unused: $(QJSC) libquickjs$(LTOEXT).a tests/test_qjsc_unused.js
	$(QJSC) -fno-unused -o $@ tests/test_qjsc_unused.js

tests/test_qjsc_modules.c: $(QJSC) tests/test_qjsc_modules.js \
    tests/fixture_bytes_module.txt tests/fixture_import_attributes.json
	$(QJSC) -e -fno-unused -fno-module-loader -M examples/fib.so,fib -m -o $@ tests/test_qjsc_modules.js

$(OBJDIR)/tests/test_qjsc_modules.o: CFLAGS+=-I.

tests/test_qjsc_modules: $(OBJDIR)/tests/test_qjsc_modules.o $(OBJDIR)/examples/fib.o libquickjs$(LTOEXT).a
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# suspension and heap snapshots (see tests/snapshot/Makefile)
test-snapshot:
	$(MAKE) -C tests/snapshot check

ifdef CONFIG_PYTHON_CAPI
# snapshots of C-extension state (C modules of libqpcapi.a), one process
# per phase
$(OBJDIR)/tests/test_python_capi_snapshot.o: tests/snapshot/test_python_capi_snapshot.c capi/qpcapi.h | $(OBJDIR)
	$(CC) $(CFLAGS_OPT) -I. -c -o $@ $<

$(OBJDIR)/tests/test_python_capi_snapshot$(EXE): $(OBJDIR)/tests/test_python_capi_snapshot.o $(QJS_LIB_OBJS) $(QJS_CAPI_OBJS) $(LAUNCHER_CAPI_LIB)
	$(CC) $(LDFLAGS) $(QPY_LDEXPORT) -o $@ $^ $(LIBS) $(LAUNCHER_CAPI_LIBS)

test-snapshot-capi: $(OBJDIR)/tests/test_python_capi_snapshot$(EXE)
	QPYHOME=$(CURDIR) ./$(OBJDIR)/tests/test_python_capi_snapshot$(EXE)
endif

ifdef CONFIG_PYTHON
# an embedder using Python through the public API, then JS_FreeContext /
# JS_FreeRuntime (rquickjs): the teardown frees everything
$(OBJDIR)/tests/test_embed_teardown.o: tests/py/embed/test_embed_teardown.c quickjs.h | $(OBJDIR)
	$(CC) $(CFLAGS_OPT) -c -o $@ $<

$(OBJDIR)/tests/test_embed_teardown$(EXE): $(OBJDIR)/tests/test_embed_teardown.o $(QJS_LIB_OBJS) $(QJS_CAPI_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

test-embed: $(OBJDIR)/tests/test_embed_teardown$(EXE)
	./$(OBJDIR)/tests/test_embed_teardown$(EXE) $(CURDIR)
.PHONY: test-embed
endif

stats: qjs$(EXE)
	$(WINE) ./qjs$(EXE) -qd

microbench: qjs$(EXE)
	$(WINE) ./qjs$(EXE) --std tests/microbench.js

ifeq ($(wildcard test262/features.txt),)
test2-bootstrap:
	git clone --single-branch --shallow-since=$(TEST262_SINCE) https://github.com/tc39/test262.git
	(cd test262 && git checkout -q $(TEST262_COMMIT) && patch -p1 < ../tests/test262.patch && cd ..)
else
test2-bootstrap:
	(cd test262 && git fetch && git reset --hard $(TEST262_COMMIT) && patch -p1 < ../tests/test262.patch && cd ..)
endif

ifeq ($(wildcard test262o/tests.txt),)
test2o test2o-update:
	@echo test262o tests not installed
else
# ES5 tests (obsolete)
test2o: run-test262
	time ./run-test262 -t -m -c test262o.conf

test2o-update: run-test262
	./run-test262 -t -u -c test262o.conf
endif

ifeq ($(wildcard test262/features.txt),)
test2 test2-update test2-default test2-check:
	@echo test262 tests not installed
else
# Test262 tests
test2-default: run-test262
	time ./run-test262 -t -m -c test262.conf

test2: run-test262
	time ./run-test262 -t -m -c test262.conf -a

test2-update: run-test262
	./run-test262 -t -u -c test262.conf -a

test2-check: run-test262
	time ./run-test262 -t -m -c test262.conf -E -a
endif

# Python support tests (see doc/python-plan.md and tests/py/run.py)
HOST_PYTHON ?= python3.15
CPYTHON_TAG = v3.15.2
CPYTHON_DIR = cpython

.PHONY: testpy-bootstrap testpy testpy-update testpy-golden testpy-ast \
	testpy-l3b testpy-l3c testpy-corpus testpy-replay

testpy-bootstrap:
	if [ ! -d $(CPYTHON_DIR) ]; then \
	  git clone --depth 1 --branch $(CPYTHON_TAG) https://github.com/python/cpython $(CPYTHON_DIR); \
	else \
	  git -C $(CPYTHON_DIR) fetch --depth 1 origin tag $(CPYTHON_TAG) && \
	  git -C $(CPYTHON_DIR) checkout -q -f $(CPYTHON_TAG) && \
	  git -C $(CPYTHON_DIR) reset -q --hard; \
	fi
	if [ -f tests/cpython.patch ]; then \
	  patch -p1 -d $(CPYTHON_DIR) < tests/cpython.patch; \
	fi
# `test` is part of the stdlib of a CPython install: qpy -I subprocesses
# of Lib/test import it from pylib/test (tests/py/run.py l3 makes it too)
	if [ ! -L pylib/test ]; then ln -s ../$(CPYTHON_DIR)/Lib/test pylib/test; fi

testpy:
	$(HOST_PYTHON) tests/py/run.py l0
	$(HOST_PYTHON) tests/py/run.py ast --lib
	$(HOST_PYTHON) tests/py/run.py l3 --set l3a

testpy-update:
	$(HOST_PYTHON) tests/py/run.py l0 -u
	$(HOST_PYTHON) tests/py/run.py l3 --set all -u

testpy-golden:
	$(HOST_PYTHON) tests/py/run.py golden-regen

testpy-ast:
	$(HOST_PYTHON) tests/py/run.py ast --lib --corpus

testpy-l3b:
	$(HOST_PYTHON) tests/py/run.py l3 --set l3b

testpy-l3c:
	$(HOST_PYTHON) tests/py/run.py l3 --set l3c

testpy-corpus:
	$(HOST_PYTHON) tests/py/corpus.py extract

testpy-replay:
	$(HOST_PYTHON) tests/py/run.py replay

# Test-only C API modules (not part of qpy): CPython's _testlimitedcapi,
# compiled from $(CPYTHON_DIR)/Modules against capi/include as the stable-ABI
# extension $(CAPI_TESTMODS_DIR)/_testlimitedcapi.abi3.so (it uses only the
# limited API), loaded through python/capi_dynload.c.  tests/py/run.py puts
# $(CAPI_TESTMODS_DIR) on the PYTHONPATH of the L3 runs, so that the Lib/test
# modules that import _testlimitedcapi (test_bytes, test_capi, ...) run it.
# _testmultiphase and _testsinglephase (PEP 489 / single-phase init tests
# of test_importlib, test_import, ...) are built the same way as
# version-specific extensions; they use the internal API of capi/compat.
# _testcapi (the full, version-specific API) is built the same way from its
# vendored copy in capi/modules, whose PATCHES compile out what tests CPython
# internals that do not exist here (tools/capi_sync.py TEST_MODULE_SOURCES).
ifdef CONFIG_PYTHON_CAPI
CAPI_TESTMODS_DIR=build/testpy/capi-testmods
CAPI_TESTMODS_OBJDIR=$(OBJDIR)/capi-testmods
CAPI_TESTLIMITEDCAPI_SRCS=$(CPYTHON_DIR)/Modules/_testlimitedcapi.c \
	$(sort $(wildcard $(CPYTHON_DIR)/Modules/_testlimitedcapi/*.c))
CAPI_TESTLIMITEDCAPI_OBJS=$(patsubst $(CPYTHON_DIR)/Modules/%.c,$(CAPI_TESTMODS_OBJDIR)/%.o,$(CAPI_TESTLIMITEDCAPI_SRCS))
CAPI_TESTMODS_INTERNAL=_testmultiphase _testsinglephase
CAPI_TESTCAPI_SRCS=capi/modules/_testcapimodule.c \
	$(sort $(wildcard capi/modules/_testcapi/*.c))
CAPI_TESTCAPI_OBJS=$(patsubst capi/modules/%.c,$(CAPI_TESTMODS_OBJDIR)/testcapi/%.o,$(CAPI_TESTCAPI_SRCS))
# "." QP_SOABI ".so" (python/sysmodule.c QP_MULTIARCH): the first entry of
# importlib.machinery.EXTENSION_SUFFIXES
ifdef CONFIG_DARWIN
CAPI_TESTMODS_EXT_SUFFIX=.quickpy-315-darwin.so
else
CAPI_TESTMODS_EXT_SUFFIX=.quickpy-315-$(shell uname -m)-linux-gnu.so
endif
# CPython's CFLAGS of a release build plus CCSHARED, without
# Py_BUILD_CORE_MODULE: the modules are external, as in CPython
CAPI_TESTMODS_CFLAGS=-std=c11 -O2 -g -DNDEBUG -fno-strict-overflow -fPIC \
	-fvisibility=hidden -Wall -Werror=implicit-function-declaration -Icapi/include \
	$(filter -fsanitize=% -fno-omit-frame-pointer,$(CFLAGS)) -MMD -MP
ifdef CONFIG_DARWIN
CAPI_TESTMODS_LDSHARED=-bundle -undefined dynamic_lookup
else
CAPI_TESTMODS_LDSHARED=-shared
endif

.PHONY: capi-testmods
capi-testmods: $(CAPI_TESTMODS_DIR)/_testlimitedcapi.abi3.so \
	$(CAPI_TESTMODS_DIR)/_testcapi$(CAPI_TESTMODS_EXT_SUFFIX) \
	$(patsubst %,$(CAPI_TESTMODS_DIR)/%$(CAPI_TESTMODS_EXT_SUFFIX),$(CAPI_TESTMODS_INTERNAL))

$(CAPI_TESTMODS_DIR)/_testlimitedcapi.abi3.so: $(CAPI_TESTLIMITEDCAPI_OBJS)
	@mkdir -p $(@D)
	$(CC) $(CAPI_TESTMODS_LDSHARED) $(filter -fsanitize=%,$(CFLAGS)) -o $@ $^

$(CAPI_TESTMODS_DIR)/_testcapi$(CAPI_TESTMODS_EXT_SUFFIX): $(CAPI_TESTCAPI_OBJS)
	@mkdir -p $(@D)
	$(CC) $(CAPI_TESTMODS_LDSHARED) $(filter -fsanitize=%,$(CFLAGS)) -o $@ $^

$(CAPI_TESTCAPI_OBJS): CAPI_TESTMODS_CFLAGS+=-Icapi/compat

$(CAPI_TESTMODS_OBJDIR)/testcapi/%.o: capi/modules/%.c
	@mkdir -p $(@D)
	$(CC) $(CAPI_TESTMODS_CFLAGS) -c -o $@ $<

$(CAPI_TESTMODS_DIR)/%$(CAPI_TESTMODS_EXT_SUFFIX): $(CAPI_TESTMODS_OBJDIR)/%.o
	@mkdir -p $(@D)
	$(CC) $(CAPI_TESTMODS_LDSHARED) $(filter -fsanitize=%,$(CFLAGS)) -o $@ $^

$(patsubst %,$(CAPI_TESTMODS_OBJDIR)/%.o,$(CAPI_TESTMODS_INTERNAL)): CAPI_TESTMODS_CFLAGS+=-Icapi/compat

$(CAPI_TESTMODS_OBJDIR)/%.o: $(CPYTHON_DIR)/Modules/%.c
	@mkdir -p $(@D)
	$(CC) $(CAPI_TESTMODS_CFLAGS) -c -o $@ $<

-include $(CAPI_TESTLIMITEDCAPI_OBJS:.o=.d) $(CAPI_TESTCAPI_OBJS:.o=.d) \
	$(patsubst %,$(CAPI_TESTMODS_OBJDIR)/%.d,$(CAPI_TESTMODS_INTERNAL))

# the L3 runs import them
testpy testpy-update testpy-l3b testpy-l3c: capi-testmods
endif

.PHONY: bench-py
bench-py:
	$(HOST_PYTHON) tests/py/bench/run.py

testall: all test microbench test2o test2

testall-complete: testall

node-test:
	node tests/test_closure.js
	node tests/test_language.js
	node tests/test_builtin.js
	node tests/test_loop.js
	node tests/test_bigint.js

node-microbench:
	node tests/microbench.js -s microbench-node.txt
	node --jitless tests/microbench.js -s microbench-node-jitless.txt

bench-v8: qjs
	make -C tests/bench-v8
	./qjs -d tests/bench-v8/combined.js

node-bench-v8:
	make -C tests/bench-v8
	node --jitless tests/bench-v8/combined.js

BENCHMARKDIR=../quickjs-benchmarks

run_sunspider_like: $(BENCHMARKDIR)/run_sunspider_like.c
	$(CC) $(CFLAGS) $(LDFLAGS) -DNO_INCLUDE_DIR -I. -o $@ $< libquickjs$(LTOEXT).a $(LIBS)

run_octane: $(BENCHMARKDIR)/run_octane.c
	$(CC) $(CFLAGS) $(LDFLAGS) -DNO_INCLUDE_DIR -I. -o $@ $< libquickjs$(LTOEXT).a $(LIBS)

benchmarks: run_sunspider_like run_octane
	./run_sunspider_like $(BENCHMARKDIR)/kraken-1.0/
	./run_sunspider_like $(BENCHMARKDIR)/kraken-1.1/
	./run_sunspider_like $(BENCHMARKDIR)/sunspider-1.0/
	./run_octane $(BENCHMARKDIR)/

-include $(wildcard $(OBJDIR)/*.d)

# Pinned pure-Python stdlib (cpython/Lib at $(CPYTHON_TAG) minus the test suite
# and Tk, pylib/MANIFEST.txt); `tools/pylib_sync.py --check` verifies,
# `--manifest` regenerates, `tools/pylib_sweep.py` import-checks it under ./qpy.
.PHONY: pylib-sync
pylib-sync:
	$(HOST_PYTHON) tools/pylib_sync.py
