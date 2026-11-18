/* Calls a system function that has no PC version yet. */
#include "../programs/mini.h"
#include "p4tp_sys.h"
int main(void) {
    sys_print("before\n");
    int v = sys_extra(21);
    sys_print(v == 42 ? "extra 42\n" : "extra not there\n");
    return 3;
}
