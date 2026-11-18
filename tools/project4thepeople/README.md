# project4thepeople: x86-64 ELF to C

A static recompiler. It reads the machine code of a 64-bit x86 ELF program, writes it out as C, and builds that C into a normal program for this PC. It works like the recompilers behind native PC versions of console games, such as N64Recomp: one C function per function in the program, with the CPU state kept in a struct.

It is an early tool for learning and research. What it does today:

- Reads plain ELF files: segments, sections, symbols. It names PlayStation file types and segments, and refuses signed or encrypted files (a SELF). **It never decrypts anything.** Notes on PS5 files: [PS5.md](PS5.md).
- Finds the functions: from symbols when the file has them; otherwise by following calls and jumps from the entry point, plus code addresses found in data (function pointer tables) and `switch` jump tables.
- Translates the integer part of x86-64: moves, arithmetic and logic at every width with the CPU flags, multiply and divide, shifts and rotates, bit scans, conditional moves and sets, `rep movs`/`stos`, calls, returns, direct and indirect jumps, `syscall`.
- Reads the program's imports (the functions it takes from system libraries) and connects each one to a PC version written in C ([below](#system-functions)). An import with no PC version yet is reported by name.
- Runs the result with a small runtime (`runtime/`): 4 GiB of guest memory, the program's segments copied in, a start-up stack, calls by address, and a few Linux system calls (`read`, `write`, `exit`, `brk`).

What it does not do yet:

- Floating point and vector instructions (SSE, AVX, x87). They become a `recomp_fail` call: the output builds, and stops with a message if one is reached. `recomp` lists how many there are.
- Relocating a program that can load at any address (PIE), threads, signals, exceptions, self-modifying code.
- PC versions of any real console library. Only the made-up test library has them. For a PlayStation program this is the main work: see [PS5.md](PS5.md).

## Use

Needs Python 3.10+, `pip install capstone`, and gcc or clang. The runtime is Linux-only for now.

```sh
python tools/project4thepeople/project4thepeople.py info   program            # what is in the file
python tools/project4thepeople/project4thepeople.py recomp program -o out/    # write C into out/
python tools/project4thepeople/project4thepeople.py build  out/ -o program_pc # compile it with the runtime
./program_pc
```

`out/recomp_data.c` holds the program's own bytes (its code and data). Never commit it or share it for a program you did not write; the repository's [hard rule](../../AGENTS.md#the-hard-rule-read-first) applies.

## System functions

A console game does not draw, read the controller or open files by itself: it calls the console's system libraries, which a PC does not have. project4thepeople swaps each of those calls for a PC version:

1. `recomp` reads the program's imports from its relocations (`info` lists them, and says which have a PC version).
2. When the program starts, the runtime gives every import a made-up address and writes it where the program expects the import's address (its GOT and data pointers).
3. A call to one of those addresses runs the PC version: a C function in `runtime/system/` that reads the arguments from the guest registers, does the job with the PC's own means, and puts the result in `RAX`.
4. An import with no PC version prints `project4thepeople: missing system function: <name>` the first time it is called and returns 0, so you can see what to write next. With `P4TP_STRICT=1` the program stops there instead.

A PC version looks like this (`runtime/system/p4tp_sys_pc.c`):

```c
static void pc_sys_file_read(Ctx *c) {
    int h = (int)ARG0;
    if (h < 0 || h >= 64 || !files[h]) RETURN((uint64_t)(int64_t)-1);
    RETURN(fread(GUEST(ARG1), 1, ARG2, files[h]));
}

RECOMP_SYSTEM_TABLE(recomp_system_p4tp,
    { "sys_print", pc_sys_print },
    { "sys_file_read", pc_sys_file_read },
    ...
);
```

To add a library: write a new file in `runtime/system/` with its table, and add the table to `runtime/system/tables.c`. `build` compiles every file in that folder.

The tests use a made-up "console library" (`tests/sysdemo/p4tp_sys.h`: text, files, memory, a controller, a clock) and a small game that only talks to the outside through it. The original game runs with the library; the recompiled game runs with the PC versions; the output must be the same.

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

The programs in `tests/programs/` are our own small C programs (arithmetic at every width, sorting with function pointers, a jump-table `switch`, CRC and bit tricks). Each is built with gcc and clang at `-O0` to `-Os`, with and without symbols, run, recompiled, run again, and the two runs must print the same text and return the same exit code. All 100 runs pass.

The system function tests build the game in `tests/sysdemo/` with gcc and clang at `-O0`, `-O2` and `-Os`: normal, stripped, and with `-fno-plt` (imports called straight through the GOT). All 18 match. Another test calls an import with no PC version and checks the report and `P4TP_STRICT`. The ELF reader has its own tests, including a PlayStation-typed header and refusing signed files. The tests are not in CI yet.

## Next steps

1. SSE2 scalar and vector instructions (every real program uses them for floating point and copies).
2. Programs that load at any address (PIE) and their `R_X86_64_RELATIVE` relocations.
3. Reading PlayStation imports (`PT_SCE_DYNLIBDATA`, NIDs), so `info` can list what a game would need.
4. A way to replace any function by hand-written C (how recompiled ports patch and extend the game).
5. A Windows runtime.
