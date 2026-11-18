# PS5 programs: what is known and what a PC port would need

This page collects public information about PS5 programs and sets out how far project4thepeople is from running one. Facts marked *(PS4)* come from PS4 work by the open-source emulator projects and the PS4 developer wiki; the PS5 is believed to follow it closely, but this repository has not checked it. Nothing here comes from a PS5 file.

## The hardware

- CPU: 8-core AMD Zen 2, x86-64. The same instruction set as a PC, so translating the CPU code is the easier part. project4thepeople already translates the integer part; floating point and vector instructions (SSE, AVX2) come next.
- GPU: AMD, RDNA 2 family, with its own command format and shader machine code. A PC cannot run it as it is.
- Memory: 16 GB GDDR6 shared by the CPU and GPU (unified memory). A PC has separate memory for each, and the port has to manage the difference.
- Storage: a fast SSD with hardware decompression, which games use through system libraries.

## The program files

- A game's main program is `eboot.bin`; its libraries are `.sprx` files. They are **SELF** files: ELF files that Sony has signed and encrypted. Reading one needs keys from the console. project4thepeople recognises the SELF header and refuses the file. It does not decrypt and never will.
- A decrypted program is a 64-bit ELF with Sony's own file type numbers *(PS4)*: `ET_SCE_EXEC` (0xFE00), `ET_SCE_DYNEXEC` (0xFE10, the usual main program), `ET_SCE_DYNAMIC` (0xFE18, a library). `project4thepeople info` names them.
- Extra segment types *(PS4)*: `PT_SCE_DYNLIBDATA` (the import and export tables), `PT_SCE_PROCPARAM` (process settings), `PT_SCE_MODULE_PARAM`, `PT_SCE_RELRO` (loaded like `PT_LOAD`), `PT_SCE_COMMENT`, `PT_SCE_LIBVERSION`.
- Imports are not by name *(PS4)*. Each imported function has a **NID**: the first 8 bytes of SHA-1(name + a fixed 16-byte salt), written as 11 characters. Since it is a hash, a NID can only be turned back into a name by hashing candidate names; emulators carry lists of known names. `sce.nid()` computes it; the result matches the IDs in public PS4 homebrew code.
- Imported symbol names look like `NID#L#M`: L and M are the library and module numbers from `DT_SCE_IMPORT_LIB` and `DT_SCE_NEEDED_MODULE`, written in the same 64-character alphabet *(PS4)*.

## The operating system

- The system is based on FreeBSD. Games do not make system calls directly: they call Sony's libraries (`libkernel` and many `libSce*` modules: video output, pad input, audio, files, network, save data, trophies).
- The graphics library talks to the GPU with command buffers and compiled shaders. On the PS4 this was "GNM". The PS5 uses a newer library, called "AGC" in public reports.

## What a PC port of a PS5 game would need

| Part | What it means | Where project4thepeople is |
|---|---|---|
| A decrypted program | From the owner's own console. | Out of scope: this tool does not decrypt and does not help to. |
| CPU code to C | Translate every function. | Done for what the Zen 2 CPU has and compilers use: integer, SSE to SSE4.1, AVX, AVX2, FMA3, BMI1/2. Tested with programs built for `-march=znver2`. |
| Threads | Every game runs several. | Atomic instructions, one host thread per guest thread, thread-local storage. Tested with four threads. The PS5's own thread functions (`scePthread*`) still need PC versions. |
| Loader and linker | Read `PT_SCE_DYNLIBDATA`, apply relocations, connect the NID imports. | Done for the PS4 layout and tested on our own programs rewritten in it: modules, libraries, NID-named imports, `RELATIVE` relocations. Not checked on a real PS5 file. |
| System libraries | Write a PC version of every `libkernel` and `libSce*` function the game calls (hundreds of them, few documented): threads, memory, files, input, audio, saves. | The machinery is built and tested: imports are connected to PC versions in `runtime/system/`, and missing ones are reported by name (README, "System functions"). No PC version of a real PlayStation function exists yet: what each one does is not public, and it cannot be checked without a game. |
| Graphics | Translate the GPU command buffers into Vulkan or Direct3D, and every shader from AMD machine code into SPIR-V. | Not started. This is the largest part. |
| Fixing | Run the game, find what breaks, fix it, for months. | Needs a working game build. |

Translating the CPU code is a small part of the whole job. Most of the work is the system libraries and the graphics.

## What exists elsewhere (November 2026, from public reports)

- **shadPS4** is a PS4 emulator: open source, actively developed, it runs many PS4 games. It is the best public reference for the PS4 file format, the NIDs and GNM.
- **PS5 emulators** are at an early stage: SharpEmu and KytyPS5 are reported to boot a few titles to their menus, with none fully playable. Projects such as "prosper" (high-level library replacement) and "AnyPS5" (turning a PS5 program into a native one ahead of time) are described as experimental research.
- Sites offering a "PS5 emulator" download (for example "PCSX4") are scams.

Before copying code from any of these, check its licence ([AGENTS.md](../../AGENTS.md#licence-rules)).

Not yet read for this page: the PS4 and PS5 developer wikis (psdevwiki.com could not be reached from the build environment). Their pages on the SELF format, NIDs and PS5 differences are the next thing to check.

Sources: [shadPS4 loader (elf.h, module.cpp, module.h)](https://github.com/shadps4-emu/shadPS4/tree/main/src/core), [PlayStation 5 emulators (Emulation General Wiki)](https://emulation.gametechwiki.com/index.php/PS5_emulators), [SharpEmu and KytyPS5](https://allthings.how/ps5-emulation-in-what-sharpemu-and-kytyps5-can-run-now/), [AnyPS5](https://tech-insider.org/anyps5-ps5-games-on-pc-without-emulation-2026/), [PS5 emulator boots GTA V's menus](https://www.notebookcheck.net/PS5-emulator-finally-boots-GTA-V-s-menus.1342432.0.html), [prosper](https://alternativeto.net/software/prosper-ps5-/about/).
