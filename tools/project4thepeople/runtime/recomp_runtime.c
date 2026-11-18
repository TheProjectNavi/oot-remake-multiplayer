/* Runtime for C code made by tools/project4thepeople. Linux host for now.
 *
 * System calls: the guest is treated as a Linux x86-64 program, so a few
 * Linux calls are passed to the host with their pointers moved into guest
 * memory. A PlayStation program calls system libraries by NID instead; those
 * would be replaced here, one by one (see ../PS5.md). */
#define _GNU_SOURCE
#include "recomp_runtime.h"
#include "recomp_system.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

uint8_t *recomp_mem;

/* Guest address space: 4 GiB reserved, pages filled in by the kernel when used. */
#define GUEST_SPACE   (1ull << 32)
#define STACK_TOP     0xF0000000ull
#define STACK_SIZE    (8ull << 20)
#define HEAP_START    0xC0000000ull     /* brk */
#define ALLOC_START   0xD0000000ull     /* recomp_guest_alloc */
#define ALLOC_END     0xEE000000ull
/* Imported functions get made-up addresses here, past the stack. A call to
 * one lands in recomp_dispatch, which runs its PC version. */
#define IMPORT_BASE   0xFF000000ull
#define IMPORT_STRIDE 16ull

static uint64_t heap_brk = HEAP_START;
static uint64_t alloc_next = ALLOC_START;

extern const RecompSystemTable *const recomp_system_tables[];
extern const uint64_t recomp_system_table_count;

/* PC version of each import (NULL: none), by import index. */
static void (**import_fns)(Ctx *);
static uint8_t *import_reported;

uint64_t recomp_guest_alloc(uint64_t size) {
    uint64_t at = (alloc_next + 15) & ~15ull;
    if (size > ALLOC_END - at) recomp_fail("out of guest memory for system allocations", size);
    alloc_next = at + size;
    memset(recomp_mem + at, 0, size);
    return at;
}

static void (*find_system_fn(const char *name))(Ctx *) {
    for (uint64_t t = 0; t < recomp_system_table_count; t++) {
        const RecompSystemTable *tab = recomp_system_tables[t];
        for (uint64_t i = 0; i < tab->count; i++)
            if (strcmp(tab->fns[i].name, name) == 0) return tab->fns[i].fn;
    }
    return NULL;
}

static void bind_imports(void) {
    import_fns = calloc(recomp_import_count + 1, sizeof *import_fns);
    import_reported = calloc(recomp_import_count + 1, 1);
    for (uint64_t i = 0; i < recomp_import_count; i++)
        import_fns[i] = find_system_fn(recomp_import_names[i]);
    for (uint64_t i = 0; i < recomp_import_slot_count; i++) {
        const RecompImportSlot *s = &recomp_import_slots[i];
        WR64(s->slot, IMPORT_BASE + s->import_index * IMPORT_STRIDE + (uint64_t)s->addend);
    }
}

static int call_import(Ctx *c, uint64_t addr) {
    if (addr < IMPORT_BASE) return 0;
    uint64_t i = (addr - IMPORT_BASE) / IMPORT_STRIDE;
    if (i >= recomp_import_count || (addr - IMPORT_BASE) % IMPORT_STRIDE) return 0;
    if (import_fns[i]) {
        import_fns[i](c);
    } else {
        if (!import_reported[i]) {
            import_reported[i] = 1;
            fflush(stdout);
            fprintf(stderr, "project4thepeople: missing system function: %s\n", recomp_import_names[i]);
            const char *strict = getenv("P4TP_STRICT");
            if (strict && strict[0] == '1') exit(126);
        }
        c->r[RAX] = 0;
    }
    c->r[RSP] += 8;  /* the return address the call pushed, as `ret` would */
    return 1;
}

void recomp_fail(const char *what, uint64_t addr) {
    fflush(stdout);
    fprintf(stderr, "project4thepeople: %s at %#llx\n", what, (unsigned long long)addr);
    exit(127);
}

void recomp_dispatch(Ctx *c, uint64_t addr) {
    if (call_import(c, addr)) return;
    uint64_t lo = 0, hi = recomp_function_count;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (recomp_functions[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo < recomp_function_count && recomp_functions[lo].addr == addr) {
        recomp_functions[lo].fn(c);
        return;
    }
    recomp_fail("call to an address that is not a recompiled function", addr);
}

void recomp_mul1(Ctx *c, uint64_t src, int bits, int is_signed) {
    uint64_t a = c->r[RAX] & MASK(bits);
    uint64_t lo, hi;
    if (is_signed) {
        __int128 p = (__int128)(int64_t)SEXT(a, bits) * (__int128)(int64_t)SEXT(src, bits);
        lo = (uint64_t)p & MASK(bits);
        hi = (uint64_t)(p >> bits) & MASK(bits);
        c->cf = c->of = (__int128)(int64_t)SEXT(lo, bits) != p;
    } else {
        unsigned __int128 p = (unsigned __int128)a * src;
        lo = (uint64_t)p & MASK(bits);
        hi = (uint64_t)(p >> bits) & MASK(bits);
        c->cf = c->of = hi != 0;
    }
    switch (bits) {
    case 8:  c->r[RAX] = (c->r[RAX] & ~0xFFFFull) | lo | (hi << 8); break;
    case 16: c->r[RAX] = (c->r[RAX] & ~0xFFFFull) | lo; c->r[RDX] = (c->r[RDX] & ~0xFFFFull) | hi; break;
    case 32: c->r[RAX] = lo; c->r[RDX] = hi; break;
    default: c->r[RAX] = lo; c->r[RDX] = hi; break;
    }
}

void recomp_div1(Ctx *c, uint64_t src, int bits, int is_signed) {
    uint64_t lo, hi;
    if (bits == 8) { lo = c->r[RAX] & 0xFF; hi = (c->r[RAX] >> 8) & 0xFF; }
    else { lo = c->r[RAX] & MASK(bits); hi = c->r[RDX] & MASK(bits); }
    if ((src & MASK(bits)) == 0) recomp_fail("divide by zero", 0);
    uint64_t q, r;
    if (is_signed) {
        __int128 n = (__int128)(((unsigned __int128)hi << bits) | lo);
        if (bits < 64) n = (n << (128 - 2 * bits)) >> (128 - 2 * bits);
        __int128 d = (int64_t)SEXT(src, bits);
        __int128 qq = n / d;
        if (qq > (__int128)(int64_t)(SIGN(bits) - 1) || qq < -(__int128)SIGN(bits))
            recomp_fail("divide overflow", 0);
        q = (uint64_t)qq & MASK(bits);
        r = (uint64_t)(n % d) & MASK(bits);
    } else {
        unsigned __int128 n = ((unsigned __int128)hi << bits) | lo;
        unsigned __int128 qq = n / (src & MASK(bits));
        if (qq > MASK(bits)) recomp_fail("divide overflow", 0);
        q = (uint64_t)qq;
        r = (uint64_t)(n % (src & MASK(bits)));
    }
    switch (bits) {
    case 8:  c->r[RAX] = (c->r[RAX] & ~0xFFFFull) | q | (r << 8); break;
    case 16: c->r[RAX] = (c->r[RAX] & ~0xFFFFull) | q; c->r[RDX] = (c->r[RDX] & ~0xFFFFull) | r; break;
    default: c->r[RAX] = q; c->r[RDX] = r; break;
    }
}

static int64_t ret_errno(long r) { return r < 0 ? -errno : r; }

void recomp_syscall(Ctx *c) {
    uint64_t n = c->r[RAX], a = c->r[RDI], b = c->r[RSI], d = c->r[RDX];
    int64_t ret;
    switch (n) {
    case 0:  ret = ret_errno(read((int)a, recomp_mem + b, d)); break;
    case 1:  fflush(stdout); ret = ret_errno(write((int)a, recomp_mem + b, d)); break;
    case 12: /* brk */
        if (b == 0 && a >= HEAP_START && a < STACK_TOP - STACK_SIZE) heap_brk = a;
        ret = (int64_t)heap_brk;
        break;
    case 39: ret = getpid(); break;
    case 60: case 231: fflush(stdout); _exit((int)a);
    default: ret = -ENOSYS; break;
    }
    c->r[RAX] = (uint64_t)ret;
    c->r[RCX] = 0;  /* the CPU puts the return address here; nothing reads it */
    c->r[R11] = 0;
}

int main(int argc, char **argv) {
    (void)argc;
    recomp_mem = mmap(NULL, GUEST_SPACE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (recomp_mem == MAP_FAILED) { perror("mmap"); return 127; }

    for (uint64_t i = 0; i < recomp_segment_count; i++) {
        const RecompSegment *s = &recomp_segments[i];
        if (s->vaddr + s->memsz > STACK_TOP - STACK_SIZE) recomp_fail("segment too high", s->vaddr);
        memcpy(recomp_mem + s->vaddr, s->bytes, s->filesz);
    }

    for (uint64_t i = 0; i < recomp_fixup_count; i++)
        WR64(recomp_fixups[i].addr, recomp_fixups[i].value);
    bind_imports();

    /* Start-up stack as Linux builds it: argc, argv[], NULL, envp NULL, auxv AT_NULL. */
    Ctx c;
    memset(&c, 0, sizeof c);
    uint64_t sp = STACK_TOP - 256;
    const char *name = argv[0];
    size_t len = strlen(name) + 1;
    if (len > 200) len = 200;
    memcpy(recomp_mem + sp, name, len);
    recomp_mem[sp + len - 1] = 0;
    uint64_t name_addr = sp;
    sp &= ~15ull;
    uint64_t frame[] = { 1, name_addr, 0, 0, 0, 0 };
    sp -= sizeof frame;
    memcpy(recomp_mem + sp, frame, sizeof frame);
    c.r[RSP] = sp;

    recomp_dispatch(&c, recomp_entry);
    fflush(stdout);
    return (int)c.r[RAX];
}
