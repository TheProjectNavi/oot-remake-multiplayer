/* Threads: shared counters changed with atomic instructions, a spinlock,
 * and a thread-local variable per thread. Only the system library starts
 * and joins the threads. */
#include "../programs/mini.h"
#include "p4tp_sys.h"

#define THREADS 4
#define ROUNDS 200000

static volatile long atomic_total;      /* lock xadd */
static volatile int lock_word;          /* spinlock: xchg and lock cmpxchg */
static long locked_total;               /* only changed while holding the lock */
static volatile unsigned long or_bits;  /* lock or */
static __thread long mine;              /* one per thread */
static __thread int tls_init = 1000;    /* starts with a value in every thread */
static long results[THREADS];

static void spin_lock(void) {
    for (;;) {
        if (__atomic_exchange_n(&lock_word, 1, __ATOMIC_ACQUIRE) == 0) return;
        while (__atomic_load_n(&lock_word, __ATOMIC_RELAXED)) __asm__ volatile("pause");
    }
}
static void spin_unlock(void) { __atomic_store_n(&lock_word, 0, __ATOMIC_RELEASE); }

static void worker(void *arg) {
    long id = (long)arg;
    for (long i = 0; i < ROUNDS; i++) {
        __atomic_fetch_add(&atomic_total, 1, __ATOMIC_SEQ_CST);
        mine += id + 1;
        if ((i & 63) == 0) {
            spin_lock();
            locked_total += id + 1;
            spin_unlock();
        }
    }
    int expected = 0;
    while (!__atomic_compare_exchange_n(&lock_word, &expected, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        expected = 0;
    lock_word = 0;
    __atomic_fetch_or(&or_bits, 1ul << (id * 8), __ATOMIC_SEQ_CST);
    results[id] = mine + tls_init;
}

static void print_num(long v) {
    char b[24]; int i = 23; b[i] = 0;
    unsigned long u = v < 0 ? 0ul - (unsigned long)v : (unsigned long)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) b[--i] = '-';
    sys_print(b + i);
}

int main(void) {
    long handles[THREADS];
    for (long t = 0; t < THREADS; t++) handles[t] = sys_thread_create(worker, (void *)t);
    mine = -5;
    for (long t = 0; t < THREADS; t++) sys_thread_join(handles[t]);
    sys_print("atomic "); print_num(atomic_total); sys_print("\n");
    sys_print("locked "); print_num(locked_total); sys_print("\n");
    sys_print("bits "); print_num((long)or_bits); sys_print("\n");
    for (int t = 0; t < THREADS; t++) { sys_print("thread "); print_num(results[t]); sys_print("\n"); }
    sys_print("main tls "); print_num(mine); sys_print(" "); print_num(tls_init); sys_print("\n");
    int ok = atomic_total == (long)THREADS * ROUNDS;
    return ok ? 0 : 1;
}
