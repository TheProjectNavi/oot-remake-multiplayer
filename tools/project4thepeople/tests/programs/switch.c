#include "mini.h"
/* A dense switch (a jump table at -O2), a string state machine, 8-bit registers. */
__attribute__((noinline)) static int op(int code, int a, int b) {
    switch (code) {
    case 0: return a + b;   case 1: return a - b;   case 2: return a * b;
    case 3: return b ? a / b : 0;  case 4: return a & b;  case 5: return a | b;
    case 6: return a ^ b;   case 7: return a << (b & 7);  case 8: return a >> (b & 7);
    case 9: return a > b ? a : b;  case 10: return a < b ? a : b;
    case 11: return -a;     case 12: return ~a;
    default: return 0x55;
    }
}
static const char text[] = "The Quick brown FOX jumps over 13 lazy dogs, 42 times!";
int main(void) {
    int acc = 1;
    for (int i = 0; i < 200; i++) acc = op(i % 15, acc, i) & 0xfffff;
    puts_("acc "); put_i(acc); nl();
    char out[sizeof text]; int words = 0, digits = 0, in_word = 0;
    for (unsigned i = 0; i < sizeof text; i++) {
        char ch = text[i];
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        else if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        out[i] = ch;
        if (ch >= '0' && ch <= '9') digits++;
        int letter = (ch | 32) >= 'a' && (ch | 32) <= 'z';
        if (letter && !in_word) words++;
        in_word = letter;
    }
    puts_(out); nl();
    puts_("words "); put_u((u64)words); puts_(" digits "); put_u((u64)digits); nl();
    return acc & 0xff;
}
