/* PC versions of the system functions a recompiled program imports.
 *
 * A program built for a console calls the console's system libraries (text,
 * files, memory, controller, sound, graphics). Those are not on a PC, so each
 * imported function the program uses needs a PC version here: a C function
 * that takes its arguments from the guest registers (System V order: RDI,
 * RSI, RDX, RCX, R8, R9) and puts its result in RAX.
 *
 * Add a function: write it in a file in runtime/system/ and list it in that
 * file's table (RECOMP_SYSTEM_TABLE). An import with no PC version is
 * reported once when it is first called, and returns 0. Set
 * P4TP_STRICT=1 to stop the program there instead. */
#ifndef RECOMP_SYSTEM_H
#define RECOMP_SYSTEM_H

#include "recomp_runtime.h"

typedef struct {
    const char *name;   /* the symbol name; a PlayStation import would use its NID here too */
    void (*fn)(Ctx *c);
} RecompSystemFn;

/* Each file in runtime/system/ makes one table; recomp_runtime.c lists them. */
typedef struct {
    const RecompSystemFn *fns;
    uint64_t count;
} RecompSystemTable;

#define RECOMP_SYSTEM_TABLE(var, ...)                                             \
    static const RecompSystemFn var##_fns[] = { __VA_ARGS__ };                    \
    const RecompSystemTable var = { var##_fns, sizeof var##_fns / sizeof var##_fns[0] }

#define ARG0 (c->r[RDI])
#define ARG1 (c->r[RSI])
#define ARG2 (c->r[RDX])
#define ARG3 (c->r[RCX])
#define ARG4 (c->r[R8])
#define ARG5 (c->r[R9])
#define RETURN(v) do { c->r[RAX] = (uint64_t)(v); return; } while (0)

/* A guest address as a host pointer. */
static inline void *GUEST(uint64_t addr) { return recomp_mem + addr; }

/* Guest memory for system functions that hand memory to the program (zeroed). */
uint64_t recomp_guest_alloc(uint64_t size);

#endif
