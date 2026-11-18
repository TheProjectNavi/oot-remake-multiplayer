#!/usr/bin/env python3
"""Static recompiler: x86-64 ELF program -> C source -> native program.

  python project4thepeople.py info  program.elf
  python project4thepeople.py recomp program.elf -o out/
  python project4thepeople.py build  out/ -o program_recomp

See README.md for what works and what does not.
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import elfparse  # noqa: E402

RUNTIME_DIR = os.path.join(HERE, "runtime")
FUNCS_PER_FILE = 1500


def cmd_info(args):
    with open(args.elf, "rb") as f:
        data = f.read()
    kind = elfparse.identify(data)
    print(f"file:      {args.elf}")
    print(f"kind:      {kind}")
    if kind != "ELF":
        if kind.startswith("PS"):
            print("This file is signed and encrypted. This tool only reads plain ELF files "
                  "and does not decrypt.")
        return 1
    names = nid_names(args.names)
    elf = elfparse.parse(data, names)
    print(f"type:      {elf.type_name}" + ("  (PlayStation)" if elf.is_playstation else ""))
    print(f"machine:   {elf.machine}" + (" (x86-64)" if elf.machine == elfparse.EM_X86_64 else ""))
    print(f"entry:     {elf.entry:#x}")
    if elf.base:
        print(f"loads at:  {elf.base:#x} (the program can load anywhere; addresses below include this)")
    print("segments:")
    for s in elf.segments:
        fl = ("R" if s.flags & 4 else "-") + ("W" if s.flags & 2 else "-") + ("X" if s.flags & 1 else "-")
        print(f"  {s.name:<20} {fl} vaddr {s.vaddr:#12x} file {s.filesz:#10x} mem {s.memsz:#10x}")
    funcs = elf.functions()
    print(f"functions: {len(funcs)} with symbols" + ("" if funcs else " (stripped: found by following code)"))
    imports = elf.imports()
    if imports:
        print(f"libraries: {', '.join(elf.needed) or '(none named)'}")
        print(f"imports:   {len(imports)}")
        known = system_function_names()
        for imp in imports:
            state = "PC version" if imp.name in known else "no PC version yet"
            kind = "function" if imp.is_function else "value"
            print(f"  {imp.name:<32} {kind:<8} {state}")
    if elf.sce:
        import sce
        print("PlayStation dynamic tables:")
        for line in sce.describe(elf.sce, names):
            print("  " + line)
    if elf.is_playstation:
        print("note:      PlayStation programs call system libraries (graphics, files, input) "
              "that are not in the file. See PS5.md.")
    return 0


def system_function_names():
    """Names listed in the runtime/system tables (the PC versions)."""
    import re
    names = set()
    folder = os.path.join(RUNTIME_DIR, "system")
    for n in os.listdir(folder):
        if n.endswith(".c"):
            with open(os.path.join(folder, n), encoding="utf-8") as f:
                names.update(re.findall(r'\{\s*"([^"]+)"\s*,\s*\w+\s*\}', f.read()))
    return names


def nid_names(names_file=None):
    """NID -> name for every PC system function, plus the names in
    `names_file` (one symbol name per line), so PlayStation imports can be
    matched to PC versions by their real names."""
    import sce
    names = set(system_function_names())
    if names_file:
        with open(names_file, encoding="utf-8") as f:
            names.update(line.strip() for line in f if line.strip() and not line.startswith("#"))
    return {sce.nid(n): n for n in names}


def c_bytes(data):
    out = []
    for i in range(0, len(data), 24):
        out.append("    " + ",".join(str(b) for b in data[i:i + 24]) + ",")
    return "\n".join(out)


def cmd_recomp(args):
    import x86_to_c

    elf = elfparse.load(args.elf, nid_names(args.names))
    if elf.machine != elfparse.EM_X86_64:
        print("only x86-64 programs are supported", file=sys.stderr)
        return 1
    funcs = x86_to_c.discover(elf)
    tr = x86_to_c.Translator(funcs)
    os.makedirs(args.output, exist_ok=True)
    for name in os.listdir(args.output):
        if name.startswith("recomp_") and name.endswith((".c", ".h")):
            os.remove(os.path.join(args.output, name))

    with open(os.path.join(args.output, "recomp_funcs.h"), "w") as f:
        f.write("#include \"recomp_runtime.h\"\n")
        for fn in funcs:
            f.write(f"void {fn.cname}(Ctx *c);\n")

    insn_count = 0
    for part in range(0, len(funcs), FUNCS_PER_FILE):
        chunk = funcs[part:part + FUNCS_PER_FILE]
        with open(os.path.join(args.output, f"recomp_code_{part // FUNCS_PER_FILE}.c"), "w") as f:
            f.write("/* Made by tools/project4thepeople. Do not edit. */\n#include \"recomp_funcs.h\"\n\n")
            for fn in chunk:
                f.write(tr.function_c(fn) + "\n\n")
                insn_count += len(fn.insns)

    with open(os.path.join(args.output, "recomp_data.c"), "w") as f:
        f.write("/* Made by tools/project4thepeople. Holds the program's own bytes: never commit it. */\n")
        f.write("#include \"recomp_funcs.h\"\n\n")
        segs = elf.load_segments()
        for i, s in enumerate(segs):
            f.write(f"static const uint8_t seg{i}[] = {{\n{c_bytes(elf.data[s.offset:s.offset + s.filesz])}\n    0 }};\n")
        f.write("const RecompSegment recomp_segments[] = {\n")
        for i, s in enumerate(segs):
            f.write(f"    {{ {s.vaddr:#x}ull, {s.memsz:#x}ull, seg{i}, {s.filesz:#x}ull }},\n")
        f.write("};\n")
        f.write(f"const uint64_t recomp_segment_count = {len(segs)};\n")
        f.write(f"const uint64_t recomp_entry = {elf.entry:#x}ull;\n\n")
        f.write("const RecompFnEntry recomp_functions[] = {\n")
        for fn in funcs:
            f.write(f"    {{ {fn.addr:#x}ull, {fn.cname} }},\n")
        f.write("};\n")
        f.write(f"const uint64_t recomp_function_count = {len(funcs)};\n\n")
        imports = elf.imports()
        f.write("const char *const recomp_import_names[] = {\n")
        for imp in imports:
            f.write(f'    "{imp.name}",\n')
        f.write("    0 };\n")
        f.write(f"const uint64_t recomp_import_count = {len(imports)};\n")
        f.write("const RecompImportSlot recomp_import_slots[] = {\n")
        n_slots = 0
        for i, imp in enumerate(imports):
            for slot, addend in imp.slots:
                f.write(f"    {{ {slot:#x}ull, {addend}, {i} }},\n")
                n_slots += 1
        f.write("    { 0, 0, 0 } };\n")
        f.write(f"const uint64_t recomp_import_slot_count = {n_slots};\n")
        tls = next((sg for sg in elf.segments if sg.type == elfparse.PT_TLS), None)
        if tls:
            f.write(f"const RecompTls recomp_tls = {{ {tls.vaddr:#x}ull, {tls.filesz:#x}ull, "
                    f"{tls.memsz:#x}ull, {tls.align:#x}ull }};\n")
        else:
            f.write("const RecompTls recomp_tls = { 0, 0, 0, 0 };\n")
        f.write("const RecompFixup recomp_fixups[] = {\n")
        for addr, value in elf.fixups:
            f.write(f"    {{ {addr:#x}ull, {value:#x}ull }},\n")
        f.write("    { 0, 0 } };\n")
        f.write(f"const uint64_t recomp_fixup_count = {len(elf.fixups)};\n")

    print(f"{len(funcs)} functions, {insn_count} instructions -> {args.output}")
    if imports:
        known = system_function_names()
        missing = [i.name for i in imports if i.name not in known]
        print(f"{len(imports)} imports, {len(imports) - len(missing)} with a PC version")
        if missing:
            print("no PC version yet (reported when called): " + ", ".join(missing))
    if tr.unsupported:
        total = sum(tr.unsupported.values())
        print(f"{total} instructions not supported yet (they stop the program if reached):")
        for m, n in sorted(tr.unsupported.items(), key=lambda kv: -kv[1]):
            print(f"  {m:<12} {n}")
    return 0


def cmd_build(args):
    cc = args.cc or os.environ.get("CC") or shutil.which("cc") or "gcc"
    sources = sorted(os.path.join(args.dir, n) for n in os.listdir(args.dir)
                     if n.startswith("recomp_") and n.endswith(".c"))
    sources.append(os.path.join(RUNTIME_DIR, "recomp_runtime.c"))
    system_dir = os.path.join(RUNTIME_DIR, "system")
    sources += sorted(os.path.join(system_dir, n) for n in os.listdir(system_dir) if n.endswith(".c"))
    cmd = [cc, f"-O{args.opt}", "-w", "-fwrapv", "-ffp-contract=off", "-I", RUNTIME_DIR, "-I", args.dir,
           *sources, "-lm", "-pthread", "-o", args.output]
    print(" ".join(cmd))
    return subprocess.run(cmd).returncode


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("info", help="show what is in an ELF file")
    s.add_argument("elf")
    s.add_argument("--names", help="file of symbol names, one per line, to name PlayStation imports")
    s.set_defaults(fn=cmd_info)
    s = sub.add_parser("recomp", help="write C source for an ELF program")
    s.add_argument("elf")
    s.add_argument("--names", help="file of symbol names, one per line, to name PlayStation imports")
    s.add_argument("-o", "--output", required=True, help="folder for the C files")
    s.set_defaults(fn=cmd_recomp)
    s = sub.add_parser("build", help="compile the C source with the runtime")
    s.add_argument("dir")
    s.add_argument("-o", "--output", required=True)
    s.add_argument("--cc")
    s.add_argument("--opt", default="1")
    s.set_defaults(fn=cmd_build)
    args = p.parse_args(argv)
    try:
        return args.fn(args)
    except elfparse.ElfError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
