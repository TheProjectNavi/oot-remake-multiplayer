/* A tiny "game" that only talks to the outside through p4tp_sys.h. */
#include "../programs/mini.h"
#include "p4tp_sys.h"

static void print_num(long v) {
    char b[24]; int i = 23; int neg = v < 0; unsigned long u = neg ? 0ul - (unsigned long)v : (unsigned long)v;
    b[i] = 0;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    sys_print(b + i);
}

typedef void (*print_fn)(const char *);
static volatile print_fn printer = sys_print;   /* the address of an imported function */

int main(void) {
    sys_print("game start\n");

    unsigned long total = 0;
    for (int k = 1; k <= 3; k++) {
        unsigned char *m = sys_alloc(4096ul * k);
        int zero = 1;
        for (unsigned long i = 0; i < 4096ul * k; i++) { if (m[i]) zero = 0; m[i] = (unsigned char)(i * k); }
        for (unsigned long i = 0; i < 4096ul * k; i++) total += m[i];
        if (!zero) sys_print("alloc not zeroed\n");
    }
    sys_print("alloc sum "); print_num((long)total); sys_print("\n");

    char buf[128];
    int h = sys_file_open("p4tp_data.txt");
    long n = h >= 0 ? sys_file_read(h, buf, sizeof buf - 1) : -1;
    if (h >= 0) sys_file_close(h);
    buf[n > 0 ? n : 0] = 0;
    sys_print("file "); print_num(n); sys_print(": "); sys_print(buf);
    sys_print("missing file "); print_num(sys_file_open("no_such_file.txt")); sys_print("\n");

    int x = 0, y = 0, jumps = 0;
    for (int f = 0; f < 8; f++) {
        unsigned b = sys_pad_read(0);
        if (b & PAD_UP) y--;
        if (b & PAD_DOWN) y++;
        if (b & PAD_LEFT) x--;
        if (b & PAD_RIGHT) x++;
        if (b & PAD_A) jumps++;
        sys_print("frame "); print_num(f); sys_print(" buttons "); print_num(b);
        sys_print(" at "); print_num(x); sys_print(","); print_num(y); sys_print("\n");
        sys_frame_end();
    }
    sys_print("jumps "); print_num(jumps); sys_print("\n");

    unsigned long t0 = sys_time_ms(), t1 = sys_time_ms();
    sys_print(t0 > 0 && t1 >= t0 ? "clock ok\n" : "clock wrong\n");
    printer("called through a pointer\n");
    sys_print("game end\n");
    return x + 10 * y + 100;
}
