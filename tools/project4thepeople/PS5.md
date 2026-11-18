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
- Imports are not by name *(PS4)*. Each imported function has a **NID**: a short ID made by hashing its name. Emulators carry lists that turn NIDs back into names such as `sceKernelOpen` or `scePadRead`.

## The operating system

- The system is based on FreeBSD. Games do not make system calls directly: they call Sony's libraries (`libkernel` and many `libSce*` modules: video output, pad input, audio, files, network, save data, trophies).
- The graphics library talks to the GPU with command buffers and compiled shaders. On the PS4 this was "GNM". The PS5 uses a newer library, called "AGC" in public reports.

## What a PC port of a PS5 game would need

| Part | What it means | Where project4thepeople is |
|---|---|---|
| A decrypted program | From the owner's own console. | Out of scope: this tool does not decrypt and does not help to. |
| CPU code to C | Translate every function. | Integer instructions work and are tested. Floating point and vector instructions are not done yet. |
| Loader and linker | Read `PT_SCE_DYNLIBDATA`, apply relocations, connect the NID imports. | Segment types are recognised; imports are not read yet. |
| System libraries | Write a PC version of every `libkernel` and `libSce*` function the game calls (hundreds of them, few documented): threads, memory, files, input, audio, saves. | Not started. Only four Linux system calls exist, for the tests. |
| Graphics | Translate the GPU command buffers into Vulkan or Direct3D, and every shader from AMD machine code into SPIR-V. | Not started. This is the largest part. |
| Fixing | Run the game, find what breaks, fix it, for months. | Needs a working game build. |

Translating the CPU code is a small part of the whole job. Most of the work is the system libraries and the graphics.

## What exists elsewhere (November 2026, from public reports)

- **shadPS4** is a PS4 emulator: open source, actively developed, it runs many PS4 games. It is the best public reference for the PS4 file format, the NIDs and GNM.
- **PS5 emulators** are at an early stage: SharpEmu and KytyPS5 are reported to boot a few titles to their menus, with none fully playable. Projects such as "prosper" (high-level library replacement) and "AnyPS5" (turning a PS5 program into a native one ahead of time) are described as experimental research.
- Sites offering a "PS5 emulator" download (for example "PCSX4") are scams.

Before copying code from any of these, check its licence ([AGENTS.md](../../AGENTS.md#licence-rules)).

Sources: [PlayStation 5 emulators (Emulation General Wiki)](https://emulation.gametechwiki.com/index.php/PS5_emulators), [SharpEmu and KytyPS5](https://allthings.how/ps5-emulation-in-what-sharpemu-and-kytyps5-can-run-now/), [AnyPS5](https://tech-insider.org/anyps5-ps5-games-on-pc-without-emulation-2026/), [PS5 emulator boots GTA V's menus](https://www.notebookcheck.net/PS5-emulator-finally-boots-GTA-V-s-menus.1342432.0.html), [prosper](https://alternativeto.net/software/prosper-ps5-/about/).
