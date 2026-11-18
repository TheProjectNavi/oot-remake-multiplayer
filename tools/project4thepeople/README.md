# project4thepeople: x86-64 ELF to C

A static recompiler. It reads the machine code of a 64-bit x86 ELF program, writes it out as C, and builds that C into a normal program for this PC. It works like the recompilers behind native PC versions of console games, such as N64Recomp: one C function per function in the program, with the CPU state kept in a struct.

It is an early tool for learning and research. What it does today:

- Reads plain ELF files: segments, sections, symbols. It names PlayStation file types and segments, and refuses signed or encrypted files (a SELF). **It never decrypts anything.** Notes on PS5 files: [PS5.md](PS5.md).
- Finds the functions: from symbols when the file has them; otherwise by following calls and jumps from the entry point, plus code addresses found in data (function pointer tables) and `switch` jump tables.
- Translates the integer part of x86-64: moves, arithmetic and logic at every width with the CPU flags, multiply and divide, shifts and rotates, bit scans, conditional moves and sets, `rep movs`/`stos`, calls, returns, direct and indirect jumps, `syscall`.
- Runs the result with a small runtime (`runtime/`): 4 GiB of guest memory, the program's segments copied in, a start-up stack, calls by address, and a few Linux system calls (`read`, `write`, `exit`, `brk`).

What it does not do yet:

- Floating point and vector instructions (SSE, AVX, x87). They become a `recomp_fail` call: the output builds, and stops with a message if one is reached. `recomp` lists how many there are.
- Dynamic linking (shared libraries, relocations), threads, signals, exceptions, self-modifying code.
- System calls beyond the few above. For a PlayStation program this is the main work: see [PS5.md](PS5.md).

## Use

Needs Python 3.10+, `pip install capstone`, and gcc or clang. The runtime is Linux-only for now.

```sh
python tools/project4thepeople/project4thepeople.py info   program            # what is in the file
python tools/project4thepeople/project4thepeople.py recomp program -o out/    # write C into out/
python tools/project4thepeople/project4thepeople.py build  out/ -o program_pc # compile it with the runtime
./program_pc
```

`out/recomp_data.c` holds the program's own bytes (its code and data). Never commit it or share it for a program you did not write; the repository's [hard rule](../../AGENTS.md#the-hard-rule-read-first) applies.

## How the C looks

```c
void f_401000(Ctx *c) {  /* main */
L_401000: /* push rbx */
    { uint64_t v = c->r[3]; PUSH(c, v); }
L_401001: /* mov ebx, edi */
    { c->r[3] = (uint64_t)((c->r[7] & 0xffffffffull)) & 0xffffffffull; }
L_401003: /* call 0x401100 */
    { PUSH(c, 0x401008ull); f_401100(c); }
...
```

- Every instruction gets a label; jumps inside a function are `goto`s. A jump to another function is a tail call.
- `call` pushes the return address on the guest stack, as the CPU does, so the stack looks the same as on the original.
- An indirect jump is a `switch` over the function's instructions; an indirect call goes through `recomp_dispatch`, a binary search over all functions.
- Flags are computed exactly at each instruction. The C compiler drops the ones nothing reads.

## Tests

```sh
python tools/project4thepeople/tests/test_project4thepeople.py -v
```

The programs in `tests/programs/` are our own small C programs (arithmetic at every width, sorting with function pointers, a jump-table `switch`, CRC and bit tricks). Each is built with gcc and clang at `-O0` to `-Os`, with and without symbols, run, recompiled, run again, and the two runs must print the same text and return the same exit code. All 100 runs pass. The ELF reader has its own tests, including a PlayStation-typed header and refusing signed files. The tests are not in CI yet.

## Next steps

1. SSE2 scalar and vector instructions (every real program uses them for floating point and copies).
2. Dynamic linking: read the relocations and imports, and let the runtime supply the imported functions.
3. A way to replace any function by hand-written C (how recompiled ports patch and extend the game).
4. A Windows runtime.
