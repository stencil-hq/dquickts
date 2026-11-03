/*
 * QuickJS C library
 *
 * Copyright (c) 2017-2018 Fabrice Bellard
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
#ifndef QUICKJS_LIBC_H
#define QUICKJS_LIBC_H

#include <stdio.h>
#include <stdlib.h>

#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

JSModuleDef *js_init_module_std(JSContext *ctx, const char *module_name);
JSModuleDef *js_init_module_os(JSContext *ctx, const char *module_name);
/* the 'bjson' module: JS_ReadObject() and JS_WriteObject() from JS */
JSModuleDef *js_init_module_bjson(JSContext *ctx, const char *module_name);
/* define the globals print(), console.log(), scriptArgs (if argc >= 0),
   setTimeout(), clearTimeout(), setInterval() and clearInterval() (the
   timers of the 'os' module: they need js_std_init_handlers()) */
void js_std_add_helpers(JSContext *ctx, int argc, char **argv);
void js_std_loop(JSContext *ctx);
/* one event of js_std_loop() after the jobs: wait for a signal, an
   expired timer, a file descriptor of os.setReadHandler() /
   os.setWriteHandler() or a worker message and run its handler. Return
   0 if an event was handled or the wait ended (e.g. by a signal), -1 if
   there is no timer and no handler left (or poll() failed) or -2 if the
   handler threw (the exception is pending in the runtime). */
int js_std_poll(JSContext *ctx);
/* functions called by the event loop of the runtime 'rt' around its
   waits (poll(), Sleep()) for a host whose other threads run code in the
   runtime under a lock (the GIL of the Python threads:
   JS_PythonSaveThread() and JS_PythonRestoreThread()): 'enter' releases
   it and returns a value which is passed to 'leave', which takes it
   again. A timer or a handler added by another thread during the wait
   ends it. Requires js_std_init_handlers(). */
void js_std_set_wait_hooks(JSRuntime *rt, void *(*enter)(JSRuntime *rt),
                           void (*leave)(JSRuntime *rt, void *state));
/* one step of js_std_loop() for a host which runs its own event loop:
   run the pending jobs, then the handler of at most one expired timer.
   Return 0 if there is more work to do now, the delay in ms until the
   next timer, -1 if there is no job and no timer (the I/O, signal and
   worker handlers are run by js_std_poll_io()) or -2 if a job or a
   handler threw (the exception is pending in the runtime). */
int js_std_loop_once(JSContext *ctx);
/* wait at most 'timeout_ms' ms (forever if < 0, unless there is nothing
   to wait for) for a signal, a file descriptor of
   os.setReadHandler()/os.setWriteHandler() or a worker message and run
   its handler. Return 0, -1 if poll() failed or -2 if the handler threw
   (the exception is pending in the runtime). The timers are not run. */
int js_std_poll_io(JSContext *ctx, int timeout_ms);
JSValue js_std_await(JSContext *ctx, JSValue obj);
void js_std_init_handlers(JSRuntime *rt);
void js_std_free_handlers(JSRuntime *rt);
void js_std_dump_error(JSContext *ctx);
uint8_t *js_load_file(JSContext *ctx, size_t *pbuf_len, const char *filename);
/* file reader of js_module_load(): return the contents of 'filename'
   allocated with js_malloc() or NULL */
typedef uint8_t *JSLoadFileFunc(JSContext *ctx, size_t *pbuf_len,
                                const char *filename);
int js_module_set_import_meta(JSContext *ctx, JSValueConst func_val,
                              JS_BOOL use_realpath, JS_BOOL is_main);
/* module types selected by the 'type' import attribute */
typedef enum {
    JS_MODULE_TYPE_JS,
    JS_MODULE_TYPE_JSON,
    JS_MODULE_TYPE_JSON5, /* JSON with the JS_PARSE_JSON_EXT extensions */
    JS_MODULE_TYPE_TEXT, /* default export: the file as a string */
    JS_MODULE_TYPE_BYTES, /* default export: the file as an Uint8Array */
} JSModuleTypeEnum;
/* return the JSModuleTypeEnum given by the 'type' import attribute
   (JS_MODULE_TYPE_JS if absent) or -1 with an exception if the type is
   not supported */
int js_module_get_type(JSContext *ctx, JSValueConst attributes);
/* return the default export of a module of a non JS 'type' whose file
   contents are 'buf'. 'buf' must be allocated with js_malloc() and is
   owned by the function, even in case of exception. */
JSValue js_module_new_value(JSContext *ctx, int type, uint8_t *buf,
                            size_t buf_len, const char *module_name);
int js_module_check_attributes(JSContext *ctx, void *opaque, JSValueConst attributes);
/* return JS_EVAL_FLAG_TYPESCRIPT if 'filename' has a TypeScript
   extension ('.ts', '.mts', '.cts'), 0 otherwise, or -1 with a
   SyntaxError for '.tsx' (JSX is not supported) */
int js_std_get_typescript_flag(JSContext *ctx, const char *filename);
/* set the JS_EVAL_FLAG_TS_LEGACY_DECORATORS and
   JS_EVAL_FLAG_TS_DECORATOR_METADATA flags added for the TypeScript
   sources by js_std_get_typescript_flag() and std.evalScript() (the
   other flags are ignored). The workers inherit them. */
void js_std_set_typescript_flags(JSRuntime *rt, int flags);
/* module name normalizer: the default normalization, then, if the file
   does not exist, './x.js', './x.mjs', './x.cjs' and './x' resolve to
   an existing './x.ts', './x.mts', './x.cts' and './x.ts' */
char *js_module_normalize(JSContext *ctx, const char *base_name,
                          const char *name, void *opaque);
/* module loader: compiles the TypeScript files as TypeScript (see
   js_std_get_typescript_flag()) */
JSModuleDef *js_module_loader(JSContext *ctx,
                              const char *module_name, void *opaque,
                              JSValueConst attributes);
/* js_module_loader() without the shared library ('.so') modules and
   with the file reader 'load_file' */
JSModuleDef *js_module_load(JSContext *ctx, const char *module_name,
                            void *opaque, JSValueConst attributes,
                            JSLoadFileFunc *load_file);
/* print() and console.log() of js_std_add_helpers(). Exported so that
   a JSSnapshotHost can list them in its natives (see qjs --drepl). */
JSValue js_std_print(JSContext *ctx, JSValueConst this_val,
                     int argc, JSValueConst *argv);
JSValue js_std_console_log(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv);
void js_std_eval_binary(JSContext *ctx, const uint8_t *buf, size_t buf_len,
                        int flags);
/* define the JSON, text or bytes module 'module_name' whose default
   export is the value serialized in 'buf'. 'type' is the 'type' import
   attribute of its imports ("json", "json5", "text" or "bytes") or NULL
   if they have no attributes. */
void js_std_eval_binary_json_module(JSContext *ctx,
                                    const uint8_t *buf, size_t buf_len,
                                    const char *module_name,
                                    const char *type);
void js_std_promise_rejection_tracker(JSContext *ctx, JSValueConst promise,
                                      JSValueConst reason,
                                      JS_BOOL is_handled, void *opaque);
/* create the runtime of the new workers (JS_NewRuntime() by default).
   No effect without worker support. */
void js_std_set_worker_new_runtime_func(JSRuntime *(*func)(void));
void js_std_set_worker_new_context_func(JSContext *(*func)(JSRuntime *rt));

#ifdef __cplusplus
} /* extern "C" { */
#endif

#endif /* QUICKJS_LIBC_H */
