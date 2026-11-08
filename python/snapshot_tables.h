/*
 * dquickts Python heap snapshots: the static tables whose entries can be
 * referenced from the heap (QPTypeSpec, QPModuleDef, QPMethodDef,
 * QPGetSetDef, QPMemberDef defined at file scope in the units included by
 * python/quickpy.c).  Every entry address is registered as a snapshot
 * native (python/snapshot.c); the list is hashed into the build signature.
 *
 * GENERATED: regenerate after adding such a table (a missing table only
 * makes JS_WriteSnapshot() fail with "unregistered native code pointer"):
 * for each python/<unit>.c included by quickpy.c, in include order (not
 * the capi_*.c units of CONFIG_PYTHON_CAPI: C-extension state is saved by
 * python/capi_snapshot.c), and
 * each local header it includes (recursively, first inclusion only), every
 * line matching
 *   ^static const (QPMethodDef|QPGetSetDef|QPMemberDef|QPTypeSpec|QPModuleDef) NAME([...])?( __maybe_unused)? =
 * (not inside a macro definition nor under a host #if: those tables are
 * listed by hand in QP_SNAP_TABLES_HOST) or
 * ^(QP_ITER_SPEC|QP_DICTITER_SPEC|QP_ODICTVIEW_SPEC)(NAME,
 * gives A(Type, NAME) for arrays and S(Type, NAME) for single objects,
 * grouped by type in the order QPTypeSpec, QPModuleDef, QPMethodDef,
 * QPGetSetDef, QPMemberDef.
 */
#ifndef QP_SNAPSHOT_TABLES_H
#define QP_SNAPSHOT_TABLES_H

#define QP_SNAP_TABLES(A, S) \
    S(QPTypeSpec, qp_fo_function_spec) /* funcobject.c */ \
    S(QPTypeSpec, qp_fo_method_spec) /* funcobject.c */ \
    S(QPTypeSpec, qp_fo_cell_spec) /* funcobject.c */ \
    S(QPTypeSpec, qp_co_code_spec) /* codeobject.c */ \
    S(QPTypeSpec, qp_fr_frame_spec) /* frameobject.c */ \
    S(QPTypeSpec, qp_long_spec) /* longobject.c */ \
    S(QPTypeSpec, qp_bool_spec) /* longobject.c */ \
    S(QPTypeSpec, qp_float_spec) /* floatobject.c */ \
    S(QPTypeSpec, qp_complex_spec) /* complexobject.c */ \
    S(QPTypeSpec, qp_str_spec) /* unicodeobject.c */ \
    S(QPTypeSpec, qpf_formatteriter_spec) /* unicode_format.c */ \
    S(QPTypeSpec, qpf_fieldnameiter_spec) /* unicode_format.c */ \
    S(QPTypeSpec, qpt_interpolation_spec) /* templateobject.c */ \
    S(QPTypeSpec, qpt_templateiter_spec) /* templateobject.c */ \
    S(QPTypeSpec, qpt_template_spec) /* templateobject.c */ \
    S(QPTypeSpec, qp_tuple_spec) /* tupleobject.c */ \
    S(QPTypeSpec, qp_list_spec) /* listobject.c */ \
    S(QPTypeSpec, qp_range_spec) /* rangeobject.c */ \
    S(QPTypeSpec, qp_slice_spec) /* sliceobject.c */ \
    S(QPTypeSpec, qp_ellipsis_spec) /* sliceobject.c */ \
    S(QPTypeSpec, qp_seqiter_spec) /* iterobject.c */ \
    S(QPTypeSpec, qp_calliter_spec) /* iterobject.c */ \
    S(QPTypeSpec, qp_listiter_spec) /* iterobject.c */ \
    S(QPTypeSpec, qp_listreviter_spec) /* iterobject.c */ \
    S(QPTypeSpec, qp_tupleiter_spec) /* iterobject.c */ \
    S(QPTypeSpec, qp_rangeiter_spec) /* iterobject.c */ \
    S(QPTypeSpec, qp_longrangeiter_spec) /* iterobject.c */ \
    S(QPTypeSpec, qp_strasciiiter_spec) /* iterobject.c */ \
    S(QPTypeSpec, qp_striter_spec) /* iterobject.c */ \
    S(QPTypeSpec, qp_bytesiter_spec) /* iterobject.c */ \
    S(QPTypeSpec, qp_bytearrayiter_spec) /* iterobject.c */ \
    S(QPTypeSpec, qp_dict_spec) /* dictobject.c */ \
    S(QPTypeSpec, qp_frozendict_spec) /* dictobject.c */ \
    S(QPTypeSpec, qp_dict_keyiterator_spec) /* dictobject.c */ \
    S(QPTypeSpec, qp_dict_valueiterator_spec) /* dictobject.c */ \
    S(QPTypeSpec, qp_dict_itemiterator_spec) /* dictobject.c */ \
    S(QPTypeSpec, qp_dict_reversekeyiterator_spec) /* dictobject.c */ \
    S(QPTypeSpec, qp_dict_reversevalueiterator_spec) /* dictobject.c */ \
    S(QPTypeSpec, qp_dict_reverseitemiterator_spec) /* dictobject.c */ \
    S(QPTypeSpec, qp_dict_keys_spec) /* dictobject.c */ \
    S(QPTypeSpec, qp_dict_items_spec) /* dictobject.c */ \
    S(QPTypeSpec, qp_dict_values_spec) /* dictobject.c */ \
    S(QPTypeSpec, qp_mappingproxy_spec) /* dictobject.c */ \
    S(QPTypeSpec, qp_set_spec) /* setobject.c */ \
    S(QPTypeSpec, qp_frozenset_spec) /* setobject.c */ \
    S(QPTypeSpec, qp_setiter_spec) /* setobject.c */ \
    S(QPTypeSpec, qp_bytes_spec) /* bytesobject.c */ \
    S(QPTypeSpec, qp_bytearray_spec) /* bytearrayobject.c */ \
    S(QPTypeSpec, qp_memoryview_spec) /* memoryobject.c */ \
    S(QPTypeSpec, qp_type_spec) /* typeobject.c */ \
    S(QPTypeSpec, qp_object_spec) /* typeobject.c */ \
    S(QPTypeSpec, qp_none_type_spec) /* typeobject.c */ \
    S(QPTypeSpec, qp_notimpl_type_spec) /* typeobject.c */ \
    S(QPTypeSpec, qp_super_spec) /* typeobject_attr.h */ \
    S(QPTypeSpec, qp_builtin_function_or_method_spec) /* descrobject.c */ \
    S(QPTypeSpec, qp_method_descriptor_spec) /* descrobject.c */ \
    S(QPTypeSpec, qp_classmethod_descriptor_spec) /* descrobject.c */ \
    S(QPTypeSpec, qp_getset_descriptor_spec) /* descrobject.c */ \
    S(QPTypeSpec, qp_member_descriptor_spec) /* descrobject.c */ \
    S(QPTypeSpec, qp_wrapper_descriptor_spec) /* descrobject.c */ \
    S(QPTypeSpec, qp_method_wrapper_spec) /* descrobject.c */ \
    S(QPTypeSpec, qp_property_spec) /* descrobject.c */ \
    S(QPTypeSpec, qp_classmethod_spec) /* descrobject.c */ \
    S(QPTypeSpec, qp_staticmethod_spec) /* descrobject.c */ \
    S(QPTypeSpec, qp_odict_spec) /* odictobject.c */ \
    S(QPTypeSpec, qp_odict_iterator_spec) /* odictobject.c */ \
    S(QPTypeSpec, qp_odict_keys_spec) /* odictobject.c */ \
    S(QPTypeSpec, qp_odict_values_spec) /* odictobject.c */ \
    S(QPTypeSpec, qp_odict_items_spec) /* odictobject.c */ \
    A(QPTypeSpec, qp_exc_specs) /* exceptions.c */ \
    S(QPTypeSpec, qp_tb_spec) /* traceback.c */ \
    S(QPTypeSpec, qp__generator_spec) /* genobject.c */ \
    S(QPTypeSpec, qp__coroutine_spec) /* genobject.c */ \
    S(QPTypeSpec, qp__async_generator_spec) /* genobject.c */ \
    S(QPTypeSpec, qp__coroutine_wrapper_spec) /* genobject.c */ \
    S(QPTypeSpec, qp__asend_spec) /* genobject.c */ \
    S(QPTypeSpec, qp__athrow_spec) /* genobject.c */ \
    S(QPTypeSpec, qp__wrapped_value_spec) /* genobject.c */ \
    S(QPTypeSpec, qp_map_spec) /* bltinmodule.c */ \
    S(QPTypeSpec, qp_filter_spec) /* bltinmodule.c */ \
    S(QPTypeSpec, qp_zip_spec) /* bltinmodule.c */ \
    S(QPTypeSpec, qp_enum_spec) /* bltinmodule.c */ \
    S(QPTypeSpec, qp_reversed_spec) /* bltinmodule.c */ \
    S(QPTypeSpec, qp_sentinel_spec) /* bltinmodule.c */ \
    S(QPTypeSpec, qp_anext_spec) /* bltinmodule.c */ \
    S(QPTypeSpec, qp_namespace_spec) /* sysmodule.c */ \
    S(QPTypeSpec, qp_stdprinter_spec) /* sysmodule.c */ \
    S(QPTypeSpec, qp_nodefault_spec) /* typevarobject.c */ \
    S(QPTypeSpec, qp_constevaluator_spec) /* typevarobject.c */ \
    S(QPTypeSpec, qp_typevar_spec) /* typevarobject.c */ \
    S(QPTypeSpec, qp_paramspecargs_spec) /* typevarobject.c */ \
    S(QPTypeSpec, qp_paramspeckwargs_spec) /* typevarobject.c */ \
    S(QPTypeSpec, qp_paramspec_spec) /* typevarobject.c */ \
    S(QPTypeSpec, qp_typevartuple_spec) /* typevarobject.c */ \
    S(QPTypeSpec, qp_typealias_spec) /* typevarobject.c */ \
    S(QPTypeSpec, qp_generic_spec) /* typevarobject.c */ \
    S(QPTypeSpec, qp_ga_spec) /* genericaliasobject.c */ \
    S(QPTypeSpec, qp_ga_iter_spec) /* genericaliasobject.c */ \
    S(QPTypeSpec, qp_union_spec) /* genericaliasobject.c */ \
    S(QPTypeSpec, qp_module_spec) /* moduleobject.c */ \
    S(QPTypeSpec, qp_lazy_import_spec) /* import.c */ \
    S(QPTypeSpec, qp_jsi_awaiter_spec) /* jsinterop.c */ \
    S(QPTypeSpec, qp_jsobject_spec) /* jsinterop.c */ \
    S(QPTypeSpec, qp_context_spec) /* mod_misc.c */ \
    S(QPTypeSpec, qp_ctxvar_spec) /* mod_misc.c */ \
    S(QPTypeSpec, qp_ctxtok_spec) /* mod_misc.c */ \
    S(QPTypeSpec, qp_ctxtok_missing_spec) /* mod_misc.c */ \
    S(QPTypeSpec, qp_wr_ref_spec) /* mod_weakref.c */ \
    S(QPTypeSpec, qp_wr_proxy_spec) /* mod_weakref.c */ \
    S(QPTypeSpec, qp_wr_callproxy_spec) /* mod_weakref.c */ \
    S(QPTypeSpec, qp_thread_handle_spec) /* mod_thread.c */ \
    S(QPTypeSpec, qp_thread_lock_spec) /* mod_thread.c */ \
    S(QPTypeSpec, qp_thread_rlock_spec) /* mod_thread.c */ \
    S(QPTypeSpec, qp_thread_local_spec) /* mod_thread.c */ \
    S(QPTypeSpec, qp_de_spec) /* mod_posix.c */ \
    S(QPTypeSpec, qp_si_spec) /* mod_posix.c */ \
    S(QPTypeSpec, qp_poll_spec) /* mod_select.c */ \
    S(QPTypeSpec, qp_cd_emap_spec) /* mod_codecs.c */ \
    S(QPTypeSpec, qpast_ast_spec) /* mod_ast.c */ \
    S(QPTypeSpec, qp_tokiter_spec) /* mod_tokenize.c */ \
    S(QPTypeSpec, qp_ste_spec) /* mod_symtable.c */ \
    S(QPTypeSpec, qp_tic_sicm_spec) /* mod_testinternalcapi.c */ \
    S(QPModuleDef, qp_moddef__suggestions) /* errors.c */ \
    S(QPModuleDef, qp_moddef__missing_stdlib_info) /* traceback.c */ \
    S(QPModuleDef, qp_moddef_builtins) /* bltinmodule.c */ \
    S(QPModuleDef, qp_moddef_sys__jit) /* sysmodule.c */ \
    S(QPModuleDef, qp_moddef_sys) /* sysmodule.c */ \
    S(QPModuleDef, qp_moddef__typing) /* typevarobject.c */ \
    S(QPModuleDef, qp_moddef__string) /* import.c */ \
    S(QPModuleDef, qp_moddef_js) /* jsinterop.c */ \
    S(QPModuleDef, qp_moddef_gc) /* mod_misc.c */ \
    S(QPModuleDef, qp_moddef_atexit) /* mod_misc.c */ \
    S(QPModuleDef, qp_moddef__contextvars) /* mod_misc.c */ \
    S(QPModuleDef, qp_moddef__warnings) /* mod_misc.c */ \
    S(QPModuleDef, qp_moddef__signal) /* mod_misc.c */ \
    S(QPModuleDef, qp_moddef_marshal) /* mod_misc.c */ \
    S(QPModuleDef, qp_moddef_sys_monitoring) /* instrumentation.c */ \
    S(QPModuleDef, qp_moddef__weakref) /* mod_weakref.c */ \
    S(QPModuleDef, qp_moddef__thread) /* mod_thread.c */ \
    S(QPModuleDef, qp_moddef_time) /* mod_time.c */ \
    S(QPModuleDef, qp_moddef_errno) /* mod_errno.c */ \
    S(QPModuleDef, qp_moddef__stat) /* mod_stat.c */ \
    S(QPModuleDef, qp_moddef_posix) /* mod_posix.c */ \
    S(QPModuleDef, qp_moddef_select) /* mod_select.c */ \
    S(QPModuleDef, qp_moddef__posixsubprocess) /* mod_posixsubprocess.c */ \
    S(QPModuleDef, qp_moddef__codecs) /* mod_codecs.c */ \
    S(QPModuleDef, qp_moddef__locale) /* mod_locale.c */ \
    S(QPModuleDef, qp_moddef__io) /* mod_io.c */ \
    S(QPModuleDef, qp_moddef__imp) /* mod_imp.c */ \
    S(QPModuleDef, qp_moddef__ast) /* mod_ast.c */ \
    S(QPModuleDef, qp_moddef__tokenize) /* mod_tokenize.c */ \
    S(QPModuleDef, qp_moddef__symtable) /* mod_symtable.c */ \
    S(QPModuleDef, qp_moddef__types) /* mod_types.c */ \
    S(QPModuleDef, qp_moddef__sysconfigdata) /* mod_types.c */ \
    S(QPModuleDef, qp_moddef__testinternalcapi) /* mod_testinternalcapi.c */ \
    A(QPMethodDef, qp_fo_method_methods) /* funcobject.c */ \
    A(QPMethodDef, qp_co_methods) /* codeobject.c */ \
    A(QPMethodDef, qp_fr_methods) /* frameobject.c */ \
    A(QPMethodDef, qp_long_methods) /* longobject.c */ \
    A(QPMethodDef, qp_float_methods) /* floatobject.c */ \
    A(QPMethodDef, qp_complex_methods) /* complexobject.c */ \
    A(QPMethodDef, qp_str_methods) /* unicodeobject.c */ \
    A(QPMethodDef, qp_string_module_methods) /* unicode_format.c */ \
    A(QPMethodDef, qpt_interpolation_methods) /* templateobject.c */ \
    A(QPMethodDef, qpt_template_methods) /* templateobject.c */ \
    A(QPMethodDef, qp_tuple_methods) /* tupleobject.c */ \
    A(QPMethodDef, qp_list_methods) /* listobject.c */ \
    A(QPMethodDef, qp_range_methods) /* rangeobject.c */ \
    A(QPMethodDef, qp_slice_methods) /* sliceobject.c */ \
    A(QPMethodDef, qp_ellipsis_methods) /* sliceobject.c */ \
    A(QPMethodDef, qp_seqiter_methods) /* iterobject.c */ \
    A(QPMethodDef, qp_calliter_methods) /* iterobject.c */ \
    A(QPMethodDef, qp_listiter_methods) /* iterobject.c */ \
    A(QPMethodDef, qp_listreviter_methods) /* iterobject.c */ \
    A(QPMethodDef, qp_tupleiter_methods) /* iterobject.c */ \
    A(QPMethodDef, qp_rangeiter_methods) /* iterobject.c */ \
    A(QPMethodDef, qp_longrangeiter_methods) /* iterobject.c */ \
    A(QPMethodDef, qp_striter_methods) /* iterobject.c */ \
    A(QPMethodDef, qp_bytesiter_methods) /* iterobject.c */ \
    A(QPMethodDef, qp_bytearrayiter_methods) /* iterobject.c */ \
    A(QPMethodDef, qp_dict_methods) /* dictobject.c */ \
    A(QPMethodDef, qp_frozendict_methods) /* dictobject.c */ \
    A(QPMethodDef, qp_dictiter_methods) /* dictobject.c */ \
    A(QPMethodDef, qp_dictkeys_methods) /* dictobject.c */ \
    A(QPMethodDef, qp_dictitems_methods) /* dictobject.c */ \
    A(QPMethodDef, qp_dictvalues_methods) /* dictobject.c */ \
    A(QPMethodDef, qp_mappingproxy_methods) /* dictobject.c */ \
    A(QPMethodDef, qp_set_methods) /* setobject.c */ \
    A(QPMethodDef, qp_frozenset_methods) /* setobject.c */ \
    A(QPMethodDef, qp_setiter_methods) /* setobject.c */ \
    A(QPMethodDef, qp_bytes_methods) /* bytesobject.c */ \
    A(QPMethodDef, qp_bytearray_methods) /* bytearrayobject.c */ \
    A(QPMethodDef, qp_memoryview_methods) /* memoryobject.c */ \
    A(QPMethodDef, qp_type_methods) /* typeobject.c */ \
    A(QPMethodDef, qp_object_methods) /* typeobject.c */ \
    A(QPMethodDef, qp_notimpl_methods) /* typeobject.c */ \
    S(QPMethodDef, qp_tp_new_methoddef) /* typeobject_slots.h */ \
    A(QPMethodDef, qp_cfunction_methods) /* descrobject.c */ \
    A(QPMethodDef, qp_descr_methods) /* descrobject.c */ \
    A(QPMethodDef, qp_property_methods) /* descrobject.c */ \
    A(QPMethodDef, qp_funcwrap_methods) /* descrobject.c */ \
    A(QPMethodDef, qp_odict_methods) /* odictobject.c */ \
    A(QPMethodDef, qp_odictiter_methods) /* odictobject.c */ \
    A(QPMethodDef, qp_odictview_methods) /* odictobject.c */ \
    A(QPMethodDef, qp_exc_base_methods) /* exceptions.c */ \
    A(QPMethodDef, qp_exc_eg_methods) /* exceptions.c */ \
    A(QPMethodDef, qp_exc_importerror_methods) /* exceptions.c */ \
    A(QPMethodDef, qp_exc_oserror_methods) /* exceptions.c */ \
    A(QPMethodDef, qp_exc_attributeerror_methods) /* exceptions.c */ \
    A(QPMethodDef, qp_suggestions_methods) /* errors.c */ \
    A(QPMethodDef, qp_tb_methods) /* traceback.c */ \
    A(QPMethodDef, qp__gen_methods) /* genobject.c */ \
    A(QPMethodDef, qp__coro_methods) /* genobject.c */ \
    A(QPMethodDef, qp__agen_methods) /* genobject.c */ \
    A(QPMethodDef, qp__cw_methods) /* genobject.c */ \
    A(QPMethodDef, qp__asend_methods) /* genobject.c */ \
    A(QPMethodDef, qp__athrow_methods) /* genobject.c */ \
    A(QPMethodDef, qp_map_methods) /* bltinmodule.c */ \
    A(QPMethodDef, qp_filter_methods) /* bltinmodule.c */ \
    A(QPMethodDef, qp_zip_methods) /* bltinmodule.c */ \
    A(QPMethodDef, qp_enum_methods) /* bltinmodule.c */ \
    A(QPMethodDef, qp_reversed_methods) /* bltinmodule.c */ \
    A(QPMethodDef, qp_sentinel_methods) /* bltinmodule.c */ \
    A(QPMethodDef, qp_anext_methods) /* bltinmodule.c */ \
    A(QPMethodDef, qp_builtin_methods) /* bltinmodule.c */ \
    A(QPMethodDef, qp_namespace_methods) /* sysmodule.c */ \
    A(QPMethodDef, qp_sp_methods) /* sysmodule.c */ \
    A(QPMethodDef, qp_sys_methods) /* sysmodule.c */ \
    A(QPMethodDef, qp_sys_jit_methods) /* sysmodule.c */ \
    A(QPMethodDef, qp_nodefault_methods) /* typevarobject.c */ \
    A(QPMethodDef, qp_typevar_methods) /* typevarobject.c */ \
    A(QPMethodDef, qp_paramspecargs_methods) /* typevarobject.c */ \
    A(QPMethodDef, qp_paramspeckwargs_methods) /* typevarobject.c */ \
    A(QPMethodDef, qp_paramspec_methods) /* typevarobject.c */ \
    A(QPMethodDef, qp_typevartuple_methods) /* typevarobject.c */ \
    A(QPMethodDef, qp_typealias_methods) /* typevarobject.c */ \
    A(QPMethodDef, qp_generic_methods) /* typevarobject.c */ \
    A(QPMethodDef, qp_typing_methods) /* typevarobject.c */ \
    A(QPMethodDef, qp_ga_methods) /* genericaliasobject.c */ \
    A(QPMethodDef, qp_ga_iter_methods) /* genericaliasobject.c */ \
    A(QPMethodDef, qp_union_methods) /* genericaliasobject.c */ \
    S(QPMethodDef, qp_ss_field_get_def) /* structseq.c */ \
    S(QPMethodDef, qp_ss_new_def) /* structseq.c */ \
    A(QPMethodDef, qp_ss_methods) /* structseq.c */ \
    A(QPMethodDef, qp_module_methods) /* moduleobject.c */ \
    A(QPMethodDef, qp_lazy_import_methods) /* import.c */ \
    A(QPMethodDef, qp_jsobj_methods) /* jsinterop.c */ \
    A(QPMethodDef, qp_jsmod_methods) /* jsinterop.c */ \
    A(QPMethodDef, qp_gc_methods) /* mod_misc.c */ \
    A(QPMethodDef, qp_atexit_methods) /* mod_misc.c */ \
    A(QPMethodDef, qp_context_methods) /* mod_misc.c */ \
    A(QPMethodDef, qp_ctxvar_methods) /* mod_misc.c */ \
    A(QPMethodDef, qp_ctxtok_methods) /* mod_misc.c */ \
    A(QPMethodDef, qp_ctxvar_module_methods) /* mod_misc.c */ \
    A(QPMethodDef, qp_warnings_methods) /* mod_misc.c */ \
    A(QPMethodDef, qp_signal_methods) /* mod_misc.c */ \
    A(QPMethodDef, qp_marshal_methods) /* mod_misc.c */ \
    A(QPMethodDef, qp_mon_methods) /* instrumentation.c */ \
    A(QPMethodDef, qp_wr_ref_methods) /* mod_weakref.c */ \
    A(QPMethodDef, qp_wr_proxy_methods) /* mod_weakref.c */ \
    A(QPMethodDef, qp_wr_module_methods) /* mod_weakref.c */ \
    A(QPMethodDef, qp_thread_handle_methods) /* mod_thread.c */ \
    A(QPMethodDef, qp_thread_lock_methods) /* mod_thread.c */ \
    A(QPMethodDef, qp_thread_rlock_methods) /* mod_thread.c */ \
    A(QPMethodDef, qp_thread_methods) /* mod_thread.c */ \
    A(QPMethodDef, qp_time_methods) /* mod_time.c */ \
    A(QPMethodDef, qp_stat_methods) /* mod_stat.c */ \
    A(QPMethodDef, qp_de_methods) /* mod_posix.c */ \
    A(QPMethodDef, qp_si_methods) /* mod_posix.c */ \
    A(QPMethodDef, qp_posix_methods) /* mod_posix.c */ \
    S(QPMethodDef, qp_os_statresult_new_def) /* mod_posix.c */ \
    A(QPMethodDef, qp_poll_methods) /* mod_select.c */ \
    A(QPMethodDef, qp_select_methods) /* mod_select.c */ \
    A(QPMethodDef, qp_posixsubprocess_methods) /* mod_posixsubprocess.c */ \
    A(QPMethodDef, qp_cd_eh_defs) /* mod_codecs.c */ \
    A(QPMethodDef, qp_cd_emap_methods) /* mod_codecs.c */ \
    A(QPMethodDef, qp_codecs_methods) /* mod_codecs.c */ \
    A(QPMethodDef, qp_locale_methods) /* mod_locale.c */ \
    S(QPMethodDef, qp_iom_read_file_def) /* mod_io.c */ \
    S(QPMethodDef, qp_iom_mark_native_def) /* mod_io.c */ \
    A(QPMethodDef, qp_io_methods) /* mod_io.c */ \
    A(QPMethodDef, qp_imp_methods) /* mod_imp.c */ \
    A(QPMethodDef, qpast_ast_methods) /* mod_ast.c */ \
    A(QPMethodDef, qpast_module_methods) /* mod_ast.c */ \
    A(QPMethodDef, qp_symtable_methods) /* mod_symtable.c */ \
    A(QPMethodDef, qp_tic_sicm_methods) /* mod_testinternalcapi.c */ \
    A(QPMethodDef, qp_tic_methods) /* mod_testinternalcapi.c */ \
    A(QPGetSetDef, qp_fo_func_getsets) /* funcobject.c */ \
    A(QPGetSetDef, qp_fo_method_getsets) /* funcobject.c */ \
    A(QPGetSetDef, qp_fo_cell_getsets) /* funcobject.c */ \
    A(QPGetSetDef, qp_co_getsets) /* codeobject.c */ \
    A(QPGetSetDef, qp_fr_getsets) /* frameobject.c */ \
    A(QPGetSetDef, qp_long_getsets) /* longobject.c */ \
    A(QPGetSetDef, qp_float_getsets) /* floatobject.c */ \
    A(QPGetSetDef, qp_complex_getsets) /* complexobject.c */ \
    A(QPGetSetDef, qpt_template_getsets) /* templateobject.c */ \
    A(QPGetSetDef, qp_range_getsets) /* rangeobject.c */ \
    A(QPGetSetDef, qp_slice_getsets) /* sliceobject.c */ \
    A(QPGetSetDef, qp_dictview_getsets) /* dictobject.c */ \
    A(QPGetSetDef, qp_memoryview_getsets) /* memoryobject.c */ \
    A(QPGetSetDef, qp_type_getsets) /* typeobject.c */ \
    A(QPGetSetDef, qp_object_getsets) /* typeobject.c */ \
    A(QPGetSetDef, qp_subtype_getsets) /* typeobject_attr.h */ \
    A(QPGetSetDef, qp_super_getsets) /* typeobject_attr.h */ \
    A(QPGetSetDef, qp_cfunction_getsets) /* descrobject.c */ \
    A(QPGetSetDef, qp_methoddescr_getsets) /* descrobject.c */ \
    A(QPGetSetDef, qp_attrdescr_getsets) /* descrobject.c */ \
    A(QPGetSetDef, qp_methodwrapper_getsets) /* descrobject.c */ \
    A(QPGetSetDef, qp_property_getsets) /* descrobject.c */ \
    A(QPGetSetDef, qp_funcwrap_getsets) /* descrobject.c */ \
    A(QPGetSetDef, qp_odict_getsets) /* odictobject.c */ \
    A(QPGetSetDef, qp_exc_base_getsets) /* exceptions.c */ \
    A(QPGetSetDef, qp_exc_stopiteration_getsets) /* exceptions.c */ \
    A(QPGetSetDef, qp_exc_systemexit_getsets) /* exceptions.c */ \
    A(QPGetSetDef, qp_exc_eg_getsets) /* exceptions.c */ \
    A(QPGetSetDef, qp_exc_importerror_getsets) /* exceptions.c */ \
    A(QPGetSetDef, qp_exc_oserror_getsets) /* exceptions.c */ \
    A(QPGetSetDef, qp_exc_nameerror_getsets) /* exceptions.c */ \
    A(QPGetSetDef, qp_exc_attributeerror_getsets) /* exceptions.c */ \
    A(QPGetSetDef, qp_exc_syntaxerror_getsets) /* exceptions.c */ \
    A(QPGetSetDef, qp_exc_ue_getsets) /* exceptions.c */ \
    A(QPGetSetDef, qp_tb_getsets) /* traceback.c */ \
    A(QPGetSetDef, qp__gen_getsets) /* genobject.c */ \
    A(QPGetSetDef, qp__coro_getsets) /* genobject.c */ \
    A(QPGetSetDef, qp__agen_getsets) /* genobject.c */ \
    A(QPGetSetDef, qp_namespace_getsets) /* sysmodule.c */ \
    A(QPGetSetDef, qp_sp_getsets) /* sysmodule.c */ \
    A(QPGetSetDef, qp_typevar_getsets) /* typevarobject.c */ \
    A(QPGetSetDef, qp_paramspec_getsets) /* typevarobject.c */ \
    A(QPGetSetDef, qp_typevartuple_getsets) /* typevarobject.c */ \
    A(QPGetSetDef, qp_typealias_getsets) /* typevarobject.c */ \
    A(QPGetSetDef, qp_ga_getsets) /* genericaliasobject.c */ \
    A(QPGetSetDef, qp_union_getsets) /* genericaliasobject.c */ \
    A(QPGetSetDef, qp_module_getsets) /* moduleobject.c */ \
    A(QPGetSetDef, qp_ctxtok_getsets) /* mod_misc.c */ \
    A(QPGetSetDef, qp_wr_ref_getsets) /* mod_weakref.c */ \
    A(QPGetSetDef, qp_thread_handle_getsets) /* mod_thread.c */ \
    A(QPGetSetDef, qp_thread_local_getsets) /* mod_thread.c */ \
    A(QPGetSetDef, qpast_ast_getsets) /* mod_ast.c */ \
    A(QPMemberDef, qpt_interpolation_members) /* templateobject.c */ \
    A(QPMemberDef, qpt_template_members) /* templateobject.c */ \
    A(QPMemberDef, qp_sentinel_members) /* bltinmodule.c */ \
    A(QPMemberDef, qp_typevar_members) /* typevarobject.c */ \
    A(QPMemberDef, qp_paramspecattr_members) /* typevarobject.c */ \
    A(QPMemberDef, qp_paramspec_members) /* typevarobject.c */ \
    A(QPMemberDef, qp_typealias_members) /* typevarobject.c */ \
    A(QPMemberDef, qp_ga_members) /* genericaliasobject.c */ \
    A(QPMemberDef, qp_union_members) /* genericaliasobject.c */ \
    A(QPMemberDef, qp_ctxvar_members) /* mod_misc.c */ \
    A(QPMemberDef, qp_de_members) /* mod_posix.c */ \
    A(QPMemberDef, qp_ste_members) /* mod_symtable.c */

/* Host-specific tables, outside the generated list (the tables defined
   under a host #if).  Expanded after QP_SNAP_TABLES. */
/* the kqueue objects of mod_select.c: macOS/BSD only */
#ifdef QP_HAVE_KQUEUE
#define QP_SNAP_TABLES_KQUEUE(A, S) \
    S(QPTypeSpec, qp_kevent_spec) /* mod_select.c */ \
    S(QPTypeSpec, qp_kqueue_spec) /* mod_select.c */ \
    A(QPMethodDef, qp_kq_methods) /* mod_select.c */ \
    A(QPGetSetDef, qp_kev_getsets) /* mod_select.c */ \
    A(QPGetSetDef, qp_kq_getsets) /* mod_select.c */
#else
#define QP_SNAP_TABLES_KQUEUE(A, S)
#endif
/* os.sched_param of mod_posix.c: not on macOS */
#ifdef QP_OS_HAVE_SCHED_PARAM
#define QP_SNAP_TABLES_SCHED_PARAM(A, S) \
    S(QPMethodDef, qp_os_sched_param_new_def) /* mod_posix.c */ \
    S(QPMethodDef, qp_os_sched_param_reduce_def) /* mod_posix.c */
#else
#define QP_SNAP_TABLES_SCHED_PARAM(A, S)
#endif
#define QP_SNAP_TABLES_HOST(A, S) \
    QP_SNAP_TABLES_KQUEUE(A, S) \
    QP_SNAP_TABLES_SCHED_PARAM(A, S)

#endif /* QP_SNAPSHOT_TABLES_H */
