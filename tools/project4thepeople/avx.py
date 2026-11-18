"""AVX, AVX2 and FMA3 instructions (VEX encoded), as in the PS4 (AVX) and
PS5 (AVX2, FMA3) CPUs.

A YMM register is two 128-bit halves: `c->x[i]` (the XMM register) and
`c->xh[i]`. Most AVX instructions work on each half separately, so they are
done by running the SSE translation once per half, on scratch registers
x[16..23] loaded from the operands:

    vaddps ymm0, ymm1, ymm2   ->  for each half: x16 = ymm1.half; x17 = ymm2.half;
                                  `addps x16, x17`; ymm0.half = x16

A 128-bit VEX instruction clears the upper half of its destination; the
legacy SSE form keeps it. Instructions that move data between the halves
(broadcasts, permutes, inserts, extracts, width-changing conversions, FMA
with three sources) are written out here directly.
"""

from capstone import x86_const as X

from sse import LANES, NotSSE

SCRATCH = 16
FAKE_BASE = 100000

# SSE instructions whose destination is not also a source: the VEX form has
# the same operands. Every other VEX instruction is "d, s1, s2": d = s1 op s2.
NON_DESTRUCTIVE = {
    "movaps", "movups", "movapd", "movupd", "movdqa", "movdqu", "lddqu", "movntps", "movntpd", "movntdq",
    "movntdqa", "movd", "movq", "movddup", "movshdup", "movsldup",
    "pshufd", "pshuflw", "pshufhw", "sqrtps", "sqrtpd", "rcpps", "rsqrtps", "roundps", "roundpd",
    "cvtdq2ps", "cvttps2dq", "cvtps2dq", "cvtdq2pd", "cvtps2pd", "cvtpd2ps", "cvttpd2dq", "cvtpd2dq",
    "cvttss2si", "cvttsd2si", "cvtss2si", "cvtsd2si", "comiss", "ucomiss", "comisd", "ucomisd",
    "movmskps", "movmskpd", "pmovmskb", "pextrw", "pextrb", "pextrd", "pextrq", "extractps",
    "pabsb", "pabsw", "pabsd", "ptest", "testps", "testpd",
    "pmovzxbw", "pmovzxbd", "pmovzxbq", "pmovzxwd", "pmovzxwq", "pmovzxdq",
    "pmovsxbw", "pmovsxbd", "pmovsxbq", "pmovsxwd", "pmovsxwq", "pmovsxdq",
}
MOVES = {"movaps", "movups", "movapd", "movupd", "movdqa", "movdqu", "lddqu",
         "movntps", "movntpd", "movntdq", "movntdqa"}


class FakeOp:
    """An XMM scratch register standing in for one half of an operand."""
    type = X.X86_OP_REG
    size = 16

    def __init__(self, index):
        self.reg = FAKE_BASE + index


class FakeImm:
    type = X.X86_OP_IMM
    size = 1

    def __init__(self, imm):
        self.imm = imm


class FakeInsn:
    def __init__(self, insn):
        self._insn = insn

    def reg_name(self, reg):
        if reg >= FAKE_BASE:
            return f"xmm{reg - FAKE_BASE}"
        return self._insn.reg_name(reg)

    def __getattr__(self, name):
        return getattr(self._insn, name)


def vreg(insn, op):
    """(index, is_ymm) for an XMM/YMM register operand, else None."""
    if op.type != X.X86_OP_REG:
        return None
    name = insn.reg_name(op.reg) or ""
    if name.startswith(("xmm", "ymm")):
        return int(name[3:]), name.startswith("ymm")
    return None


class AVX:
    def __init__(self, tr, sse):
        self.tr = tr
        self.sse = sse

    # ---- operand halves ----
    def half(self, insn, op, h, pre):
        """C expression for half `h` (0 low, 1 high) of a 128/256-bit operand."""
        v = vreg(insn, op)
        if v:
            return f"c->x{'h' if h else ''}[{v[0]}]"
        a = self.tr.addr_var(op, insn, pre)
        return f"XRD({a} + {16 * h})"

    def set_half(self, insn, op, h, pre, value):
        v = vreg(insn, op)
        if v:
            return f"c->x{'h' if h else ''}[{v[0]}] = {value};"
        a = self.tr.addr_var(op, insn, pre)
        return f"XWR({a} + {16 * h}, {value});"

    def is_vec(self, insn, op):
        return vreg(insn, op) is not None or (op.type == X.X86_OP_MEM and op.size >= 16)

    def width(self, insn, ops):
        for op in ops:
            v = vreg(insn, op)
            if (v and v[1]) or (op.type == X.X86_OP_MEM and op.size == 32):
                return 32
        return 16

    def clear_upper(self, insn, op):
        v = vreg(insn, op)
        return [f"c->xh[{v[0]}] = XZERO();"] if v else []

    # ---- entry ----
    def translate(self, fn, insn, m, ops, pre):
        """C statements for a VEX instruction, or None if `m` is not one."""
        if not m.startswith("v") or m in ("verr", "verw"):
            return None
        if m == "vzeroupper":
            return ["for (int i = 0; i < 16; i++) c->xh[i] = XZERO();"]
        if m == "vzeroall":
            return ["for (int i = 0; i < 16; i++) c->x[i] = c->xh[i] = XZERO();"]
        base = m[1:]
        special = self.special(insn, base, ops, pre)
        if special is not None:
            return special
        return self.generic(fn, insn, base, ops, pre)

    def generic(self, fn, insn, base, ops, pre):
        width = self.width(insn, ops)
        fake = FakeInsn(insn)
        body = []
        dst = ops[0]
        if base in MOVES:
            if vreg(insn, dst):
                body.append(self.set_half(insn, dst, 0, pre, self.half(insn, ops[1], 0, pre)))
                body.append(self.set_half(insn, dst, 1, pre,
                                          self.half(insn, ops[1], 1, pre) if width == 32 else "XZERO()"))
            else:
                for h in range(width // 16):
                    body.append(self.set_half(insn, dst, h, pre, self.half(insn, ops[1], h, pre)))
            return body
        # Two-operand forms (vmovss loads and stores, vmovhps stores) keep the SSE operands.
        destructive = base not in NON_DESTRUCTIVE and len(ops) >= 3
        # (psllvd and friends shift each element by its own count: those are per half.)
        shift_by_vector = base[:4] in ("psll", "psrl", "psra") and not base.endswith("dq") and base[4] != "v"
        stores = []
        for h in range(width // 16):
            k = SCRATCH
            mapped = []
            for i, op in enumerate(ops):
                if self.is_vec(insn, op):
                    # A shift count is always the low 64 bits of its register.
                    hh = 0 if (shift_by_vector and i == len(ops) - 1) else h
                    body.append(f"c->x[{k}] = {self.half(insn, op, hh, pre)};")
                    mapped.append(FakeOp(k))
                    k += 1
                else:
                    if op.type == X.X86_OP_MEM and h:
                        raise NotSSE("scalar memory operand in a 256-bit instruction")
                    if op.type == X.X86_OP_IMM and base == "shufpd" and h:
                        op = FakeImm(op.imm >> 2)   # vshufpd: two selector bits per half
                    mapped.append(op)
            sse_ops = mapped[1:] if destructive else mapped
            if base in ("blendvps", "blendvpd", "pblendvb"):
                sse_ops = mapped[1:]   # d, s1, s2, mask -> s1 (dest), s2, mask
            r = self.sse.translate(fn, fake, base, sse_ops, pre)
            if r is None:
                raise NotSSE("no SSE form")
            body += ["{"] + r + ["}"]   # each half in its own block: the SSE code declares locals
            result = sse_ops[0]
            if isinstance(result, FakeOp) and (vreg(insn, dst) or (dst.type == X.X86_OP_MEM and dst.size >= 16)):
                # Kept until both halves are done: the destination can also be a source.
                body.append(f"Xmm res{h} = c->x[{result.reg - FAKE_BASE}];")
                stores.append(self.set_half(insn, dst, h, pre, f"res{h}"))
        body += stores
        if width == 16 and vreg(insn, dst):
            body += self.clear_upper(insn, dst)
        return ["{ " + " ".join(body) + " }"]

    # ---- instructions that cross halves or have no SSE form ----
    def special(self, insn, base, ops, pre):
        tr = self.tr
        width = self.width(insn, ops)
        halves = width // 16

        def src_lane0(op, lane):
            v = vreg(insn, op)
            if v:
                return f"c->x[{v[0]}].{lane}[0]"
            if op.type == X.X86_OP_MEM:
                bits = {"b": 8, "w": 16, "d": 32, "q": 64}[lane]
                return f"RD{bits}({tr.addr_var(op, insn, pre)})"
            v, _ = tr.read(op, insn, pre)
            return v

        bcast = {"broadcastss": "d", "broadcastsd": "q", "pbroadcastb": "b", "pbroadcastw": "w",
                 "pbroadcastd": "d", "pbroadcastq": "q", "movddup_": "q"}
        if base in bcast:
            lane = bcast[base]
            n = {"b": 16, "w": 8, "d": 4, "q": 2}[lane]
            body = [f"Xmm r; uint64_t v = {src_lane0(ops[1], lane)};"]
            body += [f"r.{lane}[{i}] = v;" for i in range(n)]
            body.append(self.set_half(insn, ops[0], 0, pre, "r"))
            body.append(self.set_half(insn, ops[0], 1, pre, "r" if width == 32 else "XZERO()"))
            return ["{ " + " ".join(body) + " }"]
        if base in ("broadcastf128", "broadcasti128"):
            v = self.half(insn, ops[1], 0, pre)
            return ["{ Xmm r = " + v + "; " + self.set_half(insn, ops[0], 0, pre, "r") + " "
                    + self.set_half(insn, ops[0], 1, pre, "r") + " }"]
        if base in ("insertf128", "inserti128"):
            k = ops[3].imm & 1
            body = [f"Xmm lo = {self.half(insn, ops[1], 0, pre)}, hi = {self.half(insn, ops[1], 1, pre)};",
                    f"Xmm v = {self.half(insn, ops[2], 0, pre)};", f"{'hi' if k else 'lo'} = v;",
                    self.set_half(insn, ops[0], 0, pre, "lo"), self.set_half(insn, ops[0], 1, pre, "hi")]
            return ["{ " + " ".join(body) + " }"]
        if base in ("extractf128", "extracti128"):
            k = ops[2].imm & 1
            body = [f"Xmm v = {self.half(insn, ops[1], k, pre)};", self.set_half(insn, ops[0], 0, pre, "v")]
            body += self.clear_upper(insn, ops[0])
            return ["{ " + " ".join(body) + " }"]
        if base in ("perm2f128", "perm2i128"):
            imm = ops[3].imm & 0xFF
            body = [f"Xmm q[4] = {{ {self.half(insn, ops[1], 0, pre)}, {self.half(insn, ops[1], 1, pre)}, "
                    f"{self.half(insn, ops[2], 0, pre)}, {self.half(insn, ops[2], 1, pre)} }};"]
            for h in range(2):
                sel = (imm >> (4 * h)) & 0xF
                body.append(self.set_half(insn, ops[0], h, pre, "XZERO()" if sel & 8 else f"q[{sel & 3}]"))
            return ["{ " + " ".join(body) + " }"]
        if base in ("permq", "permpd"):
            imm = ops[2].imm & 0xFF
            body = [f"uint64_t s[4]; Xmm lo = {self.half(insn, ops[1], 0, pre)}, hi = {self.half(insn, ops[1], 1, pre)};",
                    "s[0] = lo.q[0]; s[1] = lo.q[1]; s[2] = hi.q[0]; s[3] = hi.q[1];",
                    f"lo.q[0] = s[{imm & 3}]; lo.q[1] = s[{(imm >> 2) & 3}]; "
                    f"hi.q[0] = s[{(imm >> 4) & 3}]; hi.q[1] = s[{(imm >> 6) & 3}];",
                    self.set_half(insn, ops[0], 0, pre, "lo"), self.set_half(insn, ops[0], 1, pre, "hi")]
            return ["{ " + " ".join(body) + " }"]
        if base in ("permd", "permps"):
            body = [f"Xmm il = {self.half(insn, ops[1], 0, pre)}, ih = {self.half(insn, ops[1], 1, pre)};",
                    f"Xmm sl = {self.half(insn, ops[2], 0, pre)}, sh = {self.half(insn, ops[2], 1, pre)};",
                    "uint32_t s[8], idx[8], r[8]; memcpy(s, sl.d, 16); memcpy(s + 4, sh.d, 16);",
                    "memcpy(idx, il.d, 16); memcpy(idx + 4, ih.d, 16);",
                    "for (int i = 0; i < 8; i++) r[i] = s[idx[i] & 7];",
                    "memcpy(sl.d, r, 16); memcpy(sh.d, r + 4, 16);",
                    self.set_half(insn, ops[0], 0, pre, "sl"), self.set_half(insn, ops[0], 1, pre, "sh")]
            return ["{ " + " ".join(body) + " }"]
        if base in ("permilps", "permilpd") and ops[-1].type == X.X86_OP_IMM:
            imm = ops[2].imm & 0xFF
            body = []
            for h in range(halves):
                body.append(f"{{ Xmm s = {self.half(insn, ops[1], h, pre)}, r;")
                if base == "permilps":
                    body += [f"r.d[{i}] = s.d[{(imm >> (2 * i)) & 3}];" for i in range(4)]
                else:
                    body += [f"r.q[{i}] = s.q[{(imm >> (2 * h + i)) & 1}];" for i in range(2)]
                body.append(self.set_half(insn, ops[0], h, pre, "r") + " }")
            if halves == 1:
                body += self.clear_upper(insn, ops[0])
            return ["{ " + " ".join(body) + " }"]
        if base in ("ptest", "testps", "testpd") and width == 32:
            a = [self.half(insn, ops[0], h, pre) for h in range(2)]
            b = [self.half(insn, ops[1], h, pre) for h in range(2)]
            body = [f"Xmm a0 = {a[0]}, a1 = {a[1]}, b0 = {b[0]}, b1 = {b[1]};",
                    "c->zf = ((a0.q[0] & b0.q[0]) | (a0.q[1] & b0.q[1]) | (a1.q[0] & b1.q[0]) | (a1.q[1] & b1.q[1])) == 0;",
                    "c->cf = ((~a0.q[0] & b0.q[0]) | (~a0.q[1] & b0.q[1]) | (~a1.q[0] & b1.q[0]) | (~a1.q[1] & b1.q[1])) == 0;",
                    "c->of = c->sf = c->pf = 0;"]
            return ["{ " + " ".join(body) + " }"]
        if base in ("movmskps", "movmskpd", "pmovmskb") and width == 32:
            lane, n, sign = {"movmskps": ("d", 4, 31), "movmskpd": ("q", 2, 63), "pmovmskb": ("b", 16, 7)}[base]
            body = [f"Xmm lo = {self.half(insn, ops[1], 0, pre)}, hi = {self.half(insn, ops[1], 1, pre)};",
                    "uint64_t r = 0;",
                    f"for (int i = 0; i < {n}; i++) {{ r |= (uint64_t)((lo.{lane}[i] >> {sign}) & 1) << i; "
                    f"r |= (uint64_t)((hi.{lane}[i] >> {sign}) & 1) << ({n} + i); }}",
                    tr.write(ops[0], insn, pre, "r")]
            return ["{ " + " ".join(body) + " }"]
        widen = {"cvtps2pd": ("f", "g", "(double)"), "cvtdq2pd": ("sd", "g", "(double)")}
        if base in widen and width == 32:
            sl, dl, cast = widen[base]
            src = self.half(insn, ops[1], 0, pre) if vreg(insn, ops[1]) else None
            if src is None:
                a = tr.addr_var(ops[1], insn, pre)
                src = f"XRD({a})"
            body = [f"Xmm s = {src}, lo, hi;",
                    f"lo.{dl}[0] = {cast}s.{sl}[0]; lo.{dl}[1] = {cast}s.{sl}[1];",
                    f"hi.{dl}[0] = {cast}s.{sl}[2]; hi.{dl}[1] = {cast}s.{sl}[3];",
                    self.set_half(insn, ops[0], 0, pre, "lo"), self.set_half(insn, ops[0], 1, pre, "hi")]
            return ["{ " + " ".join(body) + " }"]
        narrow = {"cvtpd2ps": ("f", "(float)s.g[{i}]"), "cvttpd2dq": ("d", "(uint32_t)CVT_I32(s.g[{i}])"),
                  "cvtpd2dq": ("d", "(uint32_t)CVT_I32(RNE(s.g[{i}]))")}
        if base in narrow and width == 32:
            lane, e = narrow[base]
            body = ["Xmm r;"]
            for h in range(2):
                body.append(f"{{ Xmm s = {self.half(insn, ops[1], h, pre)};")
                body += [f"r.{lane}[{2 * h + i}] = {e.format(i=i)};" for i in range(2)]
                body.append("}")
            body.append(self.set_half(insn, ops[0], 0, pre, "r"))
            body += self.clear_upper(insn, ops[0])
            return ["{ " + " ".join(body) + " }"]
        if base.startswith(("pmovzx", "pmovsx")) and width == 32:
            sizes = {"b": 1, "w": 2, "d": 4, "q": 8}
            src_size, dst_size = sizes[base[6]], sizes[base[7]]
            n = 32 // dst_size
            signed = base[4] == "s"
            slane = ("s" if signed else "") + {1: "b", 2: "w", 4: "d"}[src_size]
            dlane = {2: "w", 4: "d", 8: "q"}[dst_size]
            src = self.half(insn, ops[1], 0, pre) if vreg(insn, ops[1]) else f"XRD({tr.addr_var(ops[1], insn, pre)})"
            body = [f"Xmm s = {src}, lo, hi;"]
            for i in range(n):
                target = "lo" if i < n // 2 else "hi"
                j = i % (n // 2)
                e = f"(uint64_t)(int64_t)s.{slane}[{i}]" if signed else f"s.{slane}[{i}]"
                body.append(f"{target}.{dlane}[{j}] = {e};")
            body += [self.set_half(insn, ops[0], 0, pre, "lo"), self.set_half(insn, ops[0], 1, pre, "hi")]
            return ["{ " + " ".join(body) + " }"]
        if base in ("pblendd", "blendps", "blendpd", "pblendw") and len(ops) == 4:
            imm = ops[3].imm & 0xFF
            lane, n = {"pblendd": ("d", 4), "blendps": ("d", 4), "blendpd": ("q", 2), "pblendw": ("w", 8)}[base]
            body = []
            for h in range(halves):
                body.append(f"{{ Xmm a = {self.half(insn, ops[1], h, pre)}, b = {self.half(insn, ops[2], h, pre)};")
                for i in range(n):
                    bit = (imm >> (i if base == "pblendw" else (h * n + i))) & 1
                    if bit:
                        body.append(f"a.{lane}[{i}] = b.{lane}[{i}];")
                body.append(self.set_half(insn, ops[0], h, pre, "a") + " }")
            if halves == 1:
                body += self.clear_upper(insn, ops[0])
            return ["{ " + " ".join(body) + " }"]
        fma = self.fma(insn, base, ops, pre, halves)
        if fma is not None:
            return fma
        return None

    def fma(self, insn, base, ops, pre, halves):
        """vfmadd/vfmsub/vfnmadd/vfnmsub with 132, 213, 231 operand orders."""
        for kind in ("fnmadd", "fnmsub", "fmadd", "fmsub"):
            if base.startswith(kind) and base[len(kind):len(kind) + 3] in ("132", "213", "231"):
                order = base[len(kind):len(kind) + 3]
                suf = base[len(kind) + 3:]
                break
        else:
            return None
        if suf not in LANES:
            return None
        lane, n = LANES[suf]
        fn = "FMAF" if lane == "f" else "FMAD"
        neg_prod = kind.startswith("fn")
        sub = kind.endswith("sub")
        scalar = suf in ("ss", "sd")
        body = []
        for h in range(1 if scalar else halves):
            body.append(f"{{ Xmm o1 = {self.half(insn, ops[0], h, pre)};")
            body.append(f"Xmm o2 = {self.half(insn, ops[1], h, pre)};")
            if ops[2].type == X.X86_OP_MEM and scalar:
                bits = 32 if lane == "f" else 64
                ilane = "d" if lane == "f" else "q"
                body.append(f"Xmm o3 = XZERO(); o3.{ilane}[0] = RD{bits}({self.tr.addr_var(ops[2], insn, pre)});")
            else:
                body.append(f"Xmm o3 = {self.half(insn, ops[2], h, pre)};")
            a, b, cc = {"132": ("o1", "o3", "o2"), "213": ("o2", "o1", "o3"), "231": ("o2", "o3", "o1")}[order]
            for i in range(n):
                x, y, z = f"{a}.{lane}[{i}]", f"{b}.{lane}[{i}]", f"{cc}.{lane}[{i}]"
                if neg_prod:
                    x = f"-{x}"
                if sub:
                    z = f"-{z}"
                body.append(f"o1.{lane}[{i}] = {fn}({x}, {y}, {z});")
            body.append(self.set_half(insn, ops[0], h, pre, "o1") + " }")
        if halves == 1 or scalar:
            body += self.clear_upper(insn, ops[0])
        return ["{ " + " ".join(body) + " }"]
