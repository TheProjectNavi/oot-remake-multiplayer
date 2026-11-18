#include "mini.h"
/* Floating point: arithmetic, conversions both ways, compares with NaN and
 * infinity, and loops the compiler turns into SSE vector code. */
static volatile double dv[] = { 0.0, -0.0, 1.0, -1.5, 3.25, 1e10, -1e-10, 2.5, 3.5, -2.5,
                                 123456789.75, 9.3e18, -9.3e18, 1e300, 1.0 / 0.0 };
static volatile float fv[] = { 0.0f, 1.0f, -1.5f, 3.25f, 0.1f, 1e10f, -7.75f, 2.5f, 16777217.0f };
#define ND (sizeof dv / sizeof dv[0])
#define NF (sizeof fv / sizeof fv[0])

static u64 h = 0xcbf29ce484222325ULL;
__attribute__((noinline)) static void mix(u64 v) { h = (h ^ v) * 0x100000001b3ULL; }
static u64 bits_d(double d) { u64 u; memcpy(&u, &d, 8); return u; }
static u32 bits_f(float f) { u32 u; memcpy(&u, &f, 4); return u; }

static double sqrt_newton(double x) {
    if (x <= 0) return 0;
    double g = x > 1 ? x / 2 : 1;
    for (int i = 0; i < 60; i++) g = 0.5 * (g + x / g);
    return g;
}

static float va[256], vb[256], vc[256];
static int ia[256], ib[256];
static double da[128];

int main(void) {
    volatile double nan = 0.0 / 0.0;
    for (unsigned i = 0; i < ND; i++) {
        for (unsigned j = 0; j < ND; j++) {
            double a = dv[i], b = dv[j];
            mix(bits_d(a + b)); mix(bits_d(a - b)); mix(bits_d(a * b));
            if (b != 0) mix(bits_d(a / b));
            mix((u64)(a < b) | (u64)(a <= b) << 1 | (u64)(a == b) << 2 | (u64)(a > b) << 3 |
                (u64)(a != b) << 4 | (u64)(a < nan) << 5 | (u64)(nan != b) << 6);
            mix(bits_d(a < b ? a : b)); mix(bits_d(a > b ? a : b));
        }
        double a = dv[i];
        mix((u64)(i64)(a > -9e18 && a < 9e18 ? a : 0));
        mix((u64)(i32)(a > -2e9 && a < 2e9 ? a : 0));
        mix((u64)(a >= 0 && a < 1.8e19 ? (u64)a : 7));
        mix(bits_f((float)a)); mix(bits_d(sqrt_newton(a)));
        mix(bits_d((double)(i64)(i * 1000003) * a));
    }
    for (unsigned i = 0; i < NF; i++) {
        float a = fv[i];
        for (unsigned j = 0; j < NF; j++) {
            float b = fv[j];
            mix(bits_f(a + b)); mix(bits_f(a * b)); mix(bits_f(a - b));
            if (b != 0) mix(bits_f(a / b));
            mix((u64)(a < b));
        }
        mix(bits_d((double)a)); mix((u64)(i64)a); mix((u64)(u32)(i32)(a * 3.0f));
        mix(bits_f((float)(i64)(i * 7919) / 3.0f)); mix(bits_f((float)(u64)(i * 0x9e3779b97f4a7c15ULL)));
        mix(bits_d((double)(u64)(i * 0x9e3779b97f4a7c15ULL)));
    }
    for (int i = 0; i < 256; i++) {
        va[i] = (float)i * 0.5f - 20.0f; vb[i] = (float)(i % 17) * 1.25f; ia[i] = i * 37 - 4000; ib[i] = (i * 13) % 97;
    }
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 256; i++) vc[i] = va[i] * vb[i] + vc[i] * 0.25f;
        for (int i = 0; i < 256; i++) ia[i] = ia[i] * 3 + (ib[i] > 40 ? ib[i] : -ib[i]);
        for (int i = 0; i < 256; i++) vb[i] = vb[i] > va[i] ? vb[i] : va[i];
    }
    float fs = 0; int is = 0;
    for (int i = 0; i < 256; i++) { fs += vc[i]; is ^= ia[i]; mix(bits_f(vc[i])); mix((u64)(u32)ia[i]); }
    for (int i = 0; i < 128; i++) da[i] = (double)ia[i] / 7.0 + (double)vc[i];
    double ds = 0; for (int i = 0; i < 128; i++) ds += da[i] * da[i];
    mix(bits_f(fs)); mix((u64)is); mix(bits_d(ds));
    puts_("sum "); put_i((i64)fs); puts_(" ds "); put_i((i64)ds); nl();
    puts_("hash "); put_x(h); nl();
    return (int)(h & 0x7f);
}
