/* Runtime for C code made by tools/project4thepeople. Linux host for now.
 *
 * System calls: the guest is treated as a Linux x86-64 program, so a few
 * Linux calls are passed to the host with their pointers moved into guest
 * memory. A PlayStation program calls system libraries by NID instead; those
 * would be replaced here, one by one (see ../PS5.md). */
#define _GNU_SOURCE
#include "recomp_runtime.h"

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
#define HEAP_START    0xC0000000ull

static uint64_t heap_brk = HEAP_START;

void recomp_fail(const char *what, uint64_t addr) {
    fflush(stdout);
    fprintf(stderr, "project4thepeople: %s at %#llx\n", what, (unsigned long long)addr);
    exit(127);
}

void recomp_dispatch(Ctx *c, uint64_t addr) {
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
