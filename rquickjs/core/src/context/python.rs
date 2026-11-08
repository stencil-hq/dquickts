//! The Python 3.15 runtime of the engine (`JS_InitPython`, `JS_EvalPython`, `JS_PythonImport`).
//!
//! Python and JavaScript share one heap and their values by identity: a Python `list` is an
//! [`Array`](crate::Array), `str` a [`String`](crate::String), `int` a number (int32) or a
//! [`BigInt`](crate::BigInt), `float` a number, `None` is `undefined` and functions are
//! [`Function`](crate::Function)s. Other Python objects are [`Object`]s whose properties are
//! their attributes, and whose integer keys and mapping keys are their items (`dict` keys read
//! as properties). So the results convert with the ordinary [`FromJs`] and
//! [`IntoJs`](crate::IntoJs).

use alloc::{
    ffi::CString,
    string::{String as StdString, ToString},
    vec::Vec,
};
use core::{
    ffi::{c_char, c_int},
    ptr,
};

use crate::{qjs, Ctx, FromJs, Object, Result, Value};

/// The configuration of the Python state of a context, see [`Ctx::init_python`].
///
/// The defaults are those of an embedded interpreter: an empty `sys.argv`, a random hash seed,
/// no optimization and the environment honoured (`QPYHOME`, `PYTHONPATH`...).
#[non_exhaustive]
#[derive(Debug, Clone, Default)]
pub struct PythonConfig {
    /// `sys.argv`.
    pub argv: Vec<StdString>,
    /// `sys.executable` (empty by default).
    pub executable: Option<StdString>,
    /// The Python home (like `QPYHOME`): the standard library is `<home>/pylib`. The default is
    /// `$QPYHOME`, else the `pylib` directory compiled in by `rquickjs-sys` (the `pylib` of the
    /// QuickJS tree, or `RQUICKJS_PYLIB_DIR` at build time).
    pub home: Option<StdString>,
    /// Inserted as `sys.path[0]`.
    pub sys_path_0: Option<StdString>,
    /// A `PYTHONPATH`-style list of directories, searched before the standard library.
    pub python_path: Option<StdString>,
    /// The seed of the `str`/`bytes` hashes (`PYTHONHASHSEED`); `None` is random, `Some(0)`
    /// disables the randomization.
    pub hash_seed: Option<u32>,
    /// The `-O` level (`0`: `__debug__` is true and asserts run).
    pub optimize: u32,
    /// `-I`: ignore the environment variables and the user site directory.
    pub isolated: bool,
    /// `sys.getrecursionlimit()`; `None` is the default (1000).
    pub recursion_limit: Option<u32>,
}

impl PythonConfig {
    /// The default configuration.
    pub fn new() -> Self {
        Self::default()
    }

    /// Sets `sys.argv`.
    pub fn argv<I, S>(mut self, argv: I) -> Self
    where
        I: IntoIterator<Item = S>,
        S: Into<StdString>,
    {
        self.argv = argv.into_iter().map(Into::into).collect();
        self
    }

    /// Sets `sys.executable`.
    pub fn executable(mut self, executable: impl Into<StdString>) -> Self {
        self.executable = Some(executable.into());
        self
    }

    /// Sets the Python home: the standard library is `<home>/pylib`.
    pub fn home(mut self, home: impl Into<StdString>) -> Self {
        self.home = Some(home.into());
        self
    }

    /// Sets `sys.path[0]`.
    pub fn sys_path_0(mut self, path: impl Into<StdString>) -> Self {
        self.sys_path_0 = Some(path.into());
        self
    }

    /// Sets the `PYTHONPATH`-style list of module directories.
    pub fn python_path(mut self, path: impl Into<StdString>) -> Self {
        self.python_path = Some(path.into());
        self
    }

    /// Sets the hash seed (`PYTHONHASHSEED`).
    pub fn hash_seed(mut self, seed: u32) -> Self {
        self.hash_seed = Some(seed);
        self
    }

    /// Sets the `-O` level.
    pub fn optimize(mut self, level: u32) -> Self {
        self.optimize = level;
        self
    }

    /// Sets the isolated mode (`-I`).
    pub fn isolated(mut self, isolated: bool) -> Self {
        self.isolated = isolated;
        self
    }

    /// Sets `sys.getrecursionlimit()`.
    pub fn recursion_limit(mut self, limit: u32) -> Self {
        self.recursion_limit = Some(limit);
        self
    }
}

/// The strings a `JSPythonConfig` points to: `JS_InitPython` does not copy them, so they live
/// as long as the runtime.
pub(crate) struct PythonStrings {
    _strings: Vec<CString>,
    _argv: Vec<*mut c_char>,
}

// The pointers of `_argv` point into `_strings`, which are never mutated.
unsafe impl Send for PythonStrings {}
unsafe impl Sync for PythonStrings {}

/// How [`Ctx::eval_python_with_options`] compiles the source (the modes of Python's
/// `compile()`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum PythonEvalMode {
    /// Statements (`"exec"`); the result is `None` (`undefined`).
    Exec,
    /// One expression (`"eval"`); the result is its value.
    #[default]
    Eval,
    /// One interactive statement (`"single"`); expression values go to `sys.displayhook`.
    Single,
}

/// Python eval options, see [`Ctx::eval_python_with_options`].
#[non_exhaustive]
#[derive(Debug, Clone, Default)]
pub struct PythonEvalOptions {
    /// The compile mode, [`PythonEvalMode::Eval`] by default.
    pub mode: PythonEvalMode,
    /// The filename of tracebacks, `"<string>"` by default.
    pub filename: Option<StdString>,
    /// Allow top-level `await`: the result is then a [`Promise`](crate::Promise) of the value
    /// and the code runs as a task driven by the job queue (see
    /// [`Ctx::execute_pending_job`] and [`Promise::finish`](crate::Promise::finish)).
    pub promise: bool,
}

impl PythonEvalOptions {
    fn to_flag(&self) -> c_int {
        let mut flag = match self.mode {
            PythonEvalMode::Exec => qjs::JS_PY_EVAL_EXEC,
            PythonEvalMode::Eval => qjs::JS_PY_EVAL_EVAL,
            PythonEvalMode::Single => qjs::JS_PY_EVAL_SINGLE,
        };
        if self.promise {
            flag |= qjs::JS_PY_EVAL_FLAG_ASYNC;
        }
        flag as c_int
    }
}

fn opt_cstring(s: &Option<StdString>, strings: &mut Vec<CString>) -> Result<*const c_char> {
    Ok(match s {
        Some(s) => {
            let s = CString::new(s.as_str())?;
            let ptr = s.as_ptr();
            strings.push(s);
            ptr
        }
        None => ptr::null(),
    })
}

impl<'js> Ctx<'js> {
    /// Creates the Python state of the context (builtins, `sys`, importlib, `__main__` and the
    /// `js` module) with the given configuration.
    ///
    /// Python is otherwise initialized with the default configuration by the first
    /// [`Ctx::eval_python`], [`Ctx::python_import`] or `.py` module import, and the configuration
    /// is ignored when Python is already initialized in the context.
    ///
    /// When the runtime has no module loader (`Runtime::set_loader`), a default one which
    /// imports `.py` specifiers as Python modules is installed.
    pub fn init_python(&self, config: PythonConfig) -> Result<()> {
        let mut strings = Vec::with_capacity(config.argv.len() + 6);
        let mut argv = Vec::with_capacity(config.argv.len() + 1);
        for arg in &config.argv {
            let arg = CString::new(arg.as_str())?;
            argv.push(arg.as_ptr().cast_mut());
            strings.push(arg);
        }
        argv.push(ptr::null_mut());
        let hash_seed = config.hash_seed.map(|seed| seed.to_string());

        let cfg = qjs::JSPythonConfig {
            argc: c_int::try_from(config.argv.len()).map_err(|_| crate::Error::Unknown)?,
            argv: argv.as_mut_ptr(),
            executable: opt_cstring(&config.executable, &mut strings)?,
            sys_path_0: opt_cstring(&config.sys_path_0, &mut strings)?,
            pythonpath: opt_cstring(&config.python_path, &mut strings)?,
            hash_seed: opt_cstring(&hash_seed, &mut strings)?,
            optimize: config.optimize.min(c_int::MAX as u32) as c_int,
            isolated: config.isolated as c_int,
            recursion_limit: config.recursion_limit.unwrap_or(0).min(c_int::MAX as u32) as c_int,
            home: opt_cstring(&config.home, &mut strings)?,
        };

        let res = unsafe {
            #[cfg(feature = "parallel")]
            qjs::JS_UpdateStackTop(qjs::JS_GetRuntime(self.as_ptr()));
            let res = qjs::JS_InitPython(self.as_ptr(), &cfg);
            // moving the vectors keeps their heap buffers, which `cfg` points to
            self.get_opaque().keep_python_strings(PythonStrings {
                _strings: strings,
                _argv: argv,
            });
            res
        };
        if res < 0 {
            return Err(self.raise_exception());
        }
        Ok(())
    }

    /// Evaluates a Python expression in the globals of `__main__`.
    ///
    /// A Python exception is returned as [`Error::Exception`](crate::Error::Exception): the
    /// exception object is retrieved with [`Ctx::catch`].
    ///
    /// ```
    /// # use rquickjs::{Context, Runtime};
    /// # let rt = Runtime::new().unwrap();
    /// # let ctx = Context::full(&rt).unwrap();
    /// # ctx.with(|ctx| {
    /// let squares: Vec<i32> = ctx.eval_python("[x * x for x in range(4)]").unwrap();
    /// assert_eq!(squares, [0, 1, 4, 9]);
    /// # });
    /// ```
    pub fn eval_python<V: FromJs<'js>, S: AsRef<[u8]>>(&self, source: S) -> Result<V> {
        self.eval_python_with_options(source, Default::default())
    }

    /// Evaluates Python source with the given options (statements, top-level `await`...).
    pub fn eval_python_with_options<V: FromJs<'js>, S: AsRef<[u8]>>(
        &self,
        source: S,
        options: PythonEvalOptions,
    ) -> Result<V> {
        let source = source.as_ref();
        let filename = options
            .filename
            .as_deref()
            .map(CString::new)
            .transpose()?;

        V::from_js(self, unsafe {
            #[cfg(feature = "parallel")]
            qjs::JS_UpdateStackTop(qjs::JS_GetRuntime(self.as_ptr()));
            let val = qjs::JS_EvalPython(
                self.as_ptr(),
                source.as_ptr().cast::<c_char>(),
                source.len() as _,
                filename.as_ref().map_or(ptr::null(), |f| f.as_ptr()),
                options.to_flag(),
            );
            let val = self.handle_exception(val)?;
            Value::from_js_value(self.clone(), val)
        })
    }

    /// Imports a Python module (`importlib.import_module(name)`); its attributes are the
    /// properties of the returned object.
    pub fn python_import(&self, name: &str) -> Result<Object<'js>> {
        let name = CString::new(name)?;
        let module = unsafe {
            #[cfg(feature = "parallel")]
            qjs::JS_UpdateStackTop(qjs::JS_GetRuntime(self.as_ptr()));
            let val = qjs::JS_PythonImport(self.as_ptr(), name.as_ptr());
            let val = self.handle_exception(val)?;
            Value::from_js_value(self.clone(), val)
        };
        Object::from_js(self, module)
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::{Array, BigInt, Context, Error, Function, Promise, Runtime};
    use alloc::vec;

    /// CPython's C extension modules bind to one runtime at a time, and the
    /// test harness runs tests on parallel threads, so Python tests take turns.
    static PYTHON_RUNTIME: std::sync::Mutex<()> = std::sync::Mutex::new(());

    fn with_ctx(f: impl FnOnce(Ctx)) {
        let _turn = PYTHON_RUNTIME
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let rt = Runtime::new().unwrap();
        let ctx = Context::full(&rt).unwrap();
        ctx.with(f);
    }

    fn exec(ctx: &Ctx, source: &str) {
        let options = PythonEvalOptions {
            mode: PythonEvalMode::Exec,
            ..Default::default()
        };
        ctx.eval_python_with_options::<(), _>(source, options)
            .unwrap();
    }

    #[test]
    fn eval_values() {
        with_ctx(|ctx| {
            assert_eq!(ctx.eval_python::<i32, _>("6 * 7").unwrap(), 42);
            assert_eq!(ctx.eval_python::<f64, _>("1.5 * 2").unwrap(), 3.0);
            assert_eq!(
                ctx.eval_python::<StdString, _>("'py' + 'thon'").unwrap(),
                "python"
            );
            assert!(ctx.eval_python::<Value, _>("None").unwrap().is_undefined());
            assert!(ctx.eval_python::<bool, _>("3 > 2").unwrap());

            // int beyond int32: a BigInt
            let big: BigInt = ctx.eval_python("2 ** 40").unwrap();
            assert_eq!(big.to_i64().unwrap(), 1 << 40);

            // list = Array
            let list: Array = ctx.eval_python("[x * x for x in range(4)]").unwrap();
            assert_eq!(list.len(), 4);
            let squares: Vec<i32> = list.into_value().get().unwrap();
            assert_eq!(squares, [0, 1, 4, 9]);

            // dict: an object whose properties are the items
            let dict: Object = ctx
                .eval_python("{'a': 1, 'b': [1, 2], 'c': {'d': 'e'}}")
                .unwrap();
            assert_eq!(dict.get::<_, i32>("a").unwrap(), 1);
            assert_eq!(dict.get::<_, Vec<i32>>("b").unwrap(), [1, 2]);
            let inner: Object = dict.get("c").unwrap();
            assert_eq!(inner.get::<_, StdString>("d").unwrap(), "e");
            let js: Function = ctx.eval("(o) => o.a + o.b.length").unwrap();
            assert_eq!(js.call::<_, i32>((dict,)).unwrap(), 3);
        })
    }

    #[test]
    fn exec_and_call_functions() {
        with_ctx(|ctx| {
            exec(&ctx, "def add(a, b=10):\n    return a + b\ncounter = 0\n");
            let add: Function = ctx.eval_python("add").unwrap();
            assert_eq!(add.call::<_, i32>((1, 2)).unwrap(), 3);
            assert_eq!(add.call::<_, i32>((1,)).unwrap(), 11);
            assert_eq!(
                add.call::<_, StdString>(("a", "b")).unwrap(),
                "ab"
            );

            // Rust functions called by Python
            let triple = Function::new(ctx.clone(), |x: i32| x * 3).unwrap();
            let apply: Function = ctx.eval_python("lambda f, x: f(x)").unwrap();
            assert_eq!(apply.call::<_, i32>((triple, 14)).unwrap(), 42);

            // the globals of __main__ persist between evaluations
            exec(&ctx, "counter += 5");
            assert_eq!(ctx.eval_python::<i32, _>("counter").unwrap(), 5);
            let main = ctx.python_import("__main__").unwrap();
            assert_eq!(main.get::<_, i32>("counter").unwrap(), 5);
        })
    }

    #[test]
    fn exceptions() {
        with_ctx(|ctx| {
            let type_name: Function = ctx.eval_python("lambda e: type(e).__name__").unwrap();

            let err = ctx.eval_python::<i32, _>("1 / 0").unwrap_err();
            assert!(matches!(err, Error::Exception), "{err:?}");
            let exc = ctx.catch();
            assert!(exc.is_object());
            assert_eq!(
                type_name.call::<_, StdString>((exc.clone(),)).unwrap(),
                "ZeroDivisionError"
            );
            // the exception object is a Python object: str() through toString
            let to_string: Function = ctx.eval("(e) => String(e)").unwrap();
            assert_eq!(
                to_string.call::<_, StdString>((exc,)).unwrap(),
                "division by zero"
            );

            // raised by a Python function called from Rust
            exec(&ctx, "def fail(msg):\n    raise ValueError(msg)\n");
            let fail: Function = ctx.eval_python("fail").unwrap();
            let err = fail.call::<_, ()>(("bad",)).unwrap_err();
            assert!(matches!(err, Error::Exception), "{err:?}");
            let exc = ctx.catch();
            assert_eq!(
                type_name.call::<_, StdString>((exc.clone(),)).unwrap(),
                "ValueError"
            );
            // its attributes are properties, the items of the args tuple integer keys
            let args: Object = exc.as_object().unwrap().get("args").unwrap();
            assert_eq!(args.get::<_, StdString>(0).unwrap(), "bad");

            // a syntax error names the given file
            let options = PythonEvalOptions {
                filename: Some("bad.py".into()),
                ..Default::default()
            };
            let err = ctx
                .eval_python_with_options::<(), _>("1 +", options)
                .unwrap_err();
            assert!(matches!(err, Error::Exception), "{err:?}");
            let exc = ctx.catch();
            assert_eq!(
                type_name.call::<_, StdString>((exc.clone(),)).unwrap(),
                "SyntaxError"
            );
            let filename: StdString = exc.as_object().unwrap().get("filename").unwrap();
            assert_eq!(filename, "bad.py");

            // the context stays usable
            assert!(!ctx.has_exception());
            assert_eq!(ctx.eval_python::<i32, _>("1 + 1").unwrap(), 2);
        })
    }

    #[test]
    fn top_level_await() {
        with_ctx(|ctx| {
            exec(
                &ctx,
                "async def double(x):\n    return x * 2\n\
                 def set_global(name, value):\n    globals()[name] = value\n",
            );
            let options = PythonEvalOptions {
                promise: true,
                ..Default::default()
            };

            // a Python coroutine run as a task by the job queue
            let promise: Promise = ctx
                .eval_python_with_options("await double(21)", options.clone())
                .unwrap();
            assert_eq!(promise.finish::<i32>().unwrap(), 42);

            // awaiting a JS promise settled from Rust
            let (js_promise, resolve, _reject) = ctx.promise().unwrap();
            let set_global: Function = ctx.eval_python("set_global").unwrap();
            set_global.call::<_, ()>(("p", js_promise)).unwrap();
            let promise: Promise = ctx
                .eval_python_with_options("(await p) + 1", options.clone())
                .unwrap();
            assert!(matches!(promise.finish::<i32>(), Err(Error::WouldBlock)));
            resolve.call::<_, ()>((41,)).unwrap();
            assert_eq!(promise.finish::<i32>().unwrap(), 42);

            // statements with await: a promise of None
            let exec_async = PythonEvalOptions {
                mode: PythonEvalMode::Exec,
                ..options.clone()
            };
            let promise: Promise = ctx
                .eval_python_with_options("result = await double(5)", exec_async)
                .unwrap();
            assert!(promise.finish::<Value>().unwrap().is_undefined());
            assert_eq!(ctx.eval_python::<i32, _>("result").unwrap(), 10);

            // without await: an already resolved promise
            let promise: Promise = ctx.eval_python_with_options("7", options).unwrap();
            assert_eq!(promise.finish::<i32>().unwrap(), 7);

            // a rejection carries the Python exception
            let options = PythonEvalOptions {
                promise: true,
                ..Default::default()
            };
            let promise: Promise = ctx
                .eval_python_with_options("await double(None)", options)
                .unwrap();
            let err = promise.finish::<i32>().unwrap_err();
            assert!(matches!(err, Error::Exception), "{err:?}");
            let exc = ctx.catch();
            let type_name: Function = ctx.eval_python("lambda e: type(e).__name__").unwrap();
            assert_eq!(
                type_name.call::<_, StdString>((exc,)).unwrap(),
                "TypeError"
            );
        })
    }

    #[test]
    fn import_json() {
        with_ctx(|ctx| {
            let json = ctx.python_import("json").unwrap();
            let dumps: Function = json.get("dumps").unwrap();
            assert_eq!(
                dumps.call::<_, StdString>((vec![1, 2, 3],)).unwrap(),
                "[1, 2, 3]"
            );
            let loads: Function = json.get("loads").unwrap();
            let value: Object = loads.call((r#"{"a": [1, 2], "b": "x"}"#,)).unwrap();
            assert_eq!(value.get::<_, Vec<i32>>("a").unwrap(), [1, 2]);
            assert_eq!(value.get::<_, StdString>("b").unwrap(), "x");

            // a missing module raises ModuleNotFoundError
            let err = ctx.python_import("no_such_module_here").unwrap_err();
            assert!(matches!(err, Error::Exception), "{err:?}");
            let exc = ctx.catch();
            let type_name: Function = ctx.eval_python("lambda e: type(e).__name__").unwrap();
            assert_eq!(
                type_name.call::<_, StdString>((exc,)).unwrap(),
                "ModuleNotFoundError"
            );
        })
    }

    #[test]
    fn builtin_c_modules() {
        // the self-contained CPython C modules are part of the engine
        // (capi/Setup.qp): every embedder can import them
        with_ctx(|ctx| {
            exec(
                &ctx,
                "import sys, re, itertools, decimal, hashlib, math\n\
                 C_MODULES = ' '.join(map(str, [\n\
                     '_sre' in sys.builtin_module_names,\n\
                     re.sub(r'\\d+', '#', 'a1b22'),\n\
                     list(itertools.accumulate([1, 2, 3])),\n\
                     decimal.Decimal(1) / 8,\n\
                     hashlib.sha256(b'x').hexdigest()[:8],\n\
                     math.comb(5, 2)]))\n",
            );
            let res: StdString = ctx.eval_python("C_MODULES").unwrap();
            assert_eq!(res, "True a#b# [1, 3, 6] 0.125 2d711642 10");
        })
    }

    #[test]
    #[cfg(feature = "std")]
    fn init_config() {
        let home = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .and_then(|p| p.parent())
            .unwrap()
            .to_str()
            .unwrap()
            .to_string();
        let hash = |seed: u32| {
            let _turn = PYTHON_RUNTIME
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            let rt = Runtime::new().unwrap();
            let ctx = Context::full(&rt).unwrap();
            ctx.with(|ctx| {
                ctx.init_python(
                    PythonConfig::new()
                        .argv(["prog", "-x"])
                        .home(home.clone())
                        .hash_seed(seed)
                        .recursion_limit(500),
                )
                .unwrap();
                // initialized: a second configuration is ignored
                ctx.init_python(PythonConfig::new().argv(["other"])).unwrap();
                let argv: Vec<StdString> = ctx.eval_python("__import__('sys').argv").unwrap();
                assert_eq!(argv, ["prog", "-x"]);
                let prefix: StdString = ctx.eval_python("__import__('sys').prefix").unwrap();
                assert_eq!(prefix, home);
                let limit: i32 = ctx
                    .eval_python("__import__('sys').getrecursionlimit()")
                    .unwrap();
                assert_eq!(limit, 500);
                ctx.eval_python::<StdString, _>("str(hash('rquickjs'))")
                    .unwrap()
            })
        };
        assert_eq!(hash(42), hash(42));
        assert_ne!(hash(42), hash(43));
    }
}
