"""Read 64-bit little-endian ELF files: header, program headers, sections, symbols.

Plain ELF only. A signed or encrypted file (a PS4 or PS5 SELF, for example)
is recognised and refused: this tool never decrypts anything.

The PlayStation type and segment numbers below are the ones published by
open-source PS4 projects and the PS4 developer wiki. PS5 files are believed
to use the same numbers; that is not checked here (see PS5.md).
"""

import struct
from dataclasses import dataclass, field

ELF_MAGIC = b"\x7fELF"
# Signed ELF container used by the PS4 and PS5 (eboot.bin, .sprx). Encrypted.
SELF_MAGICS = {
    b"\x4f\x15\x3d\x1d": "PS4/PS5 SELF (signed, encrypted)",
    b"\x54\x14\xf5\xee": "PS5 SELF (signed, encrypted)",
}

EM_X86_64 = 62

ET_NAMES = {
    0: "ET_NONE", 1: "ET_REL", 2: "ET_EXEC", 3: "ET_DYN", 4: "ET_CORE",
    0xFE00: "ET_SCE_EXEC", 0xFE04: "ET_SCE_REPLAY_EXEC", 0xFE0C: "ET_SCE_RELEXEC",
    0xFE10: "ET_SCE_DYNEXEC", 0xFE18: "ET_SCE_DYNAMIC",
}
SCE_TYPES = {0xFE00, 0xFE04, 0xFE0C, 0xFE10, 0xFE18}

PT_NULL, PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE, PT_TLS = 0, 1, 2, 3, 4, 7
PT_NAMES = {
    0: "PT_NULL", 1: "PT_LOAD", 2: "PT_DYNAMIC", 3: "PT_INTERP", 4: "PT_NOTE",
    5: "PT_SHLIB", 6: "PT_PHDR", 7: "PT_TLS",
    0x6474E550: "PT_GNU_EH_FRAME", 0x6474E551: "PT_GNU_STACK",
    0x6474E552: "PT_GNU_RELRO", 0x6474E553: "PT_GNU_PROPERTY",
    0x61000000: "PT_SCE_DYNLIBDATA", 0x61000001: "PT_SCE_PROCPARAM",
    0x61000002: "PT_SCE_MODULE_PARAM", 0x61000010: "PT_SCE_RELRO",
    0x6FFFFF00: "PT_SCE_COMMENT", 0x6FFFFF01: "PT_SCE_LIBVERSION",
}
PT_SCE_RELRO = 0x61000010
PF_X, PF_W, PF_R = 1, 2, 4

SHT_SYMTAB, SHT_STRTAB, SHT_DYNSYM = 2, 3, 11
STT_FUNC = 2


class ElfError(Exception):
    pass


@dataclass
class Segment:
    type: int
    flags: int
    offset: int
    vaddr: int
    filesz: int
    memsz: int
    align: int

    @property
    def name(self):
        return PT_NAMES.get(self.type, hex(self.type))

    @property
    def loadable(self):
        # PT_SCE_RELRO is loaded like PT_LOAD on PlayStation files.
        return self.type in (PT_LOAD, PT_SCE_RELRO) and self.memsz > 0


@dataclass
class Section:
    name: str
    type: int
    flags: int
    addr: int
    offset: int
    size: int
    link: int
    entsize: int


@dataclass
class Symbol:
    name: str
    value: int
    size: int
    type: int
    bind: int
    shndx: int


@dataclass
class Elf:
    data: bytes
    type: int
    machine: int
    entry: int
    segments: list = field(default_factory=list)
    sections: list = field(default_factory=list)
    symbols: list = field(default_factory=list)

    @property
    def type_name(self):
        return ET_NAMES.get(self.type, hex(self.type))

    @property
    def is_playstation(self):
        return self.type in SCE_TYPES or any(
            0x61000000 <= s.type <= 0x61FFFFFF or s.type in (0x6FFFFF00, 0x6FFFFF01)
            for s in self.segments)

    def load_segments(self):
        return [s for s in self.segments if s.loadable]

    def read_vaddr(self, addr, size):
        """Bytes at a virtual address, from the file (zero past filesz)."""
        for s in self.load_segments():
            if s.vaddr <= addr and addr + size <= s.vaddr + s.memsz:
                off = addr - s.vaddr
                out = bytearray(size)
                avail = max(0, min(size, s.filesz - off))
                if avail:
                    out[:avail] = self.data[s.offset + off:s.offset + off + avail]
                return bytes(out)
        raise ElfError(f"address {addr:#x} is not in a loaded segment")

    def exec_ranges(self):
        return [(s.vaddr, s.vaddr + s.filesz) for s in self.load_segments() if s.flags & PF_X]

    def is_code(self, addr):
        return any(lo <= addr < hi for lo, hi in self.exec_ranges())

    def functions(self):
        """Function symbols with an address inside executable code, by address."""
        seen = {}
        for sym in self.symbols:
            if sym.type == STT_FUNC and sym.value and self.is_code(sym.value):
                old = seen.get(sym.value)
                if old is None or (sym.size > old.size) or (old.name.startswith(".") and sym.name):
                    seen[sym.value] = sym
        return [seen[a] for a in sorted(seen)]


def identify(data):
    """Short description of what kind of file this is."""
    head = bytes(data[:4])
    if head in SELF_MAGICS:
        return SELF_MAGICS[head]
    if head == ELF_MAGIC:
        return "ELF"
    return "unknown"


def parse(data):
    data = bytes(data)
    kind = identify(data)
    if kind != "ELF":
        if kind.startswith("PS"):
            raise ElfError(f"this is a {kind} file. Only plain ELF files can be read; "
                           "this tool does not decrypt signed files.")
        raise ElfError("not an ELF file")
    if len(data) < 64:
        raise ElfError("file too short for an ELF header")
    if data[4] != 2 or data[5] != 1:
        raise ElfError("only 64-bit little-endian ELF files are supported")
    (e_type, e_machine, _ver, e_entry, e_phoff, e_shoff, _flags, _ehsize,
     e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from(
        "<HHIQQQIHHHHHH", data, 16)
    elf = Elf(data=data, type=e_type, machine=e_machine, entry=e_entry)

    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if off + 56 > len(data):
            raise ElfError("program header past the end of the file")
        p_type, p_flags, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, p_align = \
            struct.unpack_from("<IIQQQQQQ", data, off)
        elf.segments.append(Segment(p_type, p_flags, p_offset, p_vaddr, p_filesz, p_memsz, p_align))

    raw = []
    if e_shoff and e_shnum:
        for i in range(e_shnum):
            off = e_shoff + i * e_shentsize
            if off + 64 > len(data):
                raise ElfError("section header past the end of the file")
            raw.append(struct.unpack_from("<IIQQQQIIQQ", data, off))
    shstr = raw[e_shstrndx] if raw and e_shstrndx < len(raw) else None

    def cstr(table_off, idx):
        end = data.find(b"\0", table_off + idx)
        return data[table_off + idx:end].decode("utf-8", "replace")

    for r in raw:
        name = cstr(shstr[4], r[0]) if shstr else ""
        elf.sections.append(Section(name, r[1], r[2], r[3], r[4], r[5], r[6], r[9]))

    for sec in elf.sections:
        if sec.type not in (SHT_SYMTAB, SHT_DYNSYM) or not sec.entsize:
            continue
        strtab = elf.sections[sec.link]
        for i in range(sec.size // sec.entsize):
            st_name, st_info, _other, st_shndx, st_value, st_size = struct.unpack_from(
                "<IBBHQQ", data, sec.offset + i * sec.entsize)
            elf.symbols.append(Symbol(cstr(strtab.offset, st_name), st_value, st_size,
                                      st_info & 0xF, st_info >> 4, st_shndx))
    return elf


def load(path):
    with open(path, "rb") as f:
        return parse(f.read())
