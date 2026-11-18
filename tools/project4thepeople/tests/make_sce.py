"""Test helper: rewrite one of OUR OWN position-independent test programs in
the PlayStation layout, so the PlayStation path of project4thepeople can be
tested end to end without any console file.

What changes:
- e_type becomes ET_SCE_DYNEXEC and the OS ABI FreeBSD;
- the imports, relocations and strings move into a new PT_SCE_DYNLIBDATA
  segment with DT_SCE_* tags, and imported symbols are named "NID#L#M";
- section headers, PT_INTERP and PT_PHDR are dropped (PT_TLS is kept, as
  PlayStation programs have it too) (a shipped program has
  no section names either).
The code and data bytes are not touched.

Usage: python make_sce.py input output
"""

import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import elfparse  # noqa: E402
import sce  # noqa: E402

MODULE_NAME = "libp4tp_sys"
LIBRARY_NAME = "libP4tpSys"
MODULE_ID = 1
LIBRARY_ID = 1


def convert(src, dst):
    with open(src, "rb") as f:
        data = bytearray(f.read())
    elf = elfparse.parse(bytes(data))
    if elf.type != 3:
        raise SystemExit("needs a position-independent program (ET_DYN)")

    # Strings: index 0 is empty.
    strtab = bytearray(b"\0")

    def add_str(text):
        off = len(strtab)
        strtab.extend(text.encode() + b"\0")
        return off

    mod_off = add_str(MODULE_NAME)
    lib_off = add_str(LIBRARY_NAME)

    # Symbols: one per imported name, plus the empty symbol 0.
    imported = sorted({r.symbol for r in elf.relocs if r.symbol and not r.sym_defined})
    sym_index = {}
    symtab = bytearray(24)
    for name in imported:
        raw = f"{sce.nid(name)}#{sce.encode_id(LIBRARY_ID)}#{sce.encode_id(MODULE_ID)}"
        sym_index[name] = len(symtab) // 24
        symtab += struct.pack("<IBBHQQ", add_str(raw), (1 << 4) | 2, 0, 0, 0, 0)

    # Relocations, with addresses as in the file (the program was not moved:
    # parse() moves it, so undo that).
    base = elf.base
    jmprel, rela = bytearray(), bytearray()
    for r in elf.relocs:
        if r.sym_defined:
            continue  # only RELATIVE and imports in these test programs
        si = sym_index.get(r.symbol, 0)
        entry = struct.pack("<QQq", r.offset - base, (si << 32) | r.type, r.addend)
        (jmprel if r.type == elfparse.R_X86_64_JUMP_SLOT else rela).extend(entry)

    def align(buf, n=8):
        while len(buf) % n:
            buf.append(0)

    blob = bytearray()
    offsets = {}
    for key, part in (("str", strtab), ("sym", symtab), ("jmp", jmprel), ("rela", rela)):
        align(blob)
        offsets[key] = len(blob)
        blob.extend(part)
    align(blob)

    dyn = [
        (sce.DT_SCE_NEEDED_MODULE, mod_off | (1 << 40) | (MODULE_ID << 48)),
        (sce.DT_SCE_IMPORT_LIB, lib_off | (1 << 32) | (LIBRARY_ID << 48)),
        (sce.DT_SCE_STRTAB, offsets["str"]), (sce.DT_SCE_STRSZ, len(strtab)),
        (sce.DT_SCE_SYMTAB, offsets["sym"]), (sce.DT_SCE_SYMTABSZ, len(symtab)), (sce.DT_SCE_SYMENT, 24),
        (sce.DT_SCE_JMPREL, offsets["jmp"]), (sce.DT_SCE_PLTRELSZ, len(jmprel)),
        (sce.DT_SCE_RELA, offsets["rela"]), (sce.DT_SCE_RELASZ, len(rela)), (sce.DT_SCE_RELAENT, 24),
        (0, 0),
    ]
    dyn_bytes = b"".join(struct.pack("<qQ", t, v) for t, v in dyn)

    while len(data) % 16:
        data.append(0)
    blob_off = len(data)
    data += blob
    dyn_off = len(data)
    data += dyn_bytes
    while len(data) % 8:
        data.append(0)

    # Program headers: the loadable ones as they were, then the new two.
    e_phoff, e_phnum = struct.unpack_from("<Q", data, 32)[0], struct.unpack_from("<H", data, 56)[0]
    keep = []
    for i in range(e_phnum):
        ph = bytes(data[e_phoff + i * 56:e_phoff + i * 56 + 56])
        p_type = struct.unpack_from("<I", ph)[0]
        if p_type in (elfparse.PT_LOAD, elfparse.PT_TLS, 0x6474E551, 0x6474E552):  # + GNU_STACK, GNU_RELRO
            keep.append(ph)
    keep.append(struct.pack("<IIQQQQQQ", sce.PT_DYNAMIC, 4, dyn_off, 0, 0, len(dyn_bytes), len(dyn_bytes), 8))
    keep.append(struct.pack("<IIQQQQQQ", sce.PT_SCE_DYNLIBDATA, 4, blob_off, 0, 0, len(blob), len(blob), 16))
    ph_off = len(data)
    for ph in keep:
        data += ph

    data[7] = 9  # EI_OSABI: FreeBSD
    struct.pack_into("<H", data, 16, 0xFE10)          # e_type: ET_SCE_DYNEXEC
    struct.pack_into("<Q", data, 32, ph_off)          # e_phoff
    struct.pack_into("<Q", data, 40, 0)               # e_shoff
    struct.pack_into("<H", data, 56, len(keep))       # e_phnum
    struct.pack_into("<HH", data, 60, 0, 0)           # e_shnum, e_shstrndx
    with open(dst, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2])
