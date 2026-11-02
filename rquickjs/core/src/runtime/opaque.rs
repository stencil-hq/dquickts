use crate::{
    class::{self, JsClass},
    qjs, Atom, Ctx, Error, JsLifetime, Object, Value,
};

use super::{
    exotic::ExoticMethodsHolder,
    userdata::{UserDataGuard, UserDataMap},
    InterruptHandler, PromiseHook, PromiseHookType, RejectionTracker, UserDataError,
};
use super::{Decorators, Suspender};
use crate::snapshot::{BuiltinRequest, SnapshotHost};
use alloc::{boxed::Box, sync::Arc, vec::Vec};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use core::{
    any::{Any, TypeId},
    cell::{Cell, UnsafeCell},
    hash::{BuildHasherDefault, Hasher},
    marker::PhantomData,
    ptr,
};

#[cfg(feature = "std")]
use std::collections::HashMap;

#[cfg(not(feature = "std"))]
use hashbrown::HashMap;

#[cfg(feature = "futures")]
use super::{schedular::SchedularPoll, spawner::Spawner};

#[cfg(feature = "futures")]
use core::{
    future::Future,
    task::{Context, Waker},
};

/// The class ids of the Rust classes.
///
/// Class ids are process wide in QuickJS: they are allocated once, by the first runtime, and every
/// runtime registers its classes under the same ids. So they are read without looking up the
/// runtime. `JS_NewClassID()` writes an id under a lock before any runtime registers it, which
/// orders the write before every read.
static CLASS_ID: AtomicU32 = AtomicU32::new(0);
static CALLABLE_CLASS_ID: AtomicU32 = AtomicU32::new(0);
static EXOTIC_CLASS_ID: AtomicU32 = AtomicU32::new(0);

/// Hasher of the [`TypeId`] keys of the class prototypes. A [`TypeId`] is already a hash, which
/// its `Hash` implementation writes as an `u64`, so it is used as is instead of being hashed
/// again with SipHash.
#[derive(Default)]
struct TypeIdHasher(u64);

impl Hasher for TypeIdHasher {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, bytes: &[u8]) {
        // not used by the `TypeId` of the current Rust versions, still correct for others
        for &byte in bytes {
            self.0 = self.0.rotate_left(8) ^ u64::from(byte);
        }
    }

    fn write_u64(&mut self, i: u64) {
        self.0 = self.0.rotate_left(32) ^ i;
    }
}

type PrototypeMap<'js> = HashMap<TypeId, Option<Object<'js>>, BuildHasherDefault<TypeIdHasher>>;

/// Hasher of the addresses of static data.
#[derive(Default)]
struct AddrHasher(u64);

impl Hasher for AddrHasher {
    fn finish(&self) -> u64 {
        self.0
    }

    fn write(&mut self, bytes: &[u8]) {
        for &byte in bytes {
            self.write_u64(u64::from(byte));
        }
    }

    fn write_u64(&mut self, i: u64) {
        // spreads the address bits to the high bits used by the hash table
        self.0 = (self.0.rotate_left(5) ^ i).wrapping_mul(0x9e37_79b9_7f4a_7c15);
    }

    fn write_usize(&mut self, i: usize) {
        self.write_u64(i as u64);
    }
}

/// The atoms of static property names, by the address and length of the names.
type StaticAtomMap = HashMap<(usize, usize), Box<[qjs::JSAtom]>, BuildHasherDefault<AddrHasher>>;

/// Opaque book keeping data for Rust.
pub(crate) struct Opaque<'js> {
    /// Used to carry a panic if a callback triggered one.
    panic: Cell<Option<Box<dyn Any + Send + 'static>>>,

    /// The user provided promise hook, if any.
    promise_hook: UnsafeCell<Option<PromiseHook>>,

    /// The user provided rejection tracker, if any.
    rejection_tracker: UnsafeCell<Option<RejectionTracker>>,

    /// The user provided interrupt handler, if any.
    interrupt_handler: UnsafeCell<Option<InterruptHandler>>,

    /// Set by [`Suspender::request`], polled by the interrupt handler trampoline.
    suspend_request: Arc<AtomicBool>,

    /// The natives of the [`SnapshotHost`] of this runtime, built on first use or restored from
    /// a snapshot.
    snapshot_natives: UnsafeCell<Option<(Object<'js>, SnapshotHost)>>,

    /// The request of the snapshot builtin whose call is suspending the execution.
    builtin_request: UnsafeCell<Option<BuiltinRequest>>,

    /// The value the next [`Ctx::resume`](crate::Ctx::resume) gives to the suspended call
    /// (see [`Restore::apply`](crate::snapshot::Restore::apply)).
    resume_value: UnsafeCell<Option<Value<'js>>>,

    /// How TypeScript sources compile decorators.
    decorators: Cell<Decorators>,

    /// The prototypes of the Rust classes, by the [`TypeId`] of the class.
    prototypes: UnsafeCell<PrototypeMap<'js>>,

    /// The atoms of the static property names of
    /// [`StaticObjectBuilder`](crate::object::StaticObjectBuilder), freed with the runtime.
    static_atoms: UnsafeCell<StaticAtomMap>,

    userdata: UserDataMap,

    /// The strings of the [`PythonConfig`](crate::context::PythonConfig)s given to
    /// `JS_InitPython`, which keeps pointers to them (sys.argv, the stdlib home...) for the
    /// lifetime of the context: kept until the runtime is freed.
    #[cfg(feature = "python")]
    python_strings: UnsafeCell<Vec<crate::context::PythonStrings>>,

    #[cfg(feature = "futures")]
    spawner: Option<UnsafeCell<Spawner>>,

    exotic_methods: ExoticMethodsHolder,

    _marker: PhantomData<&'js ()>,
}

impl<'js> Opaque<'js> {
    pub fn new() -> Self {
        Opaque {
            panic: Cell::new(None),

            promise_hook: UnsafeCell::new(None),

            rejection_tracker: UnsafeCell::new(None),

            interrupt_handler: UnsafeCell::new(None),

            suspend_request: Arc::new(AtomicBool::new(false)),

            snapshot_natives: UnsafeCell::new(None),

            builtin_request: UnsafeCell::new(None),

            resume_value: UnsafeCell::new(None),

            decorators: Cell::new(Decorators::Standard),

            prototypes: UnsafeCell::new(PrototypeMap::default()),

            static_atoms: UnsafeCell::new(StaticAtomMap::default()),

            userdata: UserDataMap::default(),

            #[cfg(feature = "python")]
            python_strings: UnsafeCell::new(Vec::new()),

            _marker: PhantomData,

            #[cfg(feature = "futures")]
            spawner: None,

            exotic_methods: ExoticMethodsHolder::new(),
        }
    }

    #[cfg(feature = "futures")]
    pub fn with_spawner() -> Self {
        let mut this = Opaque::new();
        this.spawner = Some(UnsafeCell::new(Spawner::new()));
        this
    }

    pub unsafe fn initialize(&mut self, rt: *mut qjs::JSRuntime) -> Result<(), Error> {
        // Class ids are process wide in QuickJS: allocate them once so that
        // every runtime uses the same ids (JS_NewClassID() is serialized and
        // keeps an already allocated id).
        let class_id = qjs::JS_NewClassID(CLASS_ID.as_ptr());
        let callable_class_id = qjs::JS_NewClassID(CALLABLE_CLASS_ID.as_ptr());
        let exotic_class_id = qjs::JS_NewClassID(EXOTIC_CLASS_ID.as_ptr());

        let class_def = qjs::JSClassDef {
            class_name: c"RustClass".as_ptr().cast(),
            finalizer: Some(class::ffi::class_finalizer),
            gc_mark: Some(class::ffi::class_trace),
            call: None,
            exotic: ptr::null_mut(),
        };

        if 0 != qjs::JS_NewClass(rt, class_id, &class_def) {
            return Err(Error::Unknown);
        }

        let class_def = qjs::JSClassDef {
            class_name: c"RustFunction".as_ptr().cast(),
            finalizer: Some(class::ffi::callable_finalizer),
            gc_mark: Some(class::ffi::callable_trace),
            call: Some(class::ffi::call),
            exotic: ptr::null_mut(),
        };

        if 0 != qjs::JS_NewClass(rt, callable_class_id, &class_def) {
            return Err(Error::Unknown);
        }

        let class_def = qjs::JSClassDef {
            class_name: c"RustExotic".as_ptr().cast(),
            finalizer: Some(class::ffi::exotic_class_finalizer),
            gc_mark: Some(class::ffi::exotic_class_trace),
            call: None,
            exotic: self.exotic_methods.as_ptr(),
        };

        if 0 != qjs::JS_NewClass(rt, exotic_class_id, &class_def) {
            return Err(Error::Unknown);
        }

        Ok(())
    }

    pub unsafe fn from_runtime_ptr<'a>(rt: *mut qjs::JSRuntime) -> &'a Self {
        &*(qjs::JS_GetRuntimeOpaque(rt).cast::<Self>())
    }

    #[cfg(feature = "futures")]
    fn spawner(&self) -> &UnsafeCell<Spawner> {
        self.spawner
            .as_ref()
            .expect("tried to use async function in non async runtime")
    }

    #[cfg(feature = "futures")]
    pub unsafe fn push<F>(&self, f: F)
    where
        F: Future<Output = ()>,
    {
        (*self.spawner().get()).push(f)
    }

    #[cfg(feature = "futures")]
    pub fn listen(&self, wake: Waker) {
        unsafe { (*self.spawner().get()).listen(wake) };
    }

    #[cfg(feature = "futures")]
    pub fn spawner_is_empty(&self) -> bool {
        unsafe { (*self.spawner().get()).is_empty() }
    }

    #[cfg(feature = "futures")]
    pub fn poll(&self, cx: &mut Context) -> SchedularPoll {
        unsafe { (*self.spawner().get()).poll(cx) }
    }

    pub fn insert_userdata<U>(&self, data: U) -> Result<Option<Box<U>>, UserDataError<U>>
    where
        U: JsLifetime<'js>,
        U::Changed<'static>: Any,
    {
        self.userdata.insert(data)
    }

    pub fn remove_userdata<U>(&self) -> Result<Option<Box<U>>, UserDataError<()>>
    where
        U: JsLifetime<'js>,
        U::Changed<'static>: Any,
    {
        self.userdata.remove()
    }

    /// Keeps `strings` alive until the runtime is freed (see `python_strings`).
    #[cfg(feature = "python")]
    pub fn keep_python_strings(&self, strings: crate::context::PythonStrings) {
        // only called with the runtime lock held, never reentrantly
        unsafe { (*self.python_strings.get()).push(strings) }
    }

    pub fn get_userdata<U>(&self) -> Option<UserDataGuard<U>>
    where
        U: JsLifetime<'js>,
        U::Changed<'static>: Any,
    {
        self.userdata.get()
    }

    pub fn set_promise_hook(&self, promise_hook: Option<PromiseHook>) {
        unsafe { (*self.promise_hook.get()) = promise_hook }
    }

    pub fn run_promise_hook<'a>(
        &self,
        ctx: Ctx<'a>,
        type_: PromiseHookType,
        promise: Value<'a>,
        parent: Value<'a>,
    ) {
        unsafe { (*self.promise_hook.get()).as_mut().unwrap()(ctx, type_, promise, parent) }
    }

    pub fn set_rejection_tracker(&self, tracker: Option<RejectionTracker>) {
        unsafe { (*self.rejection_tracker.get()) = tracker }
    }

    pub fn run_rejection_tracker<'a>(
        &self,
        ctx: Ctx<'a>,
        promise: Value<'a>,
        reason: Value<'a>,
        is_handled: bool,
    ) {
        unsafe {
            (*self.rejection_tracker.get()).as_mut().unwrap()(ctx, promise, reason, is_handled)
        }
    }

    pub fn set_interrupt_handler(&self, interupt: Option<InterruptHandler>) {
        unsafe { (*self.interrupt_handler.get()) = interupt }
    }

    pub fn run_interrupt_handler(&self) -> bool {
        unsafe {
            (*self.interrupt_handler.get())
                .as_mut()
                .is_some_and(|h| h())
        }
    }

    pub fn suspender(&self) -> Suspender {
        Suspender(self.suspend_request.clone())
    }

    pub fn decorators(&self) -> Decorators {
        self.decorators.get()
    }

    pub fn set_decorators(&self, decorators: Decorators) {
        self.decorators.set(decorators);
    }

    pub fn take_suspend_request(&self) -> bool {
        self.suspend_request.swap(false, Ordering::AcqRel)
    }

    pub fn snapshot_natives(&self) -> Option<Object<'js>> {
        unsafe {
            (*self.snapshot_natives.get())
                .as_ref()
                .map(|(tree, _)| tree.clone())
        }
    }

    pub fn snapshot_host(&self) -> Option<SnapshotHost> {
        unsafe {
            (*self.snapshot_natives.get())
                .as_ref()
                .map(|(_, host)| host.clone())
        }
    }

    pub fn set_snapshot_natives(&self, natives: Option<(Object<'js>, SnapshotHost)>) {
        unsafe { *self.snapshot_natives.get() = natives }
    }

    pub fn set_builtin_request(&self, request: Option<BuiltinRequest>) {
        unsafe { *self.builtin_request.get() = request }
    }

    pub fn take_builtin_request(&self) -> Option<BuiltinRequest> {
        unsafe { (*self.builtin_request.get()).take() }
    }

    pub fn set_resume_value(&self, value: Option<Value<'js>>) {
        unsafe { *self.resume_value.get() = value }
    }

    pub fn take_resume_value(&self) -> Option<Value<'js>> {
        unsafe { (*self.resume_value.get()).take() }
    }

    /// Replaces the cached prototypes of the Rust classes.
    pub fn reset_prototypes(&self, prototypes: impl IntoIterator<Item = (TypeId, Object<'js>)>) {
        unsafe {
            let map = &mut *self.prototypes.get();
            map.clear();
            map.extend(prototypes.into_iter().map(|(id, proto)| (id, Some(proto))));
        }
    }

    #[allow(dead_code)] // not used in no_std
    pub fn set_panic(&self, panic: Box<dyn Any + Send + 'static>) {
        self.panic.set(Some(panic))
    }

    pub fn take_panic(&self) -> Option<Box<dyn Any + Send + 'static>> {
        self.panic.take()
    }

    /// The class id of the Rust classes.
    #[inline]
    pub fn class_id() -> qjs::JSClassID {
        CLASS_ID.load(Ordering::Relaxed)
    }

    /// The class id of the Rust classes which can be called.
    #[inline]
    pub fn callable_id() -> qjs::JSClassID {
        CALLABLE_CLASS_ID.load(Ordering::Relaxed)
    }

    /// The class id of the exotic Rust classes.
    #[inline]
    pub fn exotic_id() -> qjs::JSClassID {
        EXOTIC_CLASS_ID.load(Ordering::Relaxed)
    }

    /// Returns the prototype of the class `C`, created with [`JsClass::prototype`] on first use.
    ///
    /// # Safety
    /// The returned reference points into the cache: it must be dropped before the cache can
    /// change, i.e. before running any code which could create a class prototype.
    unsafe fn prototype_ref<C: JsClass<'js>>(
        &self,
        ctx: &Ctx<'js>,
    ) -> Result<&Option<Object<'js>>, Error> {
        let id = TypeId::of::<C::Changed<'static>>();
        if let Some(proto) = (*self.prototypes.get()).get(&id) {
            return Ok(proto);
        }
        // the cache isn't borrowed while the prototype is created: creating it can create the
        // prototypes of other classes (e.g. the one of the Rust functions for its methods)
        let proto = C::prototype(ctx)?;
        Ok(&*(*self.prototypes.get()).entry(id).or_insert(proto))
    }

    pub fn get_or_insert_prototype<C: JsClass<'js>>(
        &self,
        ctx: &Ctx<'js>,
    ) -> Result<Option<Object<'js>>, Error> {
        unsafe { self.prototype_ref::<C>(ctx).cloned() }
    }

    /// Returns the prototype of the class `C` (`JS_NULL` if it has none) like
    /// [`Self::get_or_insert_prototype`], without a new reference: the cache keeps it alive
    /// until the prototypes are reset.
    pub fn get_or_insert_prototype_raw<C: JsClass<'js>>(
        &self,
        ctx: &Ctx<'js>,
    ) -> Result<qjs::JSValueConst, Error> {
        unsafe {
            self.prototype_ref::<C>(ctx)
                .map(|proto| proto.as_ref().map_or(qjs::JS_NULL, Object::as_js_value))
        }
    }

    /// Returns the atoms of the static `names`, created on first use and kept until the runtime
    /// is dropped.
    ///
    /// # Safety
    /// The atoms must not be used after the runtime is dropped.
    pub unsafe fn static_atoms(
        &self,
        ctx: &Ctx<'js>,
        names: &'static [&'static str],
    ) -> Result<*const [qjs::JSAtom], Error> {
        // static data lives at its address for the whole program, which identifies the names
        let key = (names.as_ptr() as usize, names.len());
        if let Some(atoms) = (*self.static_atoms.get()).get(&key) {
            return Ok(&**atoms as *const _);
        }
        let mut atoms = Vec::with_capacity(names.len());
        for name in names {
            match Atom::from_str(ctx.clone(), name) {
                Ok(atom) => atoms.push(atom.into_atom_val()),
                Err(error) => {
                    for atom in atoms {
                        qjs::JS_FreeAtom(ctx.as_ptr(), atom);
                    }
                    return Err(error);
                }
            }
        }
        let atoms = (*self.static_atoms.get())
            .entry(key)
            .or_insert(atoms.into_boxed_slice());
        Ok(&**atoms as *const _)
    }

    /// Cleans up all the internal state.
    ///
    /// Called before dropping the runtime `rt` to ensure that we drop everything before freeing
    /// the runtime.
    pub fn clear(&mut self, rt: *mut qjs::JSRuntime) {
        for (_, atoms) in self.static_atoms.get_mut().drain() {
            for &atom in atoms.iter() {
                unsafe { qjs::JS_FreeAtomRT(rt, atom) };
            }
        }
        self.rejection_tracker.get_mut().take();
        self.interrupt_handler.get_mut().take();
        self.panic.take();
        self.prototypes.get_mut().clear();
        self.snapshot_natives.get_mut().take();
        self.builtin_request.get_mut().take();
        self.resume_value.get_mut().take();
        #[cfg(feature = "futures")]
        self.spawner.take();
        self.userdata.clear();
    }
}
