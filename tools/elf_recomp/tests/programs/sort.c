#include "mini.h"
/* Recursion, function pointers, structs, globals in .data and .bss. */
struct item { i32 key; u16 tag; char name[6]; };
static struct item items[64];
static u32 rng_state = 12345;
static u32 rng(void) { rng_state = rng_state * 1103515245u + 12345u; return (rng_state >> 8) & 0xffff; }

typedef int (*cmp_fn)(const struct item *, const struct item *);
__attribute__((noinline)) static int by_key(const struct item *a, const struct item *b) { return a->key - b->key; }
__attribute__((noinline)) static int by_tag(const struct item *a, const struct item *b) {
    return a->tag != b->tag ? (a->tag < b->tag ? -1 : 1) : by_key(a, b);
}
static void swap(struct item *a, struct item *b) { struct item t = *a; *a = *b; *b = t; }
static void qsort_(struct item *v, int lo, int hi, cmp_fn cmp) {
    if (lo >= hi) return;
    struct item pivot = v[(lo + hi) / 2];
    int i = lo, j = hi;
    while (i <= j) {
        while (cmp(&v[i], &pivot) < 0) i++;
        while (cmp(&v[j], &pivot) > 0) j--;
        if (i <= j) { swap(&v[i], &v[j]); i++; j--; }
    }
    qsort_(v, lo, j, cmp); qsort_(v, i, hi, cmp);
}
static cmp_fn table[] = { by_key, by_tag };

int main(void) {
    for (int i = 0; i < 64; i++) {
        items[i].key = (i32)rng() - 30000;
        items[i].tag = (u16)(rng() % 5);
        for (int k = 0; k < 5; k++) items[i].name[k] = (char)('a' + rng() % 26);
        items[i].name[5] = 0;
    }
    for (int t = 0; t < 2; t++) {
        qsort_(items, 0, 63, table[t]);
        for (int i = 0; i < 64; i += 9) {
            put_i(items[i].key); puts_(" "); put_u(items[i].tag); puts_(" "); puts_(items[i].name); nl();
        }
    }
    int ok = 1;
    for (int i = 1; i < 64; i++) if (by_tag(&items[i - 1], &items[i]) > 0) ok = 0;
    puts_(ok ? "sorted\n" : "NOT sorted\n");
    return ok ? 0 : 1;
}
