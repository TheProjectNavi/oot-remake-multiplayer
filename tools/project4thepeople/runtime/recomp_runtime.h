/* Runtime for C code made by tools/project4thepeople: the CPU state, guest memory,
 * flag helpers, calls by address and system calls.
 *
 * Guest memory is one reserved block: guest address A is host address
 * recomp_mem + A. Needs GCC or Clang (unsigned __int128). */
#ifndef RECOMP_RUNTIME_H
#define RECOMP_RUNTIME_H

#include <stdint.h>
#include <string.h>

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

typedef struct {
    uint64_t r[16];
    uint64_t fs_base, gs_base;
    uint8_t cf, zf, sf, of, pf, df;
} Ctx;

typedef void (*RecompFn)(Ctx *);

typedef struct {
    uint64_t addr;
    RecompFn fn;
} RecompFnEntry;

typedef struct {
    uint64_t vaddr;
    uint64_t memsz;
    const uint8_t *bytes;
    uint64_t filesz;
} RecompSegment;

/* A slot in the program that the loader fills with an imported symbol's
 * address (a GOT entry, or a pointer in data). */
typedef struct {
    uint64_t slot;
    int64_t addend;
    uint32_t import_index;   /* into recomp_import_names */
} RecompImportSlot;

/* Filled in by the generated code. */
extern const char *const recomp_import_names[];
extern const uint64_t recomp_import_count;
extern const RecompImportSlot recomp_import_slots[];
extern const uint64_t recomp_import_slot_count;
extern const RecompFnEntry recomp_functions[];
extern const uint64_t recomp_function_count;
extern const RecompSegment recomp_segments[];
extern const uint64_t recomp_segment_count;
extern const uint64_t recomp_entry;

extern uint8_t *recomp_mem;

void recomp_dispatch(Ctx *c, uint64_t addr);
void recomp_syscall(Ctx *c);
void recomp_fail(const char *what, uint64_t addr);

/* Memory. Unaligned access is fine (memcpy). */
static inline uint8_t  RD8 (uint64_t a) { return recomp_mem[a]; }
static inline uint16_t RD16(uint64_t a) { uint16_t v; memcpy(&v, recomp_mem + a, 2); return v; }
static inline uint32_t RD32(uint64_t a) { uint32_t v; memcpy(&v, recomp_mem + a, 4); return v; }
static inline uint64_t RD64(uint64_t a) { uint64_t v; memcpy(&v, recomp_mem + a, 8); return v; }
static inline void WR8 (uint64_t a, uint64_t v) { recomp_mem[a] = (uint8_t)v; }
static inline void WR16(uint64_t a, uint64_t v) { uint16_t x = (uint16_t)v; memcpy(recomp_mem + a, &x, 2); }
static inline void WR32(uint64_t a, uint64_t v) { uint32_t x = (uint32_t)v; memcpy(recomp_mem + a, &x, 4); }
static inline void WR64(uint64_t a, uint64_t v) { memcpy(recomp_mem + a, &v, 8); }

static inline void PUSH(Ctx *c, uint64_t v) { c->r[RSP] -= 8; WR64(c->r[RSP], v); }
static inline uint64_t POP(Ctx *c) { uint64_t v = RD64(c->r[RSP]); c->r[RSP] += 8; return v; }

/* Flags. bits is the operand width (8, 16, 32 or 64); values are already
 * truncated to it. */
static inline uint64_t MASK(int bits) { return bits == 64 ? ~0ull : ((1ull << bits) - 1); }
static inline uint64_t SIGN(int bits) { return 1ull << (bits - 1); }
static inline uint64_t SEXT(uint64_t v, int bits) {
    return bits == 64 ? v : (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits));
}

static inline void SZP(Ctx *c, uint64_t res, int bits) {
    c->zf = (res & MASK(bits)) == 0;
    c->sf = (res & SIGN(bits)) != 0;
    c->pf = !__builtin_parity((unsigned)(res & 0xFF));
}

static inline uint64_t F_ADD(Ctx *c, uint64_t a, uint64_t b, uint64_t carry, int bits) {
    unsigned __int128 full = (unsigned __int128)a + b + carry;
    uint64_t res = (uint64_t)full & MASK(bits);
    c->cf = bits == 64 ? (uint8_t)(full >> 64) : (uint8_t)((full >> bits) & 1);
    c->of = ((~(a ^ b)) & (a ^ res) & SIGN(bits)) != 0;
    SZP(c, res, bits);
    return res;
}

static inline uint64_t F_SUB(Ctx *c, uint64_t a, uint64_t b, uint64_t borrow, int bits) {
    uint64_t res = (a - b - borrow) & MASK(bits);
    c->cf = (unsigned __int128)a < (unsigned __int128)b + borrow;
    c->of = ((a ^ b) & (a ^ res) & SIGN(bits)) != 0;
    SZP(c, res, bits);
    return res;
}

static inline uint64_t F_LOGIC(Ctx *c, uint64_t res, int bits) {
    res &= MASK(bits);
    c->cf = 0;
    c->of = 0;
    SZP(c, res, bits);
    return res;
}

static inline uint64_t F_INC(Ctx *c, uint64_t a, int bits) {
    uint8_t cf = c->cf; uint64_t r = F_ADD(c, a, 1, 0, bits); c->cf = cf; return r;
}
static inline uint64_t F_DEC(Ctx *c, uint64_t a, int bits) {
    uint8_t cf = c->cf; uint64_t r = F_SUB(c, a, 1, 0, bits); c->cf = cf; return r;
}
static inline uint64_t F_NEG(Ctx *c, uint64_t a, int bits) {
    uint64_t r = F_SUB(c, 0, a, 0, bits);
    c->cf = a != 0;
    return r;
}

/* Shifts: the count is masked like the CPU does; a zero count changes nothing. */
static inline uint64_t F_SHL(Ctx *c, uint64_t a, uint64_t n, int bits) {
    n &= bits == 64 ? 63 : 31;
    if (!n) return a;
    uint64_t res = n >= (uint64_t)bits ? 0 : (a << n) & MASK(bits);
    c->cf = n <= (uint64_t)bits ? (a >> (bits - n)) & 1 : 0;
    c->of = ((res & SIGN(bits)) != 0) ^ c->cf;
    SZP(c, res, bits);
    return res;
}
static inline uint64_t F_SHR(Ctx *c, uint64_t a, uint64_t n, int bits) {
    n &= bits == 64 ? 63 : 31;
    if (!n) return a;
    uint64_t res = n >= (uint64_t)bits ? 0 : a >> n;
    c->cf = n <= (uint64_t)bits ? (a >> (n - 1)) & 1 : 0;
    c->of = (a & SIGN(bits)) != 0;
    SZP(c, res, bits);
    return res;
}
static inline uint64_t F_SAR(Ctx *c, uint64_t a, uint64_t n, int bits) {
    n &= bits == 64 ? 63 : 31;
    if (!n) return a;
    int64_t s = (int64_t)SEXT(a, bits);
    uint64_t sh = n >= (uint64_t)bits ? (uint64_t)bits - 1 : n;
    uint64_t res = (uint64_t)(s >> sh) & MASK(bits);
    c->cf = (uint64_t)(s >> (sh - (n >= (uint64_t)bits ? 0 : 1))) & 1;
    c->of = 0;
    SZP(c, res, bits);
    return res;
}
static inline uint64_t F_ROL(Ctx *c, uint64_t a, uint64_t n, int bits) {
    n &= bits == 64 ? 63 : 31;
    if (!n) return a;
    n %= (uint64_t)bits;
    uint64_t res = n ? ((a << n) | (a >> (bits - n))) & MASK(bits) : a;
    c->cf = res & 1;
    c->of = ((res & SIGN(bits)) != 0) ^ c->cf;
    return res;
}
static inline uint64_t F_ROR(Ctx *c, uint64_t a, uint64_t n, int bits) {
    n &= bits == 64 ? 63 : 31;
    if (!n) return a;
    n %= (uint64_t)bits;
    uint64_t res = n ? ((a >> n) | (a << (bits - n))) & MASK(bits) : a;
    c->cf = (res & SIGN(bits)) != 0;
    c->of = ((res & SIGN(bits)) != 0) ^ (((res << 1) & SIGN(bits)) != 0);
    return res;
}

/* Two- and three-operand IMUL: truncated result, CF=OF when it did not fit. */
static inline uint64_t F_IMUL(Ctx *c, uint64_t a, uint64_t b, int bits) {
    __int128 full = (__int128)(int64_t)SEXT(a, bits) * (__int128)(int64_t)SEXT(b, bits);
    uint64_t res = (uint64_t)full & MASK(bits);
    c->cf = c->of = (__int128)(int64_t)SEXT(res, bits) != full;
    SZP(c, res, bits);
    return res;
}

/* One-operand MUL/IMUL/DIV/IDIV on RDX:RAX (or DX:AX, EDX:EAX, AH:AL). */
void recomp_mul1(Ctx *c, uint64_t src, int bits, int is_signed);
void recomp_div1(Ctx *c, uint64_t src, int bits, int is_signed);

#endif
