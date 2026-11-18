"""Turn x86-64 machine code from an ELF file into C.

Each guest function becomes one C function `void f_<address>(Ctx *c)`.
Every instruction gets a label, so jumps inside a function are `goto`s.
A call pushes the return address on the guest stack and calls the C function;
`ret` pops it and returns. Calls by address (function pointers) go through
`recomp_dispatch`, which looks the address up in a sorted table.

Integer instructions only for now (no SSE, x87 or AVX). An instruction the
translator does not know becomes a `recomp_fail(...)` call, so the output
always builds and the report lists what is missing.
"""

from dataclasses import dataclass, field

import capstone
from capstone import x86_const as X

from elfparse import Elf
from sse import SSE, NotSSE

REG64 = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
         "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"] + [f"r{i}d" for i in range(8, 16)]
REG16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"] + [f"r{i}w" for i in range(8, 16)]
REG8 = ["al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil"] + [f"r{i}b" for i in range(8, 16)]
REG8H = {"ah": 0, "ch": 1, "dh": 2, "bh": 3}

REGS = {}
for _i, _n in enumerate(REG64):
    REGS[_n] = (_i, 64, False)
for _i, _n in enumerate(REG32):
    REGS[_n] = (_i, 32, False)
for _i, _n in enumerate(REG16):
    REGS[_n] = (_i, 16, False)
for _i, _n in enumerate(REG8):
    REGS[_n] = (_i, 8, False)
for _n, _i in REG8H.items():
    REGS[_n] = (_i, 8, True)

CONDS = {
    "o": "c->of", "no": "!c->of",
    "b": "c->cf", "c": "c->cf", "nae": "c->cf",
    "ae": "!c->cf", "nb": "!c->cf", "nc": "!c->cf",
    "e": "c->zf", "z": "c->zf", "ne": "!c->zf", "nz": "!c->zf",
    "be": "(c->cf || c->zf)", "na": "(c->cf || c->zf)",
    "a": "(!c->cf && !c->zf)", "nbe": "(!c->cf && !c->zf)",
    "s": "c->sf", "ns": "!c->sf",
    "p": "c->pf", "pe": "c->pf", "np": "!c->pf", "po": "!c->pf",
    "l": "(c->sf != c->of)", "nge": "(c->sf != c->of)",
    "ge": "(c->sf == c->of)", "nl": "(c->sf == c->of)",
    "le": "(c->zf || c->sf != c->of)", "ng": "(c->zf || c->sf != c->of)",
    "g": "(!c->zf && c->sf == c->of)", "nle": "(!c->zf && c->sf == c->of)",
}

TERMINATORS = {"jmp", "ret", "hlt", "ud2", "int3"}
IGNORED = {"nop", "endbr64", "endbr32", "pause", "prefetcht0", "prefetcht1",
           "prefetcht2", "prefetchnta", "prefetchw", "lfence", "mfence", "sfence", "cld"}


def mask(bits):
    return (1 << bits) - 1


def hx(v):
    return f"0x{v:x}ull"


@dataclass
class Function:
    addr: int
    name: str
    insns: dict = field(default_factory=dict)   # address -> capstone insn

    @property
    def cname(self):
        return f"f_{self.addr:x}"


class Unsupported(Exception):
    pass


def make_disassembler():
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    cs.detail = True
    return cs


def disasm_one(cs, elf: Elf, addr):
    try:
        code = elf.read_vaddr(addr, 15)
    except Exception:
        # Near the end of a segment: read what is there.
        for lo, hi in elf.exec_ranges():
            if lo <= addr < hi:
                code = elf.read_vaddr(addr, hi - addr)
                break
        else:
            return None
    for insn in cs.disasm(code, addr, 1):
        return insn
    return None


def branch_target(insn):
    if insn.operands and insn.operands[0].type == X.X86_OP_IMM:
        return insn.operands[0].imm & mask(64)
    return None


def is_jump(insn):
    return insn.group(capstone.CS_GRP_JUMP)


def is_call(insn):
    return insn.group(capstone.CS_GRP_CALL)


def rip_targets(insn):
    """Code addresses a function takes as values (function pointers):
    `lea reg, [rip + x]` and `mov reg, imm`."""
    out = []
    if insn.mnemonic == "lea":
        m = insn.operands[1].mem
        if m.base == X.X86_REG_RIP and m.index == 0:
            out.append((insn.address + insn.size + m.disp) & mask(64))
    elif insn.mnemonic in ("mov", "movabs") and insn.operands[1].type == X.X86_OP_IMM:
        out.append(insn.operands[1].imm & mask(64))
    return out


def data_pointers(elf: Elf):
    """8-byte values in data segments that point into code."""
    import struct
    out = set()
    for s in elf.load_segments():
        if s.flags & 1:
            continue
        raw = elf.data[s.offset:s.offset + s.filesz]
        start = (-s.vaddr) % 8
        for off in range(start, len(raw) - 7, 8):
            v = struct.unpack_from("<Q", raw, off)[0]
            if elf.is_code(v):
                out.add(v)
    return out


def jump_table(elf: Elf, trail, jmp):
    """Targets of a switch jump, from the instructions just before it.

    Two shapes are known:
      jmp qword ptr [idx*8 + TABLE]                          (absolute entries)
      lea B, [rip + TABLE]; movsxd R, dword ptr [B + idx*4]; add R, B; jmp R
                                                             (relative entries; at -O0
                                                             a mov and cdqe instead of movsxd)
    The entry count comes from the nearest `cmp reg, imm` followed by ja/jae;
    without one, entries are read while they point into code (at most 1024).
    """
    import struct
    op = jmp.operands[0]
    table = None
    relative = False
    if op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index and op.mem.scale == 8:
        table = op.mem.disp & mask(64)
    elif op.type == X.X86_OP_REG:
        for insn in reversed(trail[-12:]):
            if insn.mnemonic == "lea" and insn.operands[1].mem.base == X.X86_REG_RIP:
                table = (insn.address + insn.size + insn.operands[1].mem.disp) & mask(64)
                relative = True
                break
        # The entry is read as a signed 32-bit value: movsxd from memory, or
        # (at -O0) a 32-bit mov followed by cdqe.
        if not any((i.mnemonic == "movsxd" and i.operands[1].type == X.X86_OP_MEM) or i.mnemonic == "cdqe"
                   for i in trail[-12:]):
            table = None
    if table is None:
        return []
    count = None
    for k in range(len(trail) - 1, 0, -1):
        cj = trail[k]
        if cj.mnemonic in ("ja", "jae", "jbe", "jb"):
            cmp = trail[k - 1]
            if cmp.mnemonic == "cmp" and len(cmp.operands) == 2 and cmp.operands[1].type == X.X86_OP_IMM:
                n = cmp.operands[1].imm & mask(64)
                count = n + 1 if cj.mnemonic in ("ja", "jbe") else n
            break
    limit = count if count is not None and count <= 4096 else 1024
    out = []
    for i in range(limit):
        try:
            if relative:
                v = (table + struct.unpack("<i", elf.read_vaddr(table + 4 * i, 4))[0]) & mask(64)
            else:
                v = struct.unpack("<Q", elf.read_vaddr(table + 8 * i, 8))[0]
        except Exception:
            break
        if not elf.is_code(v):
            if count is None:
                break
            continue
        out.append(v)
    return out


def discover(elf: Elf):
    """Find functions: symbols first, then call targets and taken addresses.

    Code addresses found in data (function pointer tables) are also made
    functions. A wrong guess only costs a little extra code: jumping into a
    copy of a block behaves the same as jumping to the original."""
    cs = make_disassembler()
    sized = {f.value: f for f in elf.functions() if f.size}
    names = {f.value: f.name for f in elf.functions()}
    pointers = data_pointers(elf)
    known = set(names) | pointers | {elf.entry}
    # Pointers from data go first in the list, so they are done last.
    todo = sorted(pointers - set(names)) + [elf.entry] + sorted(names)
    funcs = {}
    while todo:
        start = todo.pop()
        if start in funcs or not elf.is_code(start):
            continue
        fn = Function(start, names.get(start, ""))
        funcs[start] = fn
        found = []
        sym = sized.get(start)
        if sym:
            # Linear sweep over the symbol's bytes: also finds jump-table targets.
            addr, end = start, start + sym.size
            while addr < end:
                insn = disasm_one(cs, elf, addr)
                if insn is None:
                    break
                fn.insns[addr] = insn
                addr += insn.size
            for insn in fn.insns.values():
                t = branch_target(insn) if (is_call(insn) or is_jump(insn)) else None
                if t is not None and (is_call(insn) or not (start <= t < end)):
                    found.append(t)
                found += rip_targets(insn)
        else:
            # Recursive descent from the start.
            work = [start]
            while work:
                addr = work.pop()
                trail = []
                while addr not in fn.insns:
                    insn = disasm_one(cs, elf, addr)
                    if insn is None:
                        break
                    fn.insns[addr] = insn
                    trail.append(insn)
                    found += rip_targets(insn)
                    t = branch_target(insn)
                    if is_call(insn):
                        if t is not None:
                            found.append(t)
                    elif is_jump(insn) and t is not None:
                        if (t in known or t in funcs) and t != start:
                            found.append(t)          # tail call
                        else:
                            work.append(t)
                    elif is_jump(insn) and insn.mnemonic.split(" ")[-1] == "jmp":
                        work += jump_table(elf, trail, insn)
                    if insn.mnemonic.split(" ")[-1] in TERMINATORS:
                        break
                    addr += insn.size
        todo += [t for t in found if t not in funcs and elf.is_code(t)]
    return [funcs[a] for a in sorted(funcs)]


class Translator:
    def __init__(self, funcs):
        self.funcs = {f.addr: f for f in funcs}
        self.unsupported = {}
        self.sse = SSE(self)

    # ---- operands ----
    def reg_read(self, reg_id, insn):
        name = insn.reg_name(reg_id)
        if name not in REGS:
            raise Unsupported(f"register {name}")
        i, bits, high = REGS[name]
        if high:
            return f"((c->r[{i}] >> 8) & 0xFF)", 8
        if bits == 64:
            return f"c->r[{i}]", 64
        return f"(c->r[{i}] & {hx(mask(bits))})", bits

    def reg_write(self, reg_id, insn, value):
        name = insn.reg_name(reg_id)
        if name not in REGS:
            raise Unsupported(f"register {name}")
        i, bits, high = REGS[name]
        if high:
            return f"c->r[{i}] = (c->r[{i}] & ~0xFF00ull) | (((uint64_t)({value}) & 0xFF) << 8);"
        if bits == 64:
            return f"c->r[{i}] = (uint64_t)({value});"
        if bits == 32:
            return f"c->r[{i}] = (uint64_t)({value}) & 0xffffffffull;"
        return f"c->r[{i}] = (c->r[{i}] & ~{hx(mask(bits))}) | ((uint64_t)({value}) & {hx(mask(bits))});"

    def mem_addr(self, op, insn):
        m = op.mem
        parts = []
        if m.base == X.X86_REG_RIP:
            parts.append(hx((insn.address + insn.size + m.disp) & mask(64)))
        else:
            if m.base:
                parts.append(self.reg_read(m.base, insn)[0])
            if m.index:
                idx = self.reg_read(m.index, insn)[0]
                parts.append(f"{idx} * {m.scale}" if m.scale != 1 else idx)
            if m.disp:
                parts.append(hx(m.disp & mask(64)))
        if m.segment == X.X86_REG_FS:
            parts.append("c->fs_base")
        elif m.segment == X.X86_REG_GS:
            parts.append("c->gs_base")
        expr = " + ".join(parts) if parts else "0"
        if insn.addr_size == 4:
            return f"(uint64_t)(uint32_t)({expr})"
        return f"(uint64_t)({expr})"

    def read(self, op, insn, pre):
        """Expression for an operand's value (zero-extended), and its width."""
        bits = op.size * 8
        if op.type == X.X86_OP_REG:
            return self.reg_read(op.reg, insn)
        if op.type == X.X86_OP_IMM:
            return hx(op.imm & mask(bits)), bits
        if op.type == X.X86_OP_MEM:
            a = self.addr_var(op, insn, pre)
            return f"(uint64_t)RD{bits}({a})", bits
        raise Unsupported("operand type")

    def addr_var(self, op, insn, pre):
        key = ("addr", id(op))
        for k, name in pre.addr_vars:
            if k == key:
                return name
        name = f"a{len(pre.addr_vars)}"
        pre.lines.append(f"uint64_t {name} = {self.mem_addr(op, insn)};")
        pre.addr_vars.append((key, name))
        return name

    def write(self, op, insn, pre, value):
        if op.type == X.X86_OP_REG:
            return self.reg_write(op.reg, insn, value)
        if op.type == X.X86_OP_MEM:
            a = self.addr_var(op, insn, pre)
            return f"WR{op.size * 8}({a}, {value});"
        raise Unsupported("write to operand type")

    # ---- control flow ----
    def goto(self, fn, target):
        if target in fn.insns:
            return f"goto L_{target:x};"
        if target in self.funcs:
            return f"{{ {self.funcs[target].cname}(c); return; }}"
        return f"{{ recomp_dispatch(c, {hx(target)}); return; }}"

    def indirect_jump(self, fn, value):
        cases = " ".join(f"case {hx(a)}: goto L_{a:x};" for a in sorted(fn.insns))
        return (f"switch ((uint64_t)({value})) {{ {cases} "
                f"default: recomp_dispatch(c, {value}); return; }}")

    # ---- one instruction ----
    def translate(self, fn, insn):
        try:
            return self._translate(fn, insn)
        except Unsupported as e:
            key = insn.mnemonic
            self.unsupported[key] = self.unsupported.get(key, 0) + 1
            text = f"{insn.mnemonic} {insn.op_str}".replace("\\", "").replace('"', "'")
            return [f'recomp_fail("unsupported instruction: {text} ({e})", {hx(insn.address)});']

    def _translate(self, fn, insn):
        class Pre:
            pass
        pre = Pre()
        pre.lines = []
        pre.addr_vars = []
        m = insn.mnemonic
        ops = insn.operands
        nxt = insn.address + insn.size
        body = []

        prefix = ""
        if m.startswith(("rep ", "repe ", "repz ", "repne ", "repnz ")):
            prefix, m = m.split(" ", 1)
        if m.startswith("notrack "):
            m = m.split(" ", 1)[1]
        if m.startswith("bnd "):
            m = m.split(" ", 1)[1]

        if m in IGNORED:
            return []

        try:
            vec = self.sse.translate(fn, insn, m, ops, pre)
        except NotSSE as e:
            raise Unsupported(str(e))
        if vec is not None:
            lines = pre.lines + vec
            return ["{ " + " ".join(lines) + " }"] if lines else []

        def rd(i):
            return self.read(ops[i], insn, pre)

        def wr(i, v):
            return self.write(ops[i], insn, pre, v)

        if m in ("mov", "movabs"):
            v, _ = rd(1)
            body.append(wr(0, v))
        elif m == "movzx":
            v, _ = rd(1)
            body.append(wr(0, v))
        elif m in ("movsx", "movsxd"):
            v, sb = rd(1)
            db = ops[0].size * 8
            body.append(wr(0, f"(SEXT({v}, {sb}) & {hx(mask(db))})"))
        elif m == "lea":
            db = ops[0].size * 8
            body.append(wr(0, f"({self.mem_addr(ops[1], insn)} & {hx(mask(db))})"))
        elif m in ("add", "adc", "sub", "sbb", "cmp", "and", "or", "xor", "test"):
            a, bits = rd(0)
            b, _ = rd(1)
            b = f"(({b}) & {hx(mask(bits))})"
            if m in ("add", "adc"):
                carry = "c->cf" if m == "adc" else "0"
                body.append(f"uint64_t r = F_ADD(c, {a}, {b}, {carry}, {bits});")
            elif m in ("sub", "sbb", "cmp"):
                borrow = "c->cf" if m == "sbb" else "0"
                body.append(f"uint64_t r = F_SUB(c, {a}, {b}, {borrow}, {bits});")
            else:
                op = {"and": "&", "test": "&", "or": "|", "xor": "^"}[m]
                body.append(f"uint64_t r = F_LOGIC(c, ({a}) {op} {b}, {bits});")
            if m not in ("cmp", "test"):
                body.append(wr(0, "r"))
            else:
                body.append("(void)r;")
        elif m in ("inc", "dec", "neg"):
            a, bits = rd(0)
            fnm = {"inc": "F_INC", "dec": "F_DEC", "neg": "F_NEG"}[m]
            body.append(wr(0, f"{fnm}(c, {a}, {bits})"))
        elif m == "not":
            a, bits = rd(0)
            body.append(wr(0, f"(~({a}) & {hx(mask(bits))})"))
        elif m == "imul":
            if len(ops) == 1:
                v, bits = rd(0)
                body.append(f"recomp_mul1(c, {v}, {bits}, 1);")
            elif len(ops) == 2:
                a, bits = rd(0)
                b, _ = rd(1)
                body.append(wr(0, f"F_IMUL(c, {a}, {b}, {bits})"))
            else:
                a, bits = rd(1)
                b, _ = rd(2)
                body.append(wr(0, f"F_IMUL(c, {a}, {b}, {bits})"))
        elif m in ("mul", "div", "idiv"):
            v, bits = rd(0)
            signed = 1 if m == "idiv" else 0
            fnm = "recomp_mul1" if m == "mul" else "recomp_div1"
            body.append(f"{fnm}(c, {v}, {bits}, {signed});")
        elif m in ("shl", "sal", "shr", "sar", "rol", "ror"):
            a, bits = rd(0)
            n = rd(1)[0] if len(ops) > 1 else "1"
            fnm = {"shl": "F_SHL", "sal": "F_SHL", "shr": "F_SHR", "sar": "F_SAR",
                   "rol": "F_ROL", "ror": "F_ROR"}[m]
            body.append(wr(0, f"{fnm}(c, {a}, {n}, {bits})"))
        elif m in ("shld", "shrd"):
            a, bits = rd(0)
            b, _ = rd(1)
            n = rd(2)[0]
            body.append(f"uint64_t n = ({n}) & {63 if bits == 64 else 31};")
            body.append("if (n) {")
            if m == "shld":
                body.append(f"  unsigned __int128 w = ((unsigned __int128)({a}) << {bits}) | ({b});")
                body.append(f"  uint64_t r = (uint64_t)((w << n) >> {bits}) & {hx(mask(bits))};")
                body.append(f"  c->cf = (uint8_t)(((uint64_t)({a}) >> ({bits} - n)) & 1);")
            else:
                body.append(f"  unsigned __int128 w = ((unsigned __int128)({b}) << {bits}) | ({a});")
                body.append(f"  uint64_t r = (uint64_t)(w >> n) & {hx(mask(bits))};")
                body.append(f"  c->cf = (uint8_t)(((uint64_t)({a}) >> (n - 1)) & 1);")
            body.append(f"  SZP(c, r, {bits});")
            body.append("  " + wr(0, "r"))
            body.append("}")
        elif m == "push":
            v, bits = rd(0)
            if bits == 16:
                raise Unsupported("16-bit push")
            body.append(f"uint64_t v = {v};")
            if ops[0].type == X.X86_OP_IMM:
                body.append(f"v = SEXT(v, {bits});")
            body.append("PUSH(c, v);")
        elif m == "pop":
            body.append("uint64_t v = POP(c);")
            body.append(wr(0, "v"))
        elif m == "leave":
            body.append("c->r[RSP] = c->r[RBP];")
            body.append("c->r[RBP] = POP(c);")
        elif m == "cdq":
            body.append("c->r[RDX] = (c->r[RAX] & 0x80000000ull) ? 0xffffffffull : 0;")
        elif m == "cqo":
            body.append("c->r[RDX] = (c->r[RAX] >> 63) ? ~0ull : 0;")
        elif m == "cwd":
            body.append("c->r[RDX] = (c->r[RDX] & ~0xFFFFull) | ((c->r[RAX] & 0x8000) ? 0xFFFF : 0);")
        elif m == "cdqe":
            body.append("c->r[RAX] = SEXT(c->r[RAX] & 0xffffffffull, 32);")
        elif m == "cwde":
            body.append("c->r[RAX] = SEXT(c->r[RAX] & 0xFFFF, 16) & 0xffffffffull;")
        elif m == "cbw":
            body.append("c->r[RAX] = (c->r[RAX] & ~0xFFFFull) | (SEXT(c->r[RAX] & 0xFF, 8) & 0xFFFF);")
        elif m == "xchg":
            a, _ = rd(0)
            b, _ = rd(1)
            body.append(f"uint64_t x = {a}, y = {b};")
            body.append(wr(0, "y"))
            body.append(wr(1, "x"))
        elif m == "xadd":
            a, bits = rd(0)
            b, _ = rd(1)
            body.append(f"uint64_t x = {a}, y = {b};")
            body.append(f"uint64_t r = F_ADD(c, x, y, 0, {bits});")
            body.append(wr(1, "x"))
            body.append(wr(0, "r"))
        elif m == "cmpxchg":
            a, bits = rd(0)
            b, _ = rd(1)
            acc = {8: "(c->r[RAX] & 0xFF)", 16: "(c->r[RAX] & 0xFFFF)",
                   32: "(c->r[RAX] & 0xffffffffull)", 64: "c->r[RAX]"}[bits]
            body.append(f"uint64_t d = {a};")
            body.append(f"F_SUB(c, {acc}, d, 0, {bits});")
            body.append("if (c->zf) { " + wr(0, b) + " } else { "
                        + self.reg_write(self._acc_reg(bits), insn, "d") + " }")
        elif m.startswith("set") and m[3:] in CONDS:
            body.append(wr(0, f"({CONDS[m[3:]]} ? 1 : 0)"))
        elif m.startswith("cmov") and m[4:] in CONDS:
            a, _ = rd(0)
            b, _ = rd(1)
            body.append(wr(0, f"({CONDS[m[4:]]} ? ({b}) : ({a}))"))
        elif m in ("bt", "bts", "btr", "btc"):
            if ops[0].type != X.X86_OP_REG and ops[1].type != X.X86_OP_IMM:
                raise Unsupported("bit test on memory with a register offset")
            a, bits = rd(0)
            b, _ = rd(1)
            body.append(f"uint64_t n = ({b}) & {bits - 1};")
            body.append(f"c->cf = (uint8_t)((({a}) >> n) & 1);")
            if m == "bts":
                body.append(wr(0, f"({a}) | (1ull << n)"))
            elif m == "btr":
                body.append(wr(0, f"({a}) & ~(1ull << n)"))
            elif m == "btc":
                body.append(wr(0, f"({a}) ^ (1ull << n)"))
        elif m in ("bsf", "bsr", "tzcnt", "lzcnt", "popcnt"):
            a, _ = rd(0)
            s, bits = rd(1)
            body.append(f"uint64_t s = {s};")
            if m == "bsf":
                body.append("c->zf = s == 0;")
                body.append("if (s) { " + wr(0, "__builtin_ctzll(s)") + " }")
            elif m == "bsr":
                body.append("c->zf = s == 0;")
                body.append("if (s) { " + wr(0, "63 - __builtin_clzll(s)") + " }")
            elif m == "tzcnt":
                body.append(f"uint64_t r = s ? (uint64_t)__builtin_ctzll(s) : {bits};")
                body.append("c->cf = s == 0; c->zf = r == 0;")
                body.append(wr(0, "r"))
            elif m == "lzcnt":
                body.append(f"uint64_t r = s ? (uint64_t)(__builtin_clzll(s) - {64 - bits}) : {bits};")
                body.append("c->cf = s == 0; c->zf = r == 0;")
                body.append(wr(0, "r"))
            else:
                body.append("uint64_t r = (uint64_t)__builtin_popcountll(s);")
                body.append("c->zf = s == 0; c->cf = c->of = c->sf = c->pf = 0;")
                body.append(wr(0, "r"))
        elif m[:-1] in ("stos", "movs", "lods") and m[-1] in "bwdq":
            size = {"b": 1, "w": 2, "d": 4, "q": 8}[m[-1]]
            bits = size * 8
            kind = m[:-1]
            if kind == "stos":
                step = f"WR{bits}(c->r[RDI], c->r[RAX] & {hx(mask(bits))}); c->r[RDI] += {size};"
            elif kind == "movs":
                step = (f"WR{bits}(c->r[RDI], RD{bits}(c->r[RSI])); "
                        f"c->r[RDI] += {size}; c->r[RSI] += {size};")
            else:
                step = (self.reg_write(self._acc_reg(bits), insn, f"RD{bits}(c->r[RSI])")
                        + f" c->r[RSI] += {size};")
            if prefix in ("rep", "repe", "repz"):
                body.append(f"while (c->r[RCX]) {{ {step} c->r[RCX]--; }}")
            elif prefix:
                raise Unsupported(f"{prefix} prefix")
            else:
                body.append(step)
        elif m == "bswap":
            a, bits = rd(0)
            body.append(wr(0, f"__builtin_bswap{bits}({a})"))
        elif m == "syscall":
            body.append("recomp_syscall(c);")
        elif m in ("hlt", "ud2", "int3"):
            body.append(f'recomp_fail("{m}", {hx(insn.address)});')
        elif m == "ret":
            extra = ops[0].imm if ops else 0
            body.append(f"c->r[RSP] += {8 + extra}; return;")
        elif m == "call":
            if ops[0].type == X.X86_OP_IMM:
                t = ops[0].imm & mask(64)
                body.append(f"PUSH(c, {hx(nxt)});")
                if t in self.funcs:
                    body.append(f"{self.funcs[t].cname}(c);")
                else:
                    body.append(f"recomp_dispatch(c, {hx(t)});")
            else:
                v, _ = rd(0)
                body.append(f"uint64_t t = {v};")
                body.append(f"PUSH(c, {hx(nxt)});")
                body.append("recomp_dispatch(c, t);")
        elif m == "jmp":
            if ops[0].type == X.X86_OP_IMM:
                body.append(self.goto(fn, ops[0].imm & mask(64)))
            else:
                v, _ = rd(0)
                body.append(f"uint64_t t = {v};")
                body.append(self.indirect_jump(fn, "t"))
        elif m.startswith("j") and (m[1:] in CONDS or m in ("jrcxz", "jecxz")):
            cond = {"jrcxz": "(c->r[RCX] == 0)", "jecxz": "((c->r[RCX] & 0xffffffffull) == 0)"}.get(
                m, CONDS.get(m[1:]))
            body.append(f"if ({cond}) {self.goto(fn, ops[0].imm & mask(64))}")
        else:
            raise Unsupported("not handled")

        lines = pre.lines + body
        if not lines:
            return []
        return ["{ " + " ".join(lines) + " }"]

    @staticmethod
    def _acc_reg(bits):
        return {8: X.X86_REG_AL, 16: X.X86_REG_AX, 32: X.X86_REG_EAX, 64: X.X86_REG_RAX}[bits]

    def function_c(self, fn):
        out = [f"void {fn.cname}(Ctx *c) {{" + (f"  /* {fn.name} */" if fn.name else "")]
        addrs = sorted(fn.insns)
        if addrs and addrs[0] != fn.addr:
            # Code below the start (a block it jumps back to): begin at the start.
            out.append(f"    goto L_{fn.addr:x};")
        for i, a in enumerate(addrs):
            insn = fn.insns[a]
            out.append(f"L_{a:x}: /* {insn.mnemonic} {insn.op_str} */".replace("*/ */", "* / */"))
            for line in self.translate(fn, insn):
                out.append("    " + line)
            nxt = a + insn.size
            last = insn.mnemonic.split(" ")[-1] in TERMINATORS
            if not last and (i + 1 == len(addrs) or addrs[i + 1] != nxt):
                # The next instruction is not in this function: carry on there.
                out.append("    " + self.goto(fn, nxt))
        out.append("    return;")
        out.append("}")
        return "\n".join(out)
