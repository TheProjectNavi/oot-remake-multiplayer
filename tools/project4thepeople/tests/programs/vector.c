#include "mini.h"
/* AVX2, FMA and SSE4 instructions used on purpose through compiler
 * intrinsics (header-only, no library). Built with -march=znver2, the CPU
 * of the PS5. Every result goes into a hash. */
#ifdef __AVX2__
#include <immintrin.h>

static u64 h = 0xcbf29ce484222325ULL;
static int line;  /* the source line of the current check, for TRACE builds */
static void mix(u64 v) {
    h = (h ^ v) * 0x100000001b3ULL;
#ifdef TRACE
    put_u((u64)line); puts_(" "); put_x(v); nl();
#endif
}
static void mix128(__m128i v) { u64 q[2]; _mm_storeu_si128((__m128i *)q, v); mix(q[0]); mix(q[1]); }
static void mix256(__m256i v) { mix128(_mm256_castsi256_si128(v)); mix128(_mm256_extracti128_si256(v, 1)); }
static void mixps(__m256 v) { mix256(_mm256_castps_si256(v)); }
static void mixpd(__m256d v) { mix256(_mm256_castpd_si256(v)); }
#ifdef TRACE
#define mix(v) (line = __LINE__, mix(v))
#define mix128(v) (line = __LINE__, mix128(v))
#define mix256(v) (line = __LINE__, mix256(v))
#define mixps(v) (line = __LINE__, mixps(v))
#define mixpd(v) (line = __LINE__, mixpd(v))
#endif

static volatile float fin[16] = { 1.5f, -2.25f, 3.0f, 0.1f, -7.0f, 100.5f, 0.0f, -0.0f,
                                  2.5f, -3.5f, 1e7f, -1e-3f, 42.0f, 0.75f, -0.5f, 9.0f };
static volatile int iin[16] = { 1, -2, 3, -4, 100000, -100000, 0x7fffffff, (int)0x80000000,
                                7, 255, -255, 65536, 12, -13, 14, 0x01020304 };

int main(void) {
    float f[16]; int iv[16]; double d[8];
    for (int i = 0; i < 16; i++) { f[i] = fin[i]; iv[i] = iin[i]; }
    for (int i = 0; i < 8; i++) d[i] = (double)f[i] * 1.25 - (double)iv[i];
    __m256 a = _mm256_loadu_ps(f), b = _mm256_loadu_ps(f + 8);
    __m256i x = _mm256_loadu_si256((const __m256i *)iv), y = _mm256_loadu_si256((const __m256i *)(iv + 8));
    __m256d p = _mm256_loadu_pd(d), q = _mm256_loadu_pd(d + 4);

    mixps(_mm256_add_ps(a, b)); mixps(_mm256_mul_ps(a, b)); mixps(_mm256_sub_ps(a, b));
    mixps(_mm256_div_ps(a, _mm256_set1_ps(3.0f))); mixps(_mm256_min_ps(a, b)); mixps(_mm256_max_ps(a, b));
    mixps(_mm256_fmadd_ps(a, b, a)); mixps(_mm256_fmsub_ps(a, b, b)); mixps(_mm256_fnmadd_ps(a, a, b));
    mixpd(_mm256_fmadd_pd(p, q, p)); mixpd(_mm256_fnmsub_pd(p, q, q));
    mix128(_mm_castps_si128(_mm_fmadd_ss(_mm256_castps256_ps128(a), _mm256_castps256_ps128(b), _mm256_castps256_ps128(a))));
    mixps(_mm256_sqrt_ps(_mm256_max_ps(a, _mm256_setzero_ps())));
    mixps(_mm256_round_ps(a, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
    mixps(_mm256_floor_ps(b)); mixps(_mm256_ceil_ps(b));
    mixps(_mm256_cmp_ps(a, b, _CMP_LT_OQ)); mixps(_mm256_cmp_ps(a, b, _CMP_EQ_OQ));
    mix((u64)_mm256_movemask_ps(a)); mix((u64)_mm256_movemask_epi8(x)); mix((u64)_mm256_movemask_pd(p));
    mixps(_mm256_permutevar8x32_ps(a, _mm256_setr_epi32(7, 0, 6, 1, 5, 2, 4, 3)));
    mix256(_mm256_permute4x64_epi64(x, 0x4e)); mixpd(_mm256_permute4x64_pd(p, 0x1b));
    mixps(_mm256_permute2f128_ps(a, b, 0x21)); mix256(_mm256_permute2x128_si256(x, y, 0x83));
    mixps(_mm256_permute_ps(a, 0x1b)); mixpd(_mm256_permute_pd(p, 0x5));
    mixps(_mm256_shuffle_ps(a, b, 0x9c)); mixpd(_mm256_shuffle_pd(p, q, 0x6));
    mixps(_mm256_unpacklo_ps(a, b)); mixps(_mm256_unpackhi_ps(a, b));
    mixps(_mm256_insertf128_ps(a, _mm256_castps256_ps128(b), 1)); mix128(_mm_castps_si128(_mm256_extractf128_ps(b, 1)));
    mixps(_mm256_blend_ps(a, b, 0xa5)); mixps(_mm256_blendv_ps(a, b, a)); mix256(_mm256_blendv_epi8(x, y, x));
    mix256(_mm256_blend_epi32(x, y, 0x3c)); mix256(_mm256_blend_epi16(x, y, 0x96));
    mixps(_mm256_broadcast_ss((const float *)&fin[5])); mix256(_mm256_set1_epi16((short)iv[9]));
    mix256(_mm256_broadcastq_epi64(_mm256_castsi256_si128(y)));
    mix256(_mm256_add_epi32(x, y)); mix256(_mm256_sub_epi16(x, y)); mix256(_mm256_add_epi8(x, y));
    mix256(_mm256_mullo_epi32(x, y)); mix256(_mm256_mul_epi32(x, y)); mix256(_mm256_mul_epu32(x, y));
    mix256(_mm256_mullo_epi16(x, y)); mix256(_mm256_mulhi_epi16(x, y)); mix256(_mm256_madd_epi16(x, y));
    mix256(_mm256_min_epi32(x, y)); mix256(_mm256_max_epu32(x, y)); mix256(_mm256_min_epu8(x, y));
    mix256(_mm256_max_epi16(x, y)); mix256(_mm256_abs_epi32(x)); mix256(_mm256_abs_epi8(y));
    mix256(_mm256_cmpeq_epi32(x, y)); mix256(_mm256_cmpgt_epi32(x, y)); mix256(_mm256_cmpgt_epi64(x, y));
    mix256(_mm256_and_si256(x, y)); mix256(_mm256_andnot_si256(x, y)); mix256(_mm256_or_si256(x, y));
    mix256(_mm256_xor_si256(x, y));
    mix256(_mm256_slli_epi32(x, 5)); mix256(_mm256_srli_epi64(x, 33)); mix256(_mm256_srai_epi16(x, 3));
    mix256(_mm256_sll_epi32(x, _mm_cvtsi32_si128(iv[0] + 3))); mix256(_mm256_srl_epi16(x, _mm_cvtsi32_si128(4)));
    mix256(_mm256_sllv_epi32(x, _mm256_and_si256(y, _mm256_set1_epi32(31))));
    mix256(_mm256_srlv_epi64(x, _mm256_and_si256(y, _mm256_set1_epi64x(63))));
    mix256(_mm256_srav_epi32(x, _mm256_and_si256(y, _mm256_set1_epi32(31))));
    mix256(_mm256_slli_si256(x, 5)); mix256(_mm256_srli_si256(y, 3)); mix256(_mm256_alignr_epi8(x, y, 7));
    mix256(_mm256_shuffle_epi8(x, y)); mix256(_mm256_shuffle_epi32(x, 0x39));
    mix256(_mm256_shufflelo_epi16(x, 0x1b)); mix256(_mm256_shufflehi_epi16(y, 0xb1));
    mix256(_mm256_unpacklo_epi8(x, y)); mix256(_mm256_unpackhi_epi16(x, y)); mix256(_mm256_unpacklo_epi64(x, y));
    mix256(_mm256_packs_epi32(x, y)); mix256(_mm256_packus_epi16(x, y)); mix256(_mm256_packus_epi32(x, y));
    mix256(_mm256_hadd_epi32(x, y)); mixps(_mm256_hadd_ps(a, b)); mixps(_mm256_addsub_ps(a, b));
    mix256(_mm256_cvtepu8_epi32(_mm256_castsi256_si128(x))); mix256(_mm256_cvtepi16_epi64(_mm256_castsi256_si128(y)));
    mix256(_mm256_cvtepi32_epi64(_mm256_castsi256_si128(x)));
    mixpd(_mm256_cvtps_pd(_mm256_castps256_ps128(a))); mix128(_mm_castps_si128(_mm256_cvtpd_ps(p)));
    mixpd(_mm256_cvtepi32_pd(_mm256_castsi256_si128(x))); mix128(_mm256_cvttpd_epi32(p));
    mixps(_mm256_cvtepi32_ps(x)); mix256(_mm256_cvttps_epi32(b)); mix256(_mm256_cvtps_epi32(b));
    mixps(_mm256_dp_ps(a, b, 0xf1)); mixps(_mm256_movehdup_ps(a)); mixpd(_mm256_movedup_pd(p));
    mix((u64)_mm256_testz_si256(x, y) | (u64)_mm256_testc_si256(x, y) << 1);
    __m128i s = _mm256_castsi256_si128(x);
    mix((u64)_mm_extract_epi32(s, 2)); mix((u64)_mm_extract_epi8(s, 13)); mix((u64)_mm_extract_epi64(s, 1));
    mix128(_mm_insert_epi32(s, 77, 1)); mix128(_mm_insert_epi8(s, 9, 5));
    mix128(_mm_castps_si128(_mm_insert_ps(_mm256_castps256_ps128(a), _mm256_castps256_ps128(b), 0x9a)));
    mix((u64)_mm_extract_ps(_mm256_castps256_ps128(b), 3));
    mix128(_mm_castpd_si128(_mm_round_sd(_mm256_castpd256_pd128(p), _mm256_castpd256_pd128(q), _MM_FROUND_TO_ZERO)));
    mix128(_mm_maddubs_epi16(s, _mm256_castsi256_si128(y)));
    unsigned long long w = (unsigned long long)iv[15] * 0x9e3779b97f4a7c15ULL;
    mix(_pdep_u64(w, 0x0f0f00ff00f0ff0fULL)); mix(_pext_u64(w, 0xf0f0ff0f0ff0f0f0ULL));
    mix(_bzhi_u64(w, 37)); mix(_bextr_u64(w, 9, 20)); mix(_blsr_u64(w)); mix(_blsi_u64(w)); mix(_blsmsk_u64(w));
    mix(_andn_u64(w, 0x1234567890abcdefULL)); mix((u64)_lzcnt_u64(w >> 7)); mix((u64)_tzcnt_u64(w << 9));
    unsigned long long hi; mix(_mulx_u64(w, w ^ 0xdeadbeef, &hi)); mix(hi);
    mix(w << (iv[2] & 63)); mix(w >> (iv[4] & 63)); mix((u64)((long long)w >> (iv[0] & 63)));
    mix((w >> 13) | (w << 51));
    puts_("vector "); put_x(h); nl();
    return (int)(h & 0x7f);
}
#else
int main(void) { puts_("needs -march=znver2\n"); return 1; }
#endif
