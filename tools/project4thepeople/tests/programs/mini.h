/* Tiny freestanding support for the test programs: no C library, Linux
 * system calls only, integer code only. */
#ifndef MINI_H
#define MINI_H

typedef unsigned long size_t;
typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned short u16;
typedef short i16;
typedef unsigned char u8;
typedef signed char i8;

int main(void);

__asm__(
    ".text\n"
    ".global _start\n"
    "_start:\n"
    "  xor %ebp, %ebp\n"
    "  and $-16, %rsp\n"
    "  call main\n"
    "  mov %eax, %edi\n"
    "  mov $60, %eax\n"
    "  syscall\n"
    "  hlt\n");

static inline long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

void *memset(void *d, int v, size_t n) {
    volatile u8 *p = d;
    while (n--) *p++ = (u8)v;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    volatile u8 *p = d;
    const u8 *q = s;
    while (n--) *p++ = *q++;
    return d;
}

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void puts_(const char *s) { sys3(1, 1, (long)s, (long)slen(s)); }

static void put_u(u64 v) {
    char b[24]; int i = 23; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    puts_(b + i);
}

static void put_i(i64 v) {
    if (v < 0) { puts_("-"); put_u((u64)0 - (u64)v); } else put_u((u64)v);
}

static void put_x(u64 v) {
    char b[20]; int i = 19; b[i] = 0;
    do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    puts_("0x"); puts_(b + i);
}

static void nl(void) { puts_("\n"); }

#endif
