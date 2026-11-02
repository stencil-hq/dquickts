/*
 * The base snapshot embedded in qpy (python/pythonrun.c, "the base
 * snapshot"; doc/python-plan.md section 1).
 *
 * Compiled twice by the Makefile: without QPY_BASE_FILE for qpy-nobase,
 * the qpy that writes the snapshot at build time (it has none), and with
 * QPY_BASE_FILE, the path of that snapshot as a string literal, for qpy:
 * the file is included in the read-only data with the assembler's
 * .incbin.
 */
#include <stddef.h>
#include <stdint.h>

const uint8_t *qpy_base_snapshot(size_t *psize);

#ifdef QPY_BASE_FILE

#define QPY_STR2(x) #x
#define QPY_STR(x) QPY_STR2(x)
#define QPY_SYM(name) QPY_STR(__USER_LABEL_PREFIX__) #name

__asm__(
#ifdef __APPLE__
    ".const_data\n"
#else
    ".section .rodata\n"
#endif
    ".globl " QPY_SYM(qpy_base_data) "\n"
    ".p2align 4\n"
    QPY_SYM(qpy_base_data) ":\n"
    ".incbin \"" QPY_BASE_FILE "\"\n"
    ".globl " QPY_SYM(qpy_base_data_end) "\n"
    QPY_SYM(qpy_base_data_end) ":\n"
#ifndef __APPLE__
    ".previous\n"
#endif
);

extern const uint8_t qpy_base_data[], qpy_base_data_end[];

const uint8_t *qpy_base_snapshot(size_t *psize)
{
    *psize = (size_t)(qpy_base_data_end - qpy_base_data);
    return qpy_base_data;
}

#else

const uint8_t *qpy_base_snapshot(size_t *psize)
{
    *psize = 0;
    return NULL;
}

#endif
