#include "mini.h"
/* Arithmetic at every width, signed and unsigned, with values the compiler
 * cannot fold (they come through a volatile). */
static volatile i64 seeds[] = { 0, 1, -1, 7, -7, 255, -128, 65535, -32768,
    0x7fffffff, -0x7fffffff - 1, 0x123456789abcdefLL, -0x123456789abcdefLL,
    0x7fffffffffffffffLL, 1000003, -999983 };
#define N (sizeof seeds / sizeof seeds[0])

__attribute__((noinline)) static u64 mix(u64 h, u64 v) { return (h ^ v) * 0x100000001b3ULL; }

int main(void) {
    u64 h = 0xcbf29ce484222325ULL;
    for (unsigned i = 0; i < N; i++) {
        for (unsigned j = 0; j < N; j++) {
            i64 a = seeds[i], b = seeds[j];
            h = mix(h, (u64)(a + b)); h = mix(h, (u64)(a - b)); h = mix(h, (u64)(a * b));
            h = mix(h, (u64)(a & b) ^ (u64)(a | b));
            h = mix(h, (u64)((i32)a * (i32)b)); h = mix(h, (u64)(u32)((u32)a + (u32)b));
            h = mix(h, (u64)(i16)((i16)a - (i16)b)); h = mix(h, (u64)(u8)((u8)a * (u8)b));
            if (b != 0 && !(a == (-0x7fffffffffffffffLL - 1) && b == -1)) {
                h = mix(h, (u64)(a / b)); h = mix(h, (u64)(a % b));
                h = mix(h, (u64)a / (u64)b); h = mix(h, (u64)a % (u64)b);
            }
            if ((i32)b != 0 && !((i32)a == (-0x7fffffff - 1) && (i32)b == -1)) {
                h = mix(h, (u64)(i64)((i32)a / (i32)b)); h = mix(h, (u64)((u32)a % (u32)b));
            }
            if ((u16)b) h = mix(h, (u64)((u16)a / (u16)b));
            if ((u8)b) h = mix(h, (u64)((u8)a % (u8)b));
            unsigned s = (unsigned)b & 63;
            h = mix(h, (u64)a << s); h = mix(h, (u64)a >> s); h = mix(h, (u64)(a >> s));
            h = mix(h, (u64)((u32)a >> (s & 31))); h = mix(h, (u64)(i64)((i32)a >> (s & 31)));
            h = mix(h, ((u64)a << s) | ((u64)a >> ((64 - s) & 63)));
            h = mix(h, (u64)(a < b) | (u64)(a <= b) << 1 | (u64)((u64)a < (u64)b) << 2 |
                       (u64)((u32)a > (u32)b) << 3 | (u64)((i8)a > (i8)b) << 4 |
                       (u64)((i16)a >= (i16)b) << 5 | (u64)(a == b) << 6);
            h = mix(h, (u64)(a > b ? a : b)); h = mix(h, (u64)((u32)a < (u32)b ? (u32)a : (u32)b));
            unsigned __int128 p = (unsigned __int128)(u64)a * (u64)b;
            h = mix(h, (u64)(p >> 64));
            __int128 q = (__int128)a * b;
            h = mix(h, (u64)(q >> 64));
            h = mix(h, (u64)__builtin_popcountll((u64)a));
            if (a) { h = mix(h, (u64)__builtin_ctzll((u64)a)); h = mix(h, (u64)__builtin_clzll((u64)a)); }
            h = mix(h, (u64)-a); h = mix(h, (u64)~a);
        }
    }
    puts_("hash "); put_x(h); nl();
    return (int)(h & 0x7f);
}
