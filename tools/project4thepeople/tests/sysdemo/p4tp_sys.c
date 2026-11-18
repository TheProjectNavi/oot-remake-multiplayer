/* The "console" side of the made-up system library: a shared library using
 * Linux system calls, and the C library's threads. The controller reads its button
 * states from p4tp_pad.txt in the current folder, one number per frame. */
#include "p4tp_sys.h"

static long sc(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static long sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

void sys_print(const char *t) { unsigned long n = 0; while (t[n]) n++; sc(1, 1, (long)t, (long)n); }
int sys_file_open(const char *path) { long fd = sc(2, (long)path, 0, 0); return fd < 0 ? -1 : (int)fd; }
long sys_file_read(int h, void *buf, unsigned long n) { return sc(0, h, (long)buf, (long)n); }
void sys_file_close(int h) { sc(3, h, 0, 0); }
void *sys_alloc(unsigned long n) { return (void *)sc6(9, 0, (long)n, 3, 0x22, -1, 0); }

static int frame;
static unsigned pad[64];
static int pad_frames = -1;
static void load_pad(void) {
    pad_frames = 0;
    int fd = sys_file_open("p4tp_pad.txt");
    if (fd < 0) return;
    char buf[512];
    long n = sys_file_read(fd, buf, sizeof buf);
    sys_file_close(fd);
    unsigned v = 0; int any = 0;
    for (long i = 0; i < n && pad_frames < 64; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') { v = v * 10 + (unsigned)(buf[i] - '0'); any = 1; }
        else if (any) { pad[pad_frames++] = v; v = 0; any = 0; }
    }
    if (any && pad_frames < 64) pad[pad_frames++] = v;
}
unsigned sys_pad_read(int port) {
    if (pad_frames < 0) load_pad();
    if (port != 0 || frame >= pad_frames) return 0;
    return pad[frame];
}
void sys_frame_end(void) { frame++; }
unsigned long sys_time_ms(void) {
    long ts[2];
    sc(228, 1, (long)ts, 0);  /* clock_gettime(CLOCK_MONOTONIC) */
    return (unsigned long)ts[0] * 1000ul + (unsigned long)ts[1] / 1000000ul;
}
int sys_extra(int x) { return x * 2; }

#include <pthread.h>

typedef struct { void (*fn)(void *); void *arg; } start_t;
static start_t starts[256];
static pthread_t ids[256];
static int next_handle = 1;

static void *trampoline(void *p) {
    start_t *s = p;
    s->fn(s->arg);
    return 0;
}

long sys_thread_create(void (*fn)(void *), void *arg) {
    int h = __atomic_fetch_add(&next_handle, 1, __ATOMIC_SEQ_CST);
    if (h >= 256) return 0;
    starts[h].fn = fn;
    starts[h].arg = arg;
    return pthread_create(&ids[h], 0, trampoline, &starts[h]) == 0 ? h : 0;
}

int sys_thread_join(long h) {
    if (h <= 0 || h >= 256) return -1;
    return pthread_join(ids[h], 0);
}
