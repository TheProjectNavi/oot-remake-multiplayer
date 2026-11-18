"""PlayStation (PS4, and as far as is known PS5) dynamic linking information.

A decrypted PlayStation program keeps its import and export tables in the
PT_SCE_DYNLIBDATA segment. The PT_DYNAMIC table uses Sony's own DT_SCE_*
tags; their pointers are offsets into the PT_SCE_DYNLIBDATA bytes.

Imported symbols are named "NID#L#M": the NID is an 11-character hash of the
function's real name, L and M say which library and module it comes from.

Facts used here (constants, layouts, the NID hash) are the ones the
open-source PS4 emulator shadPS4 uses (src/core/loader/elf.h,
src/core/module.cpp and module.h, read in November 2026); no code was
copied. The NID hash was checked against IDs that appear in public PS4
homebrew code (for example __stack_chk_fail is Ou3iL1abvng). Not checked on
a PS5 file.
"""

import hashlib
import struct
from dataclasses import dataclass, field

NID_SALT = bytes([0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                  0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30])
CODES = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-"

DT_NULL, DT_NEEDED = 0, 1
DT_SCE_FINGERPRINT = 0x61000007
DT_SCE_ORIGINAL_FILENAME = 0x61000009
DT_SCE_MODULE_INFO = 0x6100000D
DT_SCE_NEEDED_MODULE = 0x6100000F
DT_SCE_MODULE_ATTR = 0x61000011
DT_SCE_EXPORT_LIB = 0x61000013
DT_SCE_IMPORT_LIB = 0x61000015
DT_SCE_IMPORT_LIB_ATTR = 0x61000019
DT_SCE_HASH = 0x61000025
DT_SCE_PLTGOT = 0x61000027
DT_SCE_JMPREL = 0x61000029
DT_SCE_PLTREL = 0x6100002B
DT_SCE_PLTRELSZ = 0x6100002D
DT_SCE_RELA = 0x6100002F
DT_SCE_RELASZ = 0x61000031
DT_SCE_RELAENT = 0x61000033
DT_SCE_STRTAB = 0x61000035
DT_SCE_STRSZ = 0x61000037
DT_SCE_SYMTAB = 0x61000039
DT_SCE_SYMENT = 0x6100003B
DT_SCE_HASHSZ = 0x6100003D
DT_SCE_SYMTABSZ = 0x6100003F

PT_DYNAMIC = 2
PT_SCE_DYNLIBDATA = 0x61000000


def nid(name):
    """The 11-character NID of a symbol name."""
    digest = struct.unpack("<Q", hashlib.sha1(name.encode() + NID_SALT).digest()[:8])[0]
    out = "".join(CODES[(digest >> (58 - i * 6)) & 0x3F] for i in range(10))
    return out + CODES[(digest & 0xF) * 4]


def encode_id(n):
    """A library or module number as written after '#' in symbol names."""
    if n < 0x40:
        return CODES[n]
    if n < 0x1000:
        return CODES[(n >> 6) & 0x3F] + CODES[n & 0x3F]
    return CODES[(n >> 12) & 0x3F] + CODES[(n >> 6) & 0x3F] + CODES[n & 0x3F]


@dataclass
class Module:
    name: str
    id: int
    version: str


@dataclass
class Library:
    name: str
    id: int
    version: int


@dataclass
class SceSymbol:
    raw: str           # "NID#L#M" as stored
    nid: str
    library: str       # library name ("" when unknown)
    module: str
    is_function: bool
    defined: bool
    value: int


@dataclass
class SceInfo:
    modules: list = field(default_factory=list)       # needed modules
    imports: list = field(default_factory=list)       # import libraries
    exports: list = field(default_factory=list)       # export libraries
    symbols: list = field(default_factory=list)       # SceSymbol, by symbol index
    relocs: list = field(default_factory=list)        # (offset, type, symbol index, addend)
    filename: str = ""
    problems: list = field(default_factory=list)


def read(elf):
    """Read the PlayStation dynamic tables of `elf`, or None if it has none."""
    dyn = next((s for s in elf.segments if s.type == PT_DYNAMIC), None)
    lib = next((s for s in elf.segments if s.type == PT_SCE_DYNLIBDATA), None)
    if dyn is None or lib is None:
        return None
    data = elf.data
    blob = data[lib.offset:lib.offset + lib.filesz]
    info = SceInfo()
    tags = []
    for off in range(dyn.offset, dyn.offset + dyn.filesz - 15, 16):
        tag, val = struct.unpack_from("<qQ", data, off)
        if tag == DT_NULL:
            break
        tags.append((tag & 0xFFFFFFFFFFFFFFFF, val))
    single = {}
    for tag, val in tags:
        single.setdefault(tag, val)

    strtab_off = single.get(DT_SCE_STRTAB, 0)
    strtab_size = single.get(DT_SCE_STRSZ, 0)

    def string(off):
        if off >= strtab_size:
            info.problems.append(f"string offset {off:#x} outside the string table")
            return ""
        start = strtab_off + off
        end = blob.find(b"\0", start, strtab_off + strtab_size)
        return blob[start:end if end >= 0 else strtab_off + strtab_size].decode("utf-8", "replace")

    for tag, val in tags:
        if tag in (DT_SCE_NEEDED_MODULE, DT_SCE_MODULE_INFO):
            m = Module(string(val & 0xFFFFFFFF), val >> 48,
                       f"{(val >> 40) & 0xFF}.{(val >> 32) & 0xFF}")
            if tag == DT_SCE_NEEDED_MODULE:
                info.modules.append(m)
        elif tag in (DT_SCE_IMPORT_LIB, DT_SCE_EXPORT_LIB):
            lb = Library(string(val & 0xFFFFFFFF), val >> 48, (val >> 32) & 0xFFFF)
            (info.imports if tag == DT_SCE_IMPORT_LIB else info.exports).append(lb)
        elif tag == DT_SCE_ORIGINAL_FILENAME:
            info.filename = string(val)

    libs = {encode_id(lb.id): lb.name for lb in info.imports + info.exports}
    mods = {encode_id(m.id): m.name for m in info.modules}
    sym_off = single.get(DT_SCE_SYMTAB, 0)
    sym_size = single.get(DT_SCE_SYMTABSZ, 0)
    for i in range(sym_size // 24):
        if sym_off + i * 24 + 24 > len(blob):
            info.problems.append("symbol table goes past the end of the segment")
            break
        st_name, st_info, _other, st_shndx, st_value, _size = struct.unpack_from("<IBBHQQ", blob, sym_off + i * 24)
        raw = string(st_name) if i else ""
        parts = raw.split("#")
        info.symbols.append(SceSymbol(
            raw, parts[0], libs.get(parts[1], "") if len(parts) == 3 else "",
            mods.get(parts[2], "") if len(parts) == 3 else "",
            (st_info & 0xF) == 2, st_value != 0, st_value))

    for off_tag, size_tag in ((DT_SCE_JMPREL, DT_SCE_PLTRELSZ), (DT_SCE_RELA, DT_SCE_RELASZ)):
        start, size = single.get(off_tag), single.get(size_tag, 0)
        if start is None:
            continue
        for i in range(size // 24):
            if start + i * 24 + 24 > len(blob):
                info.problems.append("relocation table goes past the end of the segment")
                break
            r_offset, r_info, r_addend = struct.unpack_from("<QQq", blob, start + i * 24)
            info.relocs.append((r_offset, r_info & 0xFFFFFFFF, r_info >> 32, r_addend))
    return info


def add_to_elf(elf, info, names):
    """Turn the PlayStation tables into the tool's own relocations, so imports
    and RELATIVE pointers work as for any ELF. An import is named by its real
    name when `names` (NID -> name) knows it, otherwise "NID:<nid>"."""
    from elfparse import Reloc
    for r_offset, r_type, si, addend in info.relocs:
        sym = info.symbols[si] if 0 < si < len(info.symbols) else None
        name = ""
        if sym:
            name = names.get(sym.nid, f"NID:{sym.nid}")
        elf.relocs.append(Reloc(r_offset, r_type, name, addend,
                                sym.value if sym else 0, bool(sym and sym.defined)))


def describe(info, names):
    """Lines for `info`."""
    out = []
    if info.filename:
        out.append(f"original name: {info.filename}")
    out.append("needed modules: " + (", ".join(f"{m.name} ({m.version})" for m in info.modules) or "none"))
    out.append("import libraries: " + (", ".join(lb.name for lb in info.imports) or "none"))
    if info.exports:
        out.append("export libraries: " + ", ".join(lb.name for lb in info.exports))
    imported = [s for s in info.symbols[1:] if not s.defined]
    known = sum(1 for s in imported if s.nid in names)
    out.append(f"imported symbols: {len(imported)}, {known} with a known name")
    for s in imported:
        out.append(f"  {s.nid}  {names.get(s.nid, '?'):<32} {s.library or '?'}")
    for p in info.problems:
        out.append(f"problem: {p}")
    return out
