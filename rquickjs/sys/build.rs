#![allow(clippy::uninlined_format_args)]
#[path = "build_capi.rs"]
mod capi;

use std::{
    env, fs,
    path::{Path, PathBuf},
    process::{self},
};

// WASI logic lifted from https://github.com/bytecodealliance/javy/blob/61616e1507d2bf896f46dc8d72687273438b58b2/crates/quickjs-wasm-sys/build.rs#L18

const WASI_SDK_VERSION_MAJOR: usize = 24;
const WASI_SDK_VERSION_MINOR: usize = 0;

fn download_wasi_sdk() -> PathBuf {
    let mut wasi_sdk_dir: PathBuf = env::var("OUT_DIR").unwrap().into();
    wasi_sdk_dir.push("wasi-sdk");

    fs::create_dir_all(&wasi_sdk_dir).unwrap();

    let major_version = WASI_SDK_VERSION_MAJOR;
    let minor_version = WASI_SDK_VERSION_MINOR;

    let mut archive_path = wasi_sdk_dir.clone();
    archive_path.push(format!("wasi-sdk-{major_version}-{minor_version}.tar.gz"));

    println!("SDK tar: {archive_path:?}");

    // Download archive if necessary
    if !archive_path.try_exists().unwrap() {
        let file_suffix = match (env::consts::OS, env::consts::ARCH) {
            ("linux", "x86") | ("linux", "x86_64") => "x86_64-linux",
            ("linux", "aarch64") => "arm64-linux",
            ("macos", "x86") | ("macos", "x86_64") => "x86_64-macos",
            ("macos", "aarch64") => "arm64-macos",
            ("windows", "x86") | ("windows", "x86_64") => "x86_64-windows",
            ("windows", "aarch64") => "arm64-windows",
            other => panic!("Unsupported platform tuple {:?}", other),
        };

        let uri = format!("https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-{major_version}/wasi-sdk-{major_version}.{minor_version}-{file_suffix}.tar.gz");

        println!("Downloading WASI SDK archive from {uri} to {archive_path:?}");

        let output = process::Command::new("curl")
            .args([
                "--location",
                "-o",
                archive_path.to_string_lossy().as_ref(),
                uri.as_ref(),
            ])
            .output()
            .expect("failed to download the WASI SDK with curl");
        println!("curl output: {}", String::from_utf8_lossy(&output.stdout));
        println!("curl err: {}", String::from_utf8_lossy(&output.stderr));
        if !output.status.success() {
            panic!(
                "curl WASI SDK failed: {}",
                String::from_utf8_lossy(&output.stderr)
            );
        }
    }

    let mut test_binary = wasi_sdk_dir.clone();
    test_binary.extend(["bin", "wasm-ld"]);
    // Extract archive if necessary
    if !test_binary.try_exists().unwrap() {
        println!("Extracting WASI SDK archive {archive_path:?}");
        let output = process::Command::new("tar")
            .args([
                "-zxf",
                archive_path.to_string_lossy().as_ref(),
                "--strip-components",
                "1",
            ])
            .current_dir(&wasi_sdk_dir)
            .output()
            .unwrap();
        if !output.status.success() {
            panic!(
                "Unpacking WASI SDK failed: {}",
                String::from_utf8_lossy(&output.stderr)
            );
        }
    }

    wasi_sdk_dir
}

fn get_wasi_sdk_path() -> PathBuf {
    std::env::var_os("WASI_SDK")
        .map(PathBuf::from)
        .unwrap_or_else(download_wasi_sdk)
}

fn main() {
    #[cfg(feature = "logging")]
    pretty_env_logger::init();

    // Engine dump options (debug output of the C engine): any of them
    // compiles the dump code in (ENABLE_DUMPS), rquickjs then selects the
    // dumps of each runtime with JS_SetDumpFlags().
    let dump_features = [
        "dump-bytecode",
        "dump-gc",
        "dump-gc-free",
        "dump-free",
        "dump-leaks",
        "dump-mem",
        "dump-objects",
        "dump-atoms",
        "dump-shapes",
        "dump-module-resolve",
        "dump-promise",
        "dump-read-object",
    ];

    for feature in dump_features
        .iter()
        .chain(&["bindgen", "update-bindings", "disable-assertions", "python"])
        .copied()
        .chain(capi::features())
    {
        println!("cargo:rerun-if-env-changed={}", feature_to_cargo(feature));
    }
    println!("cargo:rerun-if-env-changed=CARGO_CFG_SANITIZE");
    println!("cargo:rerun-if-env-changed=RQUICKJS_PYLIB_DIR");

    // The engine is the QuickJS tree this crate is vendored in.
    let src_dir = Path::new(&env::var("CARGO_MANIFEST_DIR").unwrap()).join("../..");
    let src_dir = src_dir
        .canonicalize()
        .expect("the QuickJS sources must be two directories above rquickjs-sys");

    let out_dir = env::var("OUT_DIR").expect("No OUT_DIR env var is set by cargo");
    let out_dir = Path::new(&out_dir);

    let header_files = [
        "cutils.h",
        "dtoa.h",
        "libregexp-opcode.h",
        "libregexp.h",
        "libunicode-table.h",
        "libunicode.h",
        "list.h",
        "quickjs-atom.h",
        "quickjs-opcode.h",
        "quickjs.h",
        // included by quickjs.c
        "quickjs-snapshot.c",
        "quickjs-snapshot-data.c",
        "quickjs-snapshot-control.c",
        "VERSION",
    ];

    let source_files = [
        "quickjs.c",
        "libregexp.c",
        "libunicode.c",
        "cutils.c",
        "dtoa.c",
    ];

    for file in source_files.iter().chain(header_files.iter()) {
        println!("cargo:rerun-if-changed={}", src_dir.join(file).display());
    }
    println!("cargo:rerun-if-changed=quickjs.bind.h");
    // quickjs.c includes python/ (python/opcode.h always, the whole runtime
    // with CONFIG_PYTHON)
    println!("cargo:rerun-if-changed={}", src_dir.join("python").display());

    let version = fs::read_to_string(src_dir.join("VERSION")).expect("Unable to read VERSION");
    let version = format!("\"{}\"", version.trim());

    let pylib_define;
    let mut defines: Vec<(String, Option<&str>)> = vec![
        ("_GNU_SOURCE".into(), None),
        ("CONFIG_VERSION".into(), Some(version.as_str())),
    ];

    if dump_features
        .iter()
        .any(|feature| env::var(feature_to_cargo(feature)).is_ok())
    {
        defines.push(("ENABLE_DUMPS".into(), None));
    }

    #[cfg(feature = "disable-assertions")]
    defines.push(("NDEBUG".into(), None));

    // The Python runtime: python/*.c are #included by quickjs.c (as in the
    // engine Makefile, CONFIG_PYTHON=y). The stdlib directory is compiled in
    // (QP_PYLIB_DIR): the launcher default <executable dir>/pylib means
    // nothing for an embedder. RQUICKJS_PYLIB_DIR overrides the vendored
    // `pylib` of the source tree; QPYHOME still overrides both at run time.
    if env::var("CARGO_FEATURE_PYTHON").is_ok() {
        let pylib = env::var_os("RQUICKJS_PYLIB_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|| src_dir.join("pylib"));
        let pylib = pylib.canonicalize().unwrap_or(pylib);
        pylib_define = c_string_literal(&pylib.to_string_lossy());
        defines.push(("CONFIG_PYTHON".into(), None));
        defines.push(("QP_PYLIB_DIR".into(), Some(pylib_define.as_str())));
    }

    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let target_env = env::var("CARGO_CFG_TARGET_ENV").unwrap();

    let mut builder = cc::Build::new();
    builder
        .extra_warnings(false)
        .include(&src_dir)
        // same code generation options as the engine Makefile
        .flag_if_supported("-funsigned-char")
        .flag_if_supported("-fwrapv")
        .flag_if_supported("-Wno-implicit-const-int-float-conversion")
        .flag_if_supported("-Wno-unused-parameter")
        .flag_if_supported("-Wno-sign-compare");

    // sanitizers: the engine and the C modules of the Python C API
    let sanitize = env::var("CARGO_CFG_SANITIZE").ok();
    if let Some(x) = sanitize.as_deref() {
        if !matches!(x, "address" | "memory" | "thread") {
            println!("cargo:warning=Unsupported sanitize_option: '{x}'");
        }
    }
    let configure_sanitizer = |b: &mut cc::Build| {
        if let Some(x @ ("address" | "memory" | "thread")) = sanitize.as_deref() {
            b.flag(format!("-fsanitize={x}"))
                .flag("-fno-sanitize-recover=all")
                .flag("-fno-omit-frame-pointer");
        }
    };
    configure_sanitizer(&mut builder);

    let mut bindgen_cflags = vec![format!("-I{}", src_dir.display())];

    if target_os == "windows" {
        if target_env == "msvc" {
            env::set_var(
                "CFLAGS",
                "/DWIN32_LEAN_AND_MEAN /std:c11 /experimental:c11atomics",
            );
        } else {
            // GNU C: the engine uses `asm`
            env::set_var("CFLAGS", "-DWIN32_LEAN_AND_MEAN -std=gnu11");
        }
    }

    if target_os == "wasi" {
        // pretend we're emscripten - there are already ifdefs that match
        // also, wasi doesn't ahve FE_DOWNWARD or FE_UPWARD
        defines.push(("EMSCRIPTEN".into(), Some("1")));
        defines.push(("FE_DOWNWARD".into(), Some("0")));
        defines.push(("FE_UPWARD".into(), Some("0")));
    }

    if target_os == "wasi" && !matches!(env::var("RQUICKJS_SYS_NO_WASI_SDK").as_deref(), Ok("1")) {
        let wasi_sdk_path = get_wasi_sdk_path();
        if !wasi_sdk_path.try_exists().unwrap() {
            panic!(
                "wasi-sdk not installed in specified path of {}",
                wasi_sdk_path.display()
            );
        }
        env::set_var("CC", wasi_sdk_path.join("bin/clang").to_str().unwrap());
        env::set_var("AR", wasi_sdk_path.join("bin/ar").to_str().unwrap());
        let sysroot = format!(
            "--sysroot={}",
            wasi_sdk_path.join("share/wasi-sysroot").display()
        );
        env::set_var("CFLAGS", &sysroot);
        bindgen_cflags.push(sysroot);
    }

    // The Python C API (python/capi_*.c in quickjs.c) and the CPython C
    // modules of capi/Setup.qp: see build_capi.rs
    if env::var("CARGO_FEATURE_PYTHON").is_ok() && capi::supported() {
        capi::build(
            &src_dir,
            out_dir,
            &mut builder,
            &mut defines,
            &configure_sanitizer,
        );
    }

    // generating bindings
    bindgen(
        out_dir,
        Path::new("quickjs.bind.h"),
        &defines,
        bindgen_cflags,
    );

    for (name, value) in &defines {
        builder.define(name, *value);
    }

    for src in &source_files {
        builder.file(src_dir.join(src));
    }

    builder.compile("libquickjs.a");
}

fn c_string_literal(s: &str) -> String {
    let mut lit = String::with_capacity(s.len() + 2);
    lit.push('"');
    for c in s.chars() {
        if c == '"' || c == '\\' {
            lit.push('\\');
        }
        lit.push(c);
    }
    lit.push('"');
    lit
}

fn feature_to_cargo(name: impl AsRef<str>) -> String {
    format!("CARGO_FEATURE_{}", feature_to_define(name))
}

fn feature_to_define(name: impl AsRef<str>) -> String {
    name.as_ref().to_uppercase().replace('-', "_")
}

#[cfg(not(feature = "bindgen"))]
fn bindgen<'a, D, H, X, K, V>(out_dir: D, _header_file: H, _defines: X, _add_cflags: Vec<String>)
where
    D: AsRef<Path>,
    H: AsRef<Path>,
    X: IntoIterator<Item = &'a (K, Option<V>)>,
    K: AsRef<str> + 'a,
    V: AsRef<str> + 'a,
{
    let target = env::var("TARGET").unwrap();

    if !Path::new("./")
        .join("src")
        .join("bindings")
        .join(format!("{}.rs", target))
        .canonicalize()
        .map(|x| x.exists())
        .unwrap_or(false)
    {
        println!(
            "cargo:warning=rquickjs probably doesn't ship bindings for platform `{}({})`. try the `bindgen` feature instead.",
            target,
            env::var("BUILD_TARGET").unwrap_or("n/a".into())
        );
    }

    let bindings_file = out_dir.as_ref().join("bindings.rs");

    fs::write(
        bindings_file,
        format!(
            r#"macro_rules! bindings_env {{
                ("TARGET") => {{ "{target}" }};
            }}"#
        ),
    )
    .unwrap();
}

#[cfg(feature = "bindgen")]
fn bindgen<'a, D, H, X, K, V>(out_dir: D, header_file: H, defines: X, add_cflags: Vec<String>)
where
    D: AsRef<Path>,
    H: AsRef<Path>,
    X: IntoIterator<Item = &'a (K, Option<V>)>,
    K: AsRef<str> + 'a,
    V: AsRef<str> + 'a,
{
    let out_dir = out_dir.as_ref();
    let header_file = header_file.as_ref();

    let target = env::var("TARGET").unwrap();
    let host = env::var("HOST").unwrap();

    // When cross-compiling with the `macro` feature, sys also gets built for the host.
    // If LIBCLANG_PATH points at the cross toolchain (e.g. Android NDK), that host build
    // generates mismatched bindings, so reuse the bundled binding for the host instead.
    // `update-bindings` still regenerates.
    if target == host && env::var("CARGO_FEATURE_UPDATE_BINDINGS").is_err() {
        let bundled = Path::new("src")
            .join("bindings")
            .join(format!("{}.rs", target));
        if bundled.exists() {
            println!(
                "cargo:warning=using bundled bindings for host target `{}` instead of running bindgen (enable the `update-bindings` feature to regenerate)",
                target
            );
            fs::copy(&bundled, out_dir.join("bindings.rs"))
                .expect("Unable to copy bundled bindings");
            return;
        }
    }

    let mut cflags = add_cflags;

    //format!("-I{}", out_dir.parent().display()),

    for (name, value) in defines {
        cflags.push(if let Some(value) = value {
            format!("-D{}={}", name.as_ref(), value.as_ref())
        } else {
            format!("-D{}", name.as_ref())
        });
    }

    let mut builder = bindgen_rs::Builder::default()
        .use_core()
        .detect_include_paths(true)
        .clang_arg("-xc")
        .clang_arg("-v")
        .clang_args(cflags)
        .size_t_is_usize(false)
        .header(header_file.display().to_string())
        .allowlist_type("JS.*")
        .allowlist_function("js.*")
        .allowlist_function("JS.*")
        .allowlist_function("__JS.*")
        .allowlist_var("JS.*")
        .opaque_type("FILE")
        .blocklist_type("FILE")
        .blocklist_function("JS_DumpMemoryUsage");

    if env::var("CARGO_CFG_TARGET_OS").unwrap() == "wasi" {
        builder = builder.clang_arg("-fvisibility=default");
    }

    let bindings = builder.generate().expect("Unable to generate bindings");

    let bindings_file = out_dir.join("bindings.rs");

    bindings
        .write_to_file(&bindings_file)
        .expect("Couldn't write bindings");

    // Special case to support bundled bindings
    if env::var("CARGO_FEATURE_UPDATE_BINDINGS").is_ok() {
        let dest_dir = Path::new("src").join("bindings");
        fs::create_dir_all(&dest_dir).unwrap();

        let dest_file = format!("{}.rs", env::var("TARGET").unwrap());
        fs::copy(&bindings_file, dest_dir.join(dest_file)).unwrap();
    }
}
