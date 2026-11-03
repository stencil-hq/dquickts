# rquickjs-sys

[![github](https://img.shields.io/badge/github-stencil--hq/dquickts-8da0cb.svg?style=for-the-badge&logo=github)](https://github.com/stencil-hq/dquickts)

This crate is a low level unsafe raw bindings for the [QuickJS](https://bellard.org/quickjs/) JavaScript engine.

__NOTE:__ Usually you shouldn't use this crate directly, instead use [rquickjs](../README.md) crate which provides high level safe bindings.

## Engine

The build script compiles the QuickJS sources two directories above this crate (`quickjs.c`
with its snapshot code, `libregexp.c`, `libunicode.c`, `cutils.c`, `dtoa.c`), without Intl,
Temporal and `quickjs-libc`. The bundled bindings of `src/bindings` are generated from its
`quickjs.h` by `scripts/gen-bindings.sh`; other targets need the `bindgen` feature. Any `dump-*`
feature compiles the engine debug dumps in (`ENABLE_DUMPS`); they are selected per runtime by
`JS_SetDumpFlags()`.

## Environment Variables

The following environment variables can be used to control which WASI SDK is used during the build:

- `WASI_SDK`: Path to the WASI SDK to use during the build. If unset, this crate will download it automatically instead.
- `RQUICKJS_SYS_NO_WASI_SDK`: If set to `1`, this crate will not attempt to use the WASI SDK
  and instead the `CC`, `AR`, and `CFLAGS` environment variables must be appropriately set.
