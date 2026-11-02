/*
 * dquickts Python C API (plan D5, unit B): slot id tables.
 * GENERATED from CPython 3.15.2 Python/slots_generated.c and
 * Include/internal/pycore_slots_generated.h (Tools/build/generate_slots.py,
 * PSF-2.0) with the _PySlot_ prefix renamed to qp_capi_slot_; included once
 * by python/capi_type.c.  Do not edit: regenerate from the CPython tag.
 */
static const char *const qp_capi_slot_names[] = {
    "Py_slot_end",
    "Py_bf_getbuffer/Py_mod_create",
    "Py_bf_releasebuffer/Py_mod_exec",
    "Py_mp_ass_subscript/Py_mod_multiple_interpreters",
    "Py_mp_length/Py_mod_gil",
    "Py_mp_subscript",
    "Py_nb_absolute",
    "Py_nb_add",
    "Py_nb_and",
    "Py_nb_bool",
    "Py_nb_divmod",
    "Py_nb_float",
    "Py_nb_floor_divide",
    "Py_nb_index",
    "Py_nb_inplace_add",
    "Py_nb_inplace_and",
    "Py_nb_inplace_floor_divide",
    "Py_nb_inplace_lshift",
    "Py_nb_inplace_multiply",
    "Py_nb_inplace_or",
    "Py_nb_inplace_power",
    "Py_nb_inplace_remainder",
    "Py_nb_inplace_rshift",
    "Py_nb_inplace_subtract",
    "Py_nb_inplace_true_divide",
    "Py_nb_inplace_xor",
    "Py_nb_int",
    "Py_nb_invert",
    "Py_nb_lshift",
    "Py_nb_multiply",
    "Py_nb_negative",
    "Py_nb_or",
    "Py_nb_positive",
    "Py_nb_power",
    "Py_nb_remainder",
    "Py_nb_rshift",
    "Py_nb_subtract",
    "Py_nb_true_divide",
    "Py_nb_xor",
    "Py_sq_ass_item",
    "Py_sq_concat",
    "Py_sq_contains",
    "Py_sq_inplace_concat",
    "Py_sq_inplace_repeat",
    "Py_sq_item",
    "Py_sq_length",
    "Py_sq_repeat",
    "Py_tp_alloc",
    "Py_tp_base",
    "Py_tp_bases",
    "Py_tp_call",
    "Py_tp_clear",
    "Py_tp_dealloc",
    "Py_tp_del",
    "Py_tp_descr_get",
    "Py_tp_descr_set",
    "Py_tp_doc",
    "Py_tp_getattr",
    "Py_tp_getattro",
    "Py_tp_hash",
    "Py_tp_init",
    "Py_tp_is_gc",
    "Py_tp_iter",
    "Py_tp_iternext",
    "Py_tp_methods",
    "Py_tp_new",
    "Py_tp_repr",
    "Py_tp_richcompare",
    "Py_tp_setattr",
    "Py_tp_setattro",
    "Py_tp_str",
    "Py_tp_traverse",
    "Py_tp_members",
    "Py_tp_getset",
    "Py_tp_free",
    "Py_nb_matrix_multiply",
    "Py_nb_inplace_matrix_multiply",
    "Py_am_await",
    "Py_am_aiter",
    "Py_am_anext",
    "Py_tp_finalize",
    "Py_am_send",
    "Py_tp_vectorcall",
    "Py_tp_token",
    "Py_mod_create",
    "Py_mod_exec",
    "Py_mod_multiple_interpreters",
    "Py_mod_gil",
    "Py_bf_getbuffer",
    "Py_bf_releasebuffer",
    "Py_mp_ass_subscript",
    "Py_mp_length",
    "Py_slot_subslots",
    "Py_tp_slots",
    "Py_mod_slots",
    "Py_tp_name",
    "Py_tp_basicsize",
    "Py_tp_extra_basicsize",
    "Py_tp_itemsize",
    "Py_tp_flags",
    "Py_mod_name",
    "Py_mod_doc",
    "Py_mod_state_size",
    "Py_mod_methods",
    "Py_mod_state_traverse",
    "Py_mod_state_clear",
    "Py_mod_state_free",
    "Py_tp_metaclass",
    "Py_tp_module",
    "Py_mod_abi",
    "Py_mod_token",
    NULL
};


static uint16_t
qp_capi_slot_resolve_type_slot(uint16_t slot_id)
{
    switch (slot_id) {
        case 1:
            return Py_bf_getbuffer;
        case 2:
            return Py_bf_releasebuffer;
        case 3:
            return Py_mp_ass_subscript;
        case 4:
            return Py_mp_length;
        case Py_slot_end:
        case Py_mp_subscript:
        case Py_nb_absolute:
        case Py_nb_add:
        case Py_nb_and:
        case Py_nb_bool:
        case Py_nb_divmod:
        case Py_nb_float:
        case Py_nb_floor_divide:
        case Py_nb_index:
        case Py_nb_inplace_add:
        case Py_nb_inplace_and:
        case Py_nb_inplace_floor_divide:
        case Py_nb_inplace_lshift:
        case Py_nb_inplace_multiply:
        case Py_nb_inplace_or:
        case Py_nb_inplace_power:
        case Py_nb_inplace_remainder:
        case Py_nb_inplace_rshift:
        case Py_nb_inplace_subtract:
        case Py_nb_inplace_true_divide:
        case Py_nb_inplace_xor:
        case Py_nb_int:
        case Py_nb_invert:
        case Py_nb_lshift:
        case Py_nb_multiply:
        case Py_nb_negative:
        case Py_nb_or:
        case Py_nb_positive:
        case Py_nb_power:
        case Py_nb_remainder:
        case Py_nb_rshift:
        case Py_nb_subtract:
        case Py_nb_true_divide:
        case Py_nb_xor:
        case Py_sq_ass_item:
        case Py_sq_concat:
        case Py_sq_contains:
        case Py_sq_inplace_concat:
        case Py_sq_inplace_repeat:
        case Py_sq_item:
        case Py_sq_length:
        case Py_sq_repeat:
        case Py_tp_alloc:
        case Py_tp_base:
        case Py_tp_bases:
        case Py_tp_call:
        case Py_tp_clear:
        case Py_tp_dealloc:
        case Py_tp_del:
        case Py_tp_descr_get:
        case Py_tp_descr_set:
        case Py_tp_doc:
        case Py_tp_getattr:
        case Py_tp_getattro:
        case Py_tp_hash:
        case Py_tp_init:
        case Py_tp_is_gc:
        case Py_tp_iter:
        case Py_tp_iternext:
        case Py_tp_methods:
        case Py_tp_new:
        case Py_tp_repr:
        case Py_tp_richcompare:
        case Py_tp_setattr:
        case Py_tp_setattro:
        case Py_tp_str:
        case Py_tp_traverse:
        case Py_tp_members:
        case Py_tp_getset:
        case Py_tp_free:
        case Py_nb_matrix_multiply:
        case Py_nb_inplace_matrix_multiply:
        case Py_am_await:
        case Py_am_aiter:
        case Py_am_anext:
        case Py_tp_finalize:
        case Py_am_send:
        case Py_tp_vectorcall:
        case Py_tp_token:
        case Py_bf_getbuffer:
        case Py_bf_releasebuffer:
        case Py_mp_ass_subscript:
        case Py_mp_length:
        case Py_slot_subslots:
        case Py_tp_slots:
        case Py_tp_name:
        case Py_tp_basicsize:
        case Py_tp_extra_basicsize:
        case Py_tp_itemsize:
        case Py_tp_flags:
        case Py_tp_metaclass:
        case Py_tp_module:
            return slot_id;
        default:
            return Py_slot_invalid;
    }
}

static uint16_t
qp_capi_slot_resolve_mod_slot(uint16_t slot_id)
{
    switch (slot_id) {
        case 1:
            return Py_mod_create;
        case 2:
            return Py_mod_exec;
        case 3:
            return Py_mod_multiple_interpreters;
        case 4:
            return Py_mod_gil;
        case Py_slot_end:
        case Py_mod_create:
        case Py_mod_exec:
        case Py_mod_multiple_interpreters:
        case Py_mod_gil:
        case Py_slot_subslots:
        case Py_mod_slots:
        case Py_mod_name:
        case Py_mod_doc:
        case Py_mod_state_size:
        case Py_mod_methods:
        case Py_mod_state_traverse:
        case Py_mod_state_clear:
        case Py_mod_state_free:
        case Py_mod_abi:
        case Py_mod_token:
            return slot_id;
        default:
            return Py_slot_invalid;
    }
}

static void*
qp_capi_slot_type_getslot(PyTypeObject *tp, uint16_t slot_id)
{
    switch (slot_id) {
        case Py_mp_subscript:
            if (!(tp->tp_as_mapping)) return NULL;
            return (void*)tp->tp_as_mapping->mp_subscript;
        case Py_nb_absolute:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_absolute;
        case Py_nb_add:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_add;
        case Py_nb_and:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_and;
        case Py_nb_bool:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_bool;
        case Py_nb_divmod:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_divmod;
        case Py_nb_float:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_float;
        case Py_nb_floor_divide:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_floor_divide;
        case Py_nb_index:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_index;
        case Py_nb_inplace_add:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_inplace_add;
        case Py_nb_inplace_and:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_inplace_and;
        case Py_nb_inplace_floor_divide:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_inplace_floor_divide;
        case Py_nb_inplace_lshift:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_inplace_lshift;
        case Py_nb_inplace_multiply:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_inplace_multiply;
        case Py_nb_inplace_or:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_inplace_or;
        case Py_nb_inplace_power:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_inplace_power;
        case Py_nb_inplace_remainder:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_inplace_remainder;
        case Py_nb_inplace_rshift:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_inplace_rshift;
        case Py_nb_inplace_subtract:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_inplace_subtract;
        case Py_nb_inplace_true_divide:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_inplace_true_divide;
        case Py_nb_inplace_xor:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_inplace_xor;
        case Py_nb_int:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_int;
        case Py_nb_invert:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_invert;
        case Py_nb_lshift:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_lshift;
        case Py_nb_multiply:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_multiply;
        case Py_nb_negative:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_negative;
        case Py_nb_or:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_or;
        case Py_nb_positive:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_positive;
        case Py_nb_power:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_power;
        case Py_nb_remainder:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_remainder;
        case Py_nb_rshift:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_rshift;
        case Py_nb_subtract:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_subtract;
        case Py_nb_true_divide:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_true_divide;
        case Py_nb_xor:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_xor;
        case Py_sq_ass_item:
            if (!(tp->tp_as_sequence)) return NULL;
            return (void*)tp->tp_as_sequence->sq_ass_item;
        case Py_sq_concat:
            if (!(tp->tp_as_sequence)) return NULL;
            return (void*)tp->tp_as_sequence->sq_concat;
        case Py_sq_contains:
            if (!(tp->tp_as_sequence)) return NULL;
            return (void*)tp->tp_as_sequence->sq_contains;
        case Py_sq_inplace_concat:
            if (!(tp->tp_as_sequence)) return NULL;
            return (void*)tp->tp_as_sequence->sq_inplace_concat;
        case Py_sq_inplace_repeat:
            if (!(tp->tp_as_sequence)) return NULL;
            return (void*)tp->tp_as_sequence->sq_inplace_repeat;
        case Py_sq_item:
            if (!(tp->tp_as_sequence)) return NULL;
            return (void*)tp->tp_as_sequence->sq_item;
        case Py_sq_length:
            if (!(tp->tp_as_sequence)) return NULL;
            return (void*)tp->tp_as_sequence->sq_length;
        case Py_sq_repeat:
            if (!(tp->tp_as_sequence)) return NULL;
            return (void*)tp->tp_as_sequence->sq_repeat;
        case Py_tp_alloc:
            return (void*)tp->tp_alloc;
        case Py_tp_base:
            return (void*)tp->tp_base;
        case Py_tp_bases:
            return (void*)tp->tp_bases;
        case Py_tp_call:
            return (void*)tp->tp_call;
        case Py_tp_clear:
            return (void*)tp->tp_clear;
        case Py_tp_dealloc:
            return (void*)tp->tp_dealloc;
        case Py_tp_del:
            return (void*)tp->tp_del;
        case Py_tp_descr_get:
            return (void*)tp->tp_descr_get;
        case Py_tp_descr_set:
            return (void*)tp->tp_descr_set;
        case Py_tp_doc:
            return (void*)tp->tp_doc;
        case Py_tp_getattr:
            return (void*)tp->tp_getattr;
        case Py_tp_getattro:
            return (void*)tp->tp_getattro;
        case Py_tp_hash:
            return (void*)tp->tp_hash;
        case Py_tp_init:
            return (void*)tp->tp_init;
        case Py_tp_is_gc:
            return (void*)tp->tp_is_gc;
        case Py_tp_iter:
            return (void*)tp->tp_iter;
        case Py_tp_iternext:
            return (void*)tp->tp_iternext;
        case Py_tp_methods:
            return (void*)tp->tp_methods;
        case Py_tp_new:
            return (void*)tp->tp_new;
        case Py_tp_repr:
            return (void*)tp->tp_repr;
        case Py_tp_richcompare:
            return (void*)tp->tp_richcompare;
        case Py_tp_setattr:
            return (void*)tp->tp_setattr;
        case Py_tp_setattro:
            return (void*)tp->tp_setattro;
        case Py_tp_str:
            return (void*)tp->tp_str;
        case Py_tp_traverse:
            return (void*)tp->tp_traverse;
        case Py_tp_members:
            return (void*)tp->tp_members;
        case Py_tp_getset:
            return (void*)tp->tp_getset;
        case Py_tp_free:
            return (void*)tp->tp_free;
        case Py_nb_matrix_multiply:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_matrix_multiply;
        case Py_nb_inplace_matrix_multiply:
            if (!(tp->tp_as_number)) return NULL;
            return (void*)tp->tp_as_number->nb_inplace_matrix_multiply;
        case Py_am_await:
            if (!(tp->tp_as_async)) return NULL;
            return (void*)tp->tp_as_async->am_await;
        case Py_am_aiter:
            if (!(tp->tp_as_async)) return NULL;
            return (void*)tp->tp_as_async->am_aiter;
        case Py_am_anext:
            if (!(tp->tp_as_async)) return NULL;
            return (void*)tp->tp_as_async->am_anext;
        case Py_tp_finalize:
            return (void*)tp->tp_finalize;
        case Py_am_send:
            if (!(tp->tp_as_async)) return NULL;
            return (void*)tp->tp_as_async->am_send;
        case Py_tp_vectorcall:
            return (void*)tp->tp_vectorcall;
        case Py_tp_token:
            if (!(tp->tp_flags & Py_TPFLAGS_HEAPTYPE)) return NULL;
            return (void*)((PyHeapTypeObject*)tp)->ht_token;
        case Py_bf_getbuffer:
            if (!(tp->tp_as_buffer)) return NULL;
            return (void*)tp->tp_as_buffer->bf_getbuffer;
        case Py_bf_releasebuffer:
            if (!(tp->tp_as_buffer)) return NULL;
            return (void*)tp->tp_as_buffer->bf_releasebuffer;
        case Py_mp_ass_subscript:
            if (!(tp->tp_as_mapping)) return NULL;
            return (void*)tp->tp_as_mapping->mp_ass_subscript;
        case Py_mp_length:
            if (!(tp->tp_as_mapping)) return NULL;
            return (void*)tp->tp_as_mapping->mp_length;
    }
    qp_capi_slot_err_bad_slot("PyType_GetSlot", slot_id);
    return NULL;
}

static void
qp_capi_slot_heaptype_apply_field_slot(PyHeapTypeObject *ht, PySlot slot)
{
    switch (slot.sl_id) {
        case Py_mp_subscript:
            ht->as_mapping.mp_subscript = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_absolute:
            ht->as_number.nb_absolute = (unaryfunc)slot.sl_func;
            break;
        case Py_nb_add:
            ht->as_number.nb_add = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_and:
            ht->as_number.nb_and = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_bool:
            ht->as_number.nb_bool = (inquiry)slot.sl_func;
            break;
        case Py_nb_divmod:
            ht->as_number.nb_divmod = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_float:
            ht->as_number.nb_float = (unaryfunc)slot.sl_func;
            break;
        case Py_nb_floor_divide:
            ht->as_number.nb_floor_divide = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_index:
            ht->as_number.nb_index = (unaryfunc)slot.sl_func;
            break;
        case Py_nb_inplace_add:
            ht->as_number.nb_inplace_add = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_inplace_and:
            ht->as_number.nb_inplace_and = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_inplace_floor_divide:
            ht->as_number.nb_inplace_floor_divide = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_inplace_lshift:
            ht->as_number.nb_inplace_lshift = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_inplace_multiply:
            ht->as_number.nb_inplace_multiply = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_inplace_or:
            ht->as_number.nb_inplace_or = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_inplace_power:
            ht->as_number.nb_inplace_power = (ternaryfunc)slot.sl_func;
            break;
        case Py_nb_inplace_remainder:
            ht->as_number.nb_inplace_remainder = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_inplace_rshift:
            ht->as_number.nb_inplace_rshift = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_inplace_subtract:
            ht->as_number.nb_inplace_subtract = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_inplace_true_divide:
            ht->as_number.nb_inplace_true_divide = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_inplace_xor:
            ht->as_number.nb_inplace_xor = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_int:
            ht->as_number.nb_int = (unaryfunc)slot.sl_func;
            break;
        case Py_nb_invert:
            ht->as_number.nb_invert = (unaryfunc)slot.sl_func;
            break;
        case Py_nb_lshift:
            ht->as_number.nb_lshift = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_multiply:
            ht->as_number.nb_multiply = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_negative:
            ht->as_number.nb_negative = (unaryfunc)slot.sl_func;
            break;
        case Py_nb_or:
            ht->as_number.nb_or = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_positive:
            ht->as_number.nb_positive = (unaryfunc)slot.sl_func;
            break;
        case Py_nb_power:
            ht->as_number.nb_power = (ternaryfunc)slot.sl_func;
            break;
        case Py_nb_remainder:
            ht->as_number.nb_remainder = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_rshift:
            ht->as_number.nb_rshift = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_subtract:
            ht->as_number.nb_subtract = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_true_divide:
            ht->as_number.nb_true_divide = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_xor:
            ht->as_number.nb_xor = (binaryfunc)slot.sl_func;
            break;
        case Py_sq_ass_item:
            ht->as_sequence.sq_ass_item = (ssizeobjargproc)slot.sl_func;
            break;
        case Py_sq_concat:
            ht->as_sequence.sq_concat = (binaryfunc)slot.sl_func;
            break;
        case Py_sq_contains:
            ht->as_sequence.sq_contains = (objobjproc)slot.sl_func;
            break;
        case Py_sq_inplace_concat:
            ht->as_sequence.sq_inplace_concat = (binaryfunc)slot.sl_func;
            break;
        case Py_sq_inplace_repeat:
            ht->as_sequence.sq_inplace_repeat = (ssizeargfunc)slot.sl_func;
            break;
        case Py_sq_item:
            ht->as_sequence.sq_item = (ssizeargfunc)slot.sl_func;
            break;
        case Py_sq_length:
            ht->as_sequence.sq_length = (lenfunc)slot.sl_func;
            break;
        case Py_sq_repeat:
            ht->as_sequence.sq_repeat = (ssizeargfunc)slot.sl_func;
            break;
        case Py_tp_alloc:
            ht->ht_type.tp_alloc = (allocfunc)slot.sl_func;
            break;
        case Py_tp_base:
            ht->ht_type.tp_base = slot.sl_ptr;
            break;
        case Py_tp_bases:
            ht->ht_type.tp_bases = slot.sl_ptr;
            break;
        case Py_tp_call:
            ht->ht_type.tp_call = (ternaryfunc)slot.sl_func;
            break;
        case Py_tp_clear:
            ht->ht_type.tp_clear = (inquiry)slot.sl_func;
            break;
        case Py_tp_dealloc:
            ht->ht_type.tp_dealloc = (destructor)slot.sl_func;
            break;
        case Py_tp_del:
            ht->ht_type.tp_del = (destructor)slot.sl_func;
            break;
        case Py_tp_descr_get:
            ht->ht_type.tp_descr_get = (descrgetfunc)slot.sl_func;
            break;
        case Py_tp_descr_set:
            ht->ht_type.tp_descr_set = (descrsetfunc)slot.sl_func;
            break;
        case Py_tp_doc:
            ht->ht_type.tp_doc = slot.sl_ptr;
            break;
        case Py_tp_getattr:
            ht->ht_type.tp_getattr = (getattrfunc)slot.sl_func;
            break;
        case Py_tp_getattro:
            ht->ht_type.tp_getattro = (getattrofunc)slot.sl_func;
            break;
        case Py_tp_hash:
            ht->ht_type.tp_hash = (hashfunc)slot.sl_func;
            break;
        case Py_tp_init:
            ht->ht_type.tp_init = (initproc)slot.sl_func;
            break;
        case Py_tp_is_gc:
            ht->ht_type.tp_is_gc = (inquiry)slot.sl_func;
            break;
        case Py_tp_iter:
            ht->ht_type.tp_iter = (getiterfunc)slot.sl_func;
            break;
        case Py_tp_iternext:
            ht->ht_type.tp_iternext = (iternextfunc)slot.sl_func;
            break;
        case Py_tp_methods:
            ht->ht_type.tp_methods = slot.sl_ptr;
            break;
        case Py_tp_new:
            ht->ht_type.tp_new = (newfunc)slot.sl_func;
            break;
        case Py_tp_repr:
            ht->ht_type.tp_repr = (reprfunc)slot.sl_func;
            break;
        case Py_tp_richcompare:
            ht->ht_type.tp_richcompare = (richcmpfunc)slot.sl_func;
            break;
        case Py_tp_setattr:
            ht->ht_type.tp_setattr = (setattrfunc)slot.sl_func;
            break;
        case Py_tp_setattro:
            ht->ht_type.tp_setattro = (setattrofunc)slot.sl_func;
            break;
        case Py_tp_str:
            ht->ht_type.tp_str = (reprfunc)slot.sl_func;
            break;
        case Py_tp_traverse:
            ht->ht_type.tp_traverse = (traverseproc)slot.sl_func;
            break;
        case Py_tp_members:
            ht->ht_type.tp_members = slot.sl_ptr;
            break;
        case Py_tp_getset:
            ht->ht_type.tp_getset = slot.sl_ptr;
            break;
        case Py_tp_free:
            ht->ht_type.tp_free = (freefunc)slot.sl_func;
            break;
        case Py_nb_matrix_multiply:
            ht->as_number.nb_matrix_multiply = (binaryfunc)slot.sl_func;
            break;
        case Py_nb_inplace_matrix_multiply:
            ht->as_number.nb_inplace_matrix_multiply = (binaryfunc)slot.sl_func;
            break;
        case Py_am_await:
            ht->as_async.am_await = (unaryfunc)slot.sl_func;
            break;
        case Py_am_aiter:
            ht->as_async.am_aiter = (unaryfunc)slot.sl_func;
            break;
        case Py_am_anext:
            ht->as_async.am_anext = (unaryfunc)slot.sl_func;
            break;
        case Py_tp_finalize:
            ht->ht_type.tp_finalize = (destructor)slot.sl_func;
            break;
        case Py_am_send:
            ht->as_async.am_send = (sendfunc)slot.sl_func;
            break;
        case Py_tp_vectorcall:
            ht->ht_type.tp_vectorcall = (vectorcallfunc)slot.sl_func;
            break;
        case Py_bf_getbuffer:
            ht->as_buffer.bf_getbuffer = (getbufferproc)slot.sl_func;
            break;
        case Py_bf_releasebuffer:
            ht->as_buffer.bf_releasebuffer = (releasebufferproc)slot.sl_func;
            break;
        case Py_mp_ass_subscript:
            ht->as_mapping.mp_ass_subscript = (objobjargproc)slot.sl_func;
            break;
        case Py_mp_length:
            ht->as_mapping.mp_length = (lenfunc)slot.sl_func;
            break;
    }
}

static QPCapiSlotDType
qp_capi_slot_get_dtype(uint16_t slot_id)
{
    switch (slot_id) {
        case Py_slot_end: return QP_CAPI_SLOT_DTYPE_VOID;
        case Py_mp_subscript: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_absolute: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_add: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_and: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_bool: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_divmod: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_float: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_floor_divide: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_index: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_inplace_add: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_inplace_and: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_inplace_floor_divide: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_inplace_lshift: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_inplace_multiply: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_inplace_or: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_inplace_power: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_inplace_remainder: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_inplace_rshift: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_inplace_subtract: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_inplace_true_divide: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_inplace_xor: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_int: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_invert: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_lshift: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_multiply: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_negative: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_or: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_positive: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_power: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_remainder: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_rshift: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_subtract: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_true_divide: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_xor: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_sq_ass_item: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_sq_concat: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_sq_contains: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_sq_inplace_concat: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_sq_inplace_repeat: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_sq_item: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_sq_length: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_sq_repeat: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_alloc: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_base: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_tp_bases: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_tp_call: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_clear: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_dealloc: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_del: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_descr_get: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_descr_set: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_doc: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_tp_getattr: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_getattro: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_hash: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_init: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_is_gc: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_iter: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_iternext: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_methods: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_tp_new: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_repr: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_richcompare: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_setattr: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_setattro: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_str: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_traverse: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_members: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_tp_getset: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_tp_free: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_matrix_multiply: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_nb_inplace_matrix_multiply: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_am_await: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_am_aiter: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_am_anext: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_finalize: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_am_send: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_vectorcall: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_token: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_mod_create: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_mod_exec: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_mod_multiple_interpreters: return QP_CAPI_SLOT_DTYPE_UINT64;
        case Py_mod_gil: return QP_CAPI_SLOT_DTYPE_UINT64;
        case Py_bf_getbuffer: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_bf_releasebuffer: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_mp_ass_subscript: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_mp_length: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_slot_subslots: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_tp_slots: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_mod_slots: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_tp_name: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_tp_basicsize: return QP_CAPI_SLOT_DTYPE_SIZE;
        case Py_tp_extra_basicsize: return QP_CAPI_SLOT_DTYPE_SIZE;
        case Py_tp_itemsize: return QP_CAPI_SLOT_DTYPE_SIZE;
        case Py_tp_flags: return QP_CAPI_SLOT_DTYPE_UINT64;
        case Py_mod_name: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_mod_doc: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_mod_state_size: return QP_CAPI_SLOT_DTYPE_SIZE;
        case Py_mod_methods: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_mod_state_traverse: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_mod_state_clear: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_mod_state_free: return QP_CAPI_SLOT_DTYPE_FUNC;
        case Py_tp_metaclass: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_tp_module: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_mod_abi: return QP_CAPI_SLOT_DTYPE_PTR;
        case Py_mod_token: return QP_CAPI_SLOT_DTYPE_PTR;
        default: return QP_CAPI_SLOT_DTYPE_VOID;
    }
}

static QPCapiSlotProblem
qp_capi_slot_get_duplicate_handling(uint16_t slot_id)
{
    switch (slot_id) {
        case Py_mp_subscript:
        case Py_nb_absolute:
        case Py_nb_add:
        case Py_nb_and:
        case Py_nb_bool:
        case Py_nb_divmod:
        case Py_nb_float:
        case Py_nb_floor_divide:
        case Py_nb_index:
        case Py_nb_inplace_add:
        case Py_nb_inplace_and:
        case Py_nb_inplace_floor_divide:
        case Py_nb_inplace_lshift:
        case Py_nb_inplace_multiply:
        case Py_nb_inplace_or:
        case Py_nb_inplace_power:
        case Py_nb_inplace_remainder:
        case Py_nb_inplace_rshift:
        case Py_nb_inplace_subtract:
        case Py_nb_inplace_true_divide:
        case Py_nb_inplace_xor:
        case Py_nb_int:
        case Py_nb_invert:
        case Py_nb_lshift:
        case Py_nb_multiply:
        case Py_nb_negative:
        case Py_nb_or:
        case Py_nb_positive:
        case Py_nb_power:
        case Py_nb_remainder:
        case Py_nb_rshift:
        case Py_nb_subtract:
        case Py_nb_true_divide:
        case Py_nb_xor:
        case Py_sq_ass_item:
        case Py_sq_concat:
        case Py_sq_contains:
        case Py_sq_inplace_concat:
        case Py_sq_inplace_repeat:
        case Py_sq_item:
        case Py_sq_length:
        case Py_sq_repeat:
        case Py_tp_alloc:
        case Py_tp_base:
        case Py_tp_bases:
        case Py_tp_call:
        case Py_tp_clear:
        case Py_tp_dealloc:
        case Py_tp_del:
        case Py_tp_descr_get:
        case Py_tp_descr_set:
        case Py_tp_getattr:
        case Py_tp_getattro:
        case Py_tp_hash:
        case Py_tp_init:
        case Py_tp_is_gc:
        case Py_tp_iter:
        case Py_tp_iternext:
        case Py_tp_methods:
        case Py_tp_new:
        case Py_tp_repr:
        case Py_tp_richcompare:
        case Py_tp_setattr:
        case Py_tp_setattro:
        case Py_tp_str:
        case Py_tp_traverse:
        case Py_tp_getset:
        case Py_tp_free:
        case Py_nb_matrix_multiply:
        case Py_nb_inplace_matrix_multiply:
        case Py_am_await:
        case Py_am_aiter:
        case Py_am_anext:
        case Py_tp_finalize:
        case Py_am_send:
        case Py_tp_vectorcall:
        case Py_tp_token:
        case Py_bf_getbuffer:
        case Py_bf_releasebuffer:
        case Py_mp_ass_subscript:
        case Py_mp_length:
            return QP_CAPI_SLOT_PROBLEM_DEPRECATED;
        case Py_mod_exec:
        case Py_mod_abi:
            return QP_CAPI_SLOT_PROBLEM_ALLOW;
        default:
            return QP_CAPI_SLOT_PROBLEM_REJECT;
    }
}

static QPCapiSlotProblem
qp_capi_slot_get_null_handling(uint16_t slot_id)
{
    switch (slot_id) {
        case Py_slot_end:
        case Py_tp_doc:
        case Py_tp_token:
        case Py_mod_multiple_interpreters:
        case Py_mod_gil:
        case Py_slot_subslots:
        case Py_tp_slots:
        case Py_mod_slots:
        case Py_tp_basicsize:
        case Py_tp_extra_basicsize:
        case Py_tp_itemsize:
        case Py_tp_flags:
        case Py_mod_state_size:
            return QP_CAPI_SLOT_PROBLEM_ALLOW;
        case Py_mp_subscript:
        case Py_nb_absolute:
        case Py_nb_add:
        case Py_nb_and:
        case Py_nb_bool:
        case Py_nb_divmod:
        case Py_nb_float:
        case Py_nb_floor_divide:
        case Py_nb_index:
        case Py_nb_inplace_add:
        case Py_nb_inplace_and:
        case Py_nb_inplace_floor_divide:
        case Py_nb_inplace_lshift:
        case Py_nb_inplace_multiply:
        case Py_nb_inplace_or:
        case Py_nb_inplace_power:
        case Py_nb_inplace_remainder:
        case Py_nb_inplace_rshift:
        case Py_nb_inplace_subtract:
        case Py_nb_inplace_true_divide:
        case Py_nb_inplace_xor:
        case Py_nb_int:
        case Py_nb_invert:
        case Py_nb_lshift:
        case Py_nb_multiply:
        case Py_nb_negative:
        case Py_nb_or:
        case Py_nb_positive:
        case Py_nb_power:
        case Py_nb_remainder:
        case Py_nb_rshift:
        case Py_nb_subtract:
        case Py_nb_true_divide:
        case Py_nb_xor:
        case Py_sq_ass_item:
        case Py_sq_concat:
        case Py_sq_contains:
        case Py_sq_inplace_concat:
        case Py_sq_inplace_repeat:
        case Py_sq_item:
        case Py_sq_length:
        case Py_sq_repeat:
        case Py_tp_alloc:
        case Py_tp_base:
        case Py_tp_bases:
        case Py_tp_call:
        case Py_tp_clear:
        case Py_tp_dealloc:
        case Py_tp_del:
        case Py_tp_descr_get:
        case Py_tp_descr_set:
        case Py_tp_getattr:
        case Py_tp_getattro:
        case Py_tp_hash:
        case Py_tp_init:
        case Py_tp_is_gc:
        case Py_tp_iter:
        case Py_tp_iternext:
        case Py_tp_methods:
        case Py_tp_new:
        case Py_tp_repr:
        case Py_tp_richcompare:
        case Py_tp_setattr:
        case Py_tp_setattro:
        case Py_tp_str:
        case Py_tp_traverse:
        case Py_tp_getset:
        case Py_tp_free:
        case Py_nb_matrix_multiply:
        case Py_nb_inplace_matrix_multiply:
        case Py_am_await:
        case Py_am_aiter:
        case Py_am_anext:
        case Py_tp_finalize:
        case Py_am_send:
        case Py_tp_vectorcall:
        case Py_mod_create:
        case Py_bf_getbuffer:
        case Py_bf_releasebuffer:
        case Py_mp_ass_subscript:
        case Py_mp_length:
            return QP_CAPI_SLOT_PROBLEM_DEPRECATED;
        default:
            return QP_CAPI_SLOT_PROBLEM_REJECT;
    }
}

static bool
qp_capi_slot_get_must_be_static(uint16_t slot_id)
{
    switch (slot_id) {
        case Py_tp_methods: return true;
        case Py_tp_members: return true;
        case Py_tp_getset: return true;
        case Py_mod_methods: return true;
    }
    return false;
}

