//! The Python C API and the CPython C modules of the engine (`python`
//! feature on 64-bit Unix targets): the counterpart of the engine
//! Makefile's CONFIG_PYTHON_CAPI section.
//!
//! capi/Setup.qp lists the modules (its header documents the format; the
//! Makefile reads it with capi/setup.awk).  The self-contained modules are
//! always built; a module that links a system library (an `*external*`
//! condition) is built when its cargo feature is on:
//!
//! | feature           | condition | library                         |
//! |-------------------|-----------|---------------------------------|
//! | `python-zlib`     | zlib      | zlib                            |
//! | `python-bz2`      | bz2       | bzip2                           |
//! | `python-lzma`     | lzma      | liblzma                         |
//! | `python-sqlite3`  | sqlite3   | sqlite3                         |
//! | `python-ssl`      | ssl       | OpenSSL (_ssl, _hashlib)        |
//! | `python-zstd`     | zstd      | libzstd                         |
//! | `python-ctypes`   | ffi       | libffi                          |
//! | `python-readline` | readline  | GNU readline, else libedit      |
//! | `python-uuid`     | libuuid   | libuuid (not macOS: libSystem)  |
//!
//! On macOS `_scproxy` (urllib.request) is always built, with the
//! SystemConfiguration and CoreFoundation frameworks.  Libraries are found
//! with pkg-config when available, else linked by their usual name.
//!
//! rquickjs is its own launcher: every built module goes into the one
//! inittab the runtime registers (`_PyQp_CoreInittab`).

use std::{
    collections::{BTreeMap, BTreeSet},
    env, fs,
    path::{Path, PathBuf},
    process::Command,
};

/// (cargo feature, Setup.qp condition, pkg-config packages tried in order
/// with their extra cflags, fallback link libraries)
const EXTERNAL: &[(&str, &str, &[(&str, &str)], &[&str])] = &[
    ("python-zlib", "zlib", &[("zlib", "")], &["z"]),
    ("python-bz2", "bz2", &[("bzip2", "")], &["bz2"]),
    ("python-lzma", "lzma", &[("liblzma", "")], &["lzma"]),
    ("python-sqlite3", "sqlite3", &[("sqlite3", "")], &["sqlite3"]),
    ("python-ssl", "ssl", &[("openssl", "")], &["ssl", "crypto"]),
    ("python-zstd", "zstd", &[("libzstd", "")], &["zstd"]),
    ("python-ctypes", "ffi", &[("libffi", "")], &["ffi"]),
    (
        "python-readline",
        "readline",
        &[("readline", ""), ("libedit", "-DWITH_EDITLINE")],
        &["readline"],
    ),
    ("python-uuid", "libuuid", &[("uuid", "")], &["uuid"]),
];

/// Cargo features of the external modules (for rerun-if-env-changed).
pub fn features() -> impl Iterator<Item = &'static str> {
    EXTERNAL.iter().map(|e| e.0)
}

/// Whether the target supports the C API (as the Makefile: 64-bit, POSIX).
pub fn supported() -> bool {
    env::var("CARGO_CFG_TARGET_FAMILY").as_deref() == Ok("unix")
        && env::var("CARGO_CFG_TARGET_POINTER_WIDTH").as_deref() == Ok("64")
        && env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("wasi")
}

struct Line {
    lineno: usize,
    /// module name, or None for a `+` line
    name: Option<String>,
    conds: Vec<String>,
    sources: Vec<String>,
    flags: Vec<String>,
}

struct Setup {
    vars: BTreeMap<String, String>,
    external: BTreeSet<String>,
    lines: Vec<Line>,
}

fn parse_setup(path: &Path) -> Setup {
    let text = fs::read_to_string(path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", path.display()));
    let mut setup = Setup {
        vars: BTreeMap::new(),
        external: BTreeSet::new(),
        lines: Vec::new(),
    };
    for (i, raw) in text.lines().enumerate() {
        let lineno = i + 1;
        let line = raw.split('#').next().unwrap();
        let toks: Vec<&str> = line.split_whitespace().collect();
        if toks.is_empty() {
            continue;
        }
        if toks[0] == "*external*" {
            setup.external.extend(toks[1..].iter().map(|s| s.to_string()));
            continue;
        }
        if toks.len() >= 2 && toks[1] == "=" {
            setup
                .vars
                .insert(toks[0].to_string(), toks[2..].join(" "));
            continue;
        }
        let name = if toks[0] == "+" {
            None
        } else {
            Some(toks[0].to_string())
        };
        let mut l = Line {
            lineno,
            name,
            conds: Vec::new(),
            sources: Vec::new(),
            flags: Vec::new(),
        };
        for t in &toks[1..] {
            if let Some(c) = t.strip_prefix('@') {
                l.conds.push(c.to_string());
            } else if t.ends_with(".c") {
                l.sources.push(t.to_string());
            } else {
                l.flags.push(t.to_string());
            }
        }
        setup.lines.push(l);
    }
    setup
}

/// pkg-config output split into words, or None when the package is missing.
fn pkg_config(args: &[&str], pkg: &str) -> Option<Vec<String>> {
    let pkg_config = env::var("PKG_CONFIG").unwrap_or_else(|_| "pkg-config".into());
    let out = Command::new(pkg_config).args(args).arg(pkg).output().ok()?;
    if !out.status.success() {
        return None;
    }
    Some(
        String::from_utf8_lossy(&out.stdout)
            .split_whitespace()
            .map(str::to_string)
            .collect(),
    )
}

fn emit_link_flags(words: &[String]) {
    let mut it = words.iter();
    while let Some(w) = it.next() {
        if let Some(dir) = w.strip_prefix("-L") {
            println!("cargo:rustc-link-search=native={dir}");
        } else if let Some(lib) = w.strip_prefix("-l") {
            println!("cargo:rustc-link-lib={lib}");
        } else if w == "-framework" {
            if let Some(fw) = it.next() {
                println!("cargo:rustc-link-lib=framework={fw}");
            }
        } else {
            println!("cargo:rustc-link-arg={w}");
        }
    }
}

/// Configure `quickjs` (the builder of quickjs.c) for the C API and compile
/// the C modules into it.  `defines` gets CONFIG_PYTHON_CAPI.
pub fn build(
    src_dir: &Path,
    out_dir: &Path,
    quickjs: &mut cc::Build,
    defines: &mut Vec<(String, Option<&str>)>,
    configure: &dyn Fn(&mut cc::Build),
) {
    let capi_dir = src_dir.join("capi");
    println!("cargo:rerun-if-changed={}", capi_dir.display());
    println!("cargo:rerun-if-env-changed=PKG_CONFIG");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");
    defines.push(("CONFIG_PYTHON_CAPI".into(), None));

    let setup = parse_setup(&capi_dir.join("Setup.qp"));

    // enabled conditions and the flags of their libraries
    let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap();
    let mut conds: BTreeSet<String> = ["expat".to_string()].into();
    let mut vars = setup.vars.clone();
    if env::var("CARGO_CFG_TARGET_ARCH").as_deref() == Ok("x86_64") {
        conds.insert("x86_64".into());
    }
    if target_os == "macos" {
        conds.insert("darwin".into());
        conds.insert("scproxy".into());
        emit_link_flags(&[
            "-framework".into(),
            "SystemConfiguration".into(),
            "-framework".into(),
            "CoreFoundation".into(),
        ]);
    }
    let mut link: Vec<String> = Vec::new();
    for &(feature, cond, pkgs, fallback) in EXTERNAL {
        let var = format!("CARGO_FEATURE_{}", feature.to_uppercase().replace('-', "_"));
        if env::var(var).is_err() {
            continue;
        }
        // libuuid: libSystem provides uuid on macOS (`_uuid @darwin`)
        if cond == "libuuid" && target_os == "macos" {
            continue;
        }
        conds.insert(cond.into());
        let found = pkgs.iter().find_map(|&(pkg, extra)| {
            let cflags = pkg_config(&["--cflags"], pkg)?;
            let libs = pkg_config(&["--libs"], pkg)?;
            Some((extra, cflags, libs))
        });
        let (cflags, libs) = match found {
            Some((extra, mut cflags, libs)) => {
                if !extra.is_empty() {
                    cflags.insert(0, extra.to_string());
                }
                (cflags.join(" "), libs)
            }
            None => (
                String::new(),
                fallback.iter().map(|l| format!("-l{l}")).collect(),
            ),
        };
        vars.insert(format!("CAPI_{}_CFLAGS", cond.to_uppercase()), cflags);
        link.extend(libs);
    }

    // the enabled modules and their objects, grouped by flags
    let mut modules: Vec<String> = Vec::new();
    let mut groups: BTreeMap<Vec<String>, Vec<PathBuf>> = BTreeMap::new();
    let mut seen: BTreeSet<String> = BTreeSet::new();
    let mut skip_module = false;
    let mut cur: Option<String> = None;
    for l in &setup.lines {
        let keep = l.conds.iter().all(|c| conds.contains(c));
        match &l.name {
            Some(name) => {
                cur = Some(name.clone());
                skip_module = !keep;
                if keep {
                    if modules.contains(name) {
                        panic!("capi/Setup.qp:{}: module {name} enabled twice", l.lineno);
                    }
                    modules.push(name.clone());
                }
            }
            None if cur.is_none() => {
                panic!("capi/Setup.qp:{}: '+' before any module", l.lineno)
            }
            None => {}
        }
        if !keep || skip_module {
            continue;
        }
        let flags = expand_flags(&l.flags, &vars, src_dir);
        for src in &l.sources {
            if seen.insert(src.clone()) {
                groups
                    .entry(flags.clone())
                    .or_default()
                    .push(capi_dir.join("modules").join(src));
            }
        }
    }

    // the inittab of every built module
    let mut inittab = String::from(
        "/* Generated from capi/Setup.qp by rquickjs-sys/build.rs. */\n\
         #include \"Python.h\"\n#include \"qpcapi.h\"\n\n",
    );
    for m in &modules {
        inittab += &format!("PyMODINIT_FUNC PyInit_{m}(void);\n");
    }
    inittab += "\nstruct _inittab _PyQp_CoreInittab[] = {\n";
    for m in &modules {
        inittab += &format!("    {{\"{m}\", PyInit_{m}}},\n");
    }
    inittab += "    {NULL, NULL}\n};\n";
    let inittab_path = out_dir.join("capi_core_inittab.c");
    fs::write(&inittab_path, inittab).expect("cannot write capi_core_inittab.c");
    groups
        .entry(vec![format!("-I{}", capi_dir.display())])
        .or_default()
        .push(inittab_path);
    groups
        .entry(Vec::new())
        .or_default()
        .push(capi_dir.join("compat/qp_internals.c"));

    // CPython's module build flags (the Makefile's CAPI_CFLAGS)
    for (i, (flags, files)) in groups.iter().enumerate() {
        let mut b = cc::Build::new();
        configure(&mut b);
        b.warnings(false)
            .extra_warnings(false)
            .std("c11")
            .flag("-fno-strict-overflow")
            .flag("-fvisibility=hidden")
            .define("NDEBUG", None)
            .define("Py_BUILD_CORE_MODULE", None)
            .include(capi_dir.join("include"))
            .include(capi_dir.join("compat"))
            .include(capi_dir.join("modules"));
        for f in flags {
            b.flag(f);
        }
        b.files(files);
        // distinct object directories: equal file names in several groups
        b.out_dir(out_dir.join(format!("capi{i}")));
        for obj in b.compile_intermediates() {
            quickjs.object(obj);
        }
    }

    emit_link_flags(&link);
}

/// Expand $(VARS) and make the -I paths of Setup.qp absolute.
fn expand_flags(flags: &[String], vars: &BTreeMap<String, String>, src_dir: &Path) -> Vec<String> {
    let mut out = Vec::new();
    for f in flags {
        let words: Vec<String> = match f.strip_prefix("$(").and_then(|v| v.strip_suffix(')')) {
            Some(var) => vars
                .get(var)
                .map(|v| v.split_whitespace().map(str::to_string).collect())
                .unwrap_or_default(),
            None => vec![f.clone()],
        };
        for w in words {
            match w.strip_prefix("-I") {
                Some(dir) if !Path::new(dir).is_absolute() => {
                    out.push(format!("-I{}", src_dir.join(dir).display()))
                }
                _ => out.push(w),
            }
        }
    }
    out
}
