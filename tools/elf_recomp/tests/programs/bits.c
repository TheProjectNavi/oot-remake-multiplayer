#include "mini.h"
/* CRC-32, rotations, byte swaps, bit fields and a sieve in .bss. */
static u8 sieve[20000];
static u32 crc32(const u8 *p, size_t n) {
    u32 c = 0xffffffffu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1))); }
    return ~c;
}
struct packed { u32 a : 3, b : 7, c : 13, d : 9; };
int main(void) {
    static const char msg[] = "elf to c, one instruction at a time";
    u32 crc = crc32((const u8 *)msg, sizeof msg - 1);
    puts_("crc "); put_x(crc); nl();
    u64 r = 0x0123456789abcdefULL;
    for (int i = 0; i < 70; i++) r = ((r << 13) | (r >> 51)) ^ (u64)i;
    puts_("rot "); put_x(r); nl();
    puts_("bswap "); put_x(__builtin_bswap64(r)); puts_(" "); put_x(__builtin_bswap32((u32)r)); nl();
    volatile struct packed p = { 5, 100, 4000, 300 };
    p.b += 50; p.c ^= 0x155; p.d = p.d - p.a;
    puts_("fields "); put_u(p.a); puts_(" "); put_u(p.b); puts_(" "); put_u(p.c); puts_(" "); put_u(p.d); nl();
    int primes = 0; u64 sum = 0;
    for (u32 i = 2; i < sizeof sieve; i++) {
        if (sieve[i]) continue;
        primes++; sum += i;
        for (u32 j = i * 2; j < sizeof sieve; j += i) sieve[j] = 1;
    }
    puts_("primes "); put_u((u64)primes); puts_(" sum "); put_u(sum); nl();
    return primes & 0x7f;
}
