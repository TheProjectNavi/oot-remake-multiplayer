"""SSE and SSE2 instructions (the x86-64 baseline) to C.

The 16 XMM registers live in `c->x[i]`, a union with lanes of every type.
Each instruction loads its source into a local `Xmm s`, works on the
destination lanes and stores the result. Rounding is the default (nearest
even); MXCSR changes and floating point exceptions are not modelled.
"""

from capstone import x86_const as X

LANES = {"ss": ("f", 1), "sd": ("g", 1), "ps": ("f", 4), "pd": ("g", 2)}
INT_LANES = {"b": ("b", "sb", 16, 8), "w": ("w", "sw", 8, 16), "d": ("d", "sd", 4, 32), "q": ("q", "sq", 2, 64)}
CMP_PRED = {  # cmpXXss predicate -> C condition on (a, b)
    "eq": "a == b", "lt": "a < b", "le": "a <= b", "unord": "(a != a || b != b)",
    "neq": "!(a == b)", "nlt": "!(a < b)", "nle": "!(a <= b)", "ord": "(a == a && b == b)",
}
CMP_IMM = ["eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"]


class NotSSE(Exception):
    pass


def xmm_index(insn, op):
    name = insn.reg_name(op.reg)
    if name and name.startswith("xmm"):
        return int(name[3:])
    return None


class SSE:
    def __init__(self, tr):
        self.tr = tr

    def is_xmm(self, insn, op):
        return op.type == X.X86_OP_REG and xmm_index(insn, op) is not None

    def load(self, insn, op, pre, var="s"):
        """Statements that put the operand into a local Xmm `var`.
        A memory operand smaller than 16 bytes fills the low lanes, rest zero."""
        if self.is_xmm(insn, op):
            return [f"Xmm {var} = c->x[{xmm_index(insn, op)}];"]
        if op.type == X.X86_OP_MEM:
            a = self.tr.addr_var(op, insn, pre)
            if op.size == 16:
                return [f"Xmm {var} = XRD({a});"]
            bits = op.size * 8
            lane = {4: "d", 8: "q", 2: "w", 1: "b"}[op.size]
            return [f"Xmm {var} = XZERO(); {var}.{lane}[0] = RD{bits}({a});"]
        if op.type == X.X86_OP_REG:
            v, bits = self.tr.reg_read(op.reg, insn)
            lane = "d" if bits <= 32 else "q"
            return [f"Xmm {var} = XZERO(); {var}.{lane}[0] = {v};"]
        raise NotSSE("operand")

    def dst(self, insn, op):
        if not self.is_xmm(insn, op):
            raise NotSSE("destination is not an XMM register")
        return f"c->x[{xmm_index(insn, op)}]"

    def translate(self, fn, insn, m, ops, pre):
        """C statements for an SSE instruction, or None if `m` is not one."""
        tr = self.tr
        uses_xmm = any(self.is_xmm(insn, o) for o in ops)
        if m in ("ldmxcsr",):
            return ["/* rounding mode changes are not modelled */"]
        if m == "stmxcsr":
            return [tr.write(ops[0], insn, pre, "0x1f80")]
        if not uses_xmm and m not in ("cvttss2si", "cvttsd2si", "cvtss2si", "cvtsd2si"):
            return None

        # ---- moves ----
        if m in ("movaps", "movups", "movapd", "movupd", "movdqa", "movdqu", "lddqu"):
            if self.is_xmm(insn, ops[0]):
                return self.load(insn, ops[1], pre) + [f"{self.dst(insn, ops[0])} = s;"]
            a = tr.addr_var(ops[0], insn, pre)
            return [f"XWR({a}, c->x[{xmm_index(insn, ops[1])}]);"]
        if m in ("movss", "movsd", "movd", "movq"):
            lane, size = ("d", 4) if m in ("movss", "movd") else ("q", 8)
            d, s = ops
            if self.is_xmm(insn, d):
                body = self.load(insn, s, pre)
                if m in ("movss", "movsd") and self.is_xmm(insn, s):
                    return body + [f"{self.dst(insn, d)}.{lane}[0] = s.{lane}[0];"]
                return body + [f"{self.dst(insn, d)} = XZERO(); {self.dst(insn, d)}.{lane}[0] = s.{lane}[0];"]
            src = f"c->x[{xmm_index(insn, s)}].{lane}[0]"
            return [tr.write(d, insn, pre, src)]
        if m in ("movlps", "movlpd", "movhps", "movhpd"):
            half = 0 if m[3] == "l" else 1
            d, s = ops
            if self.is_xmm(insn, d):
                a = tr.addr_var(s, insn, pre)
                return [f"{self.dst(insn, d)}.q[{half}] = RD64({a});"]
            a = tr.addr_var(d, insn, pre)
            return [f"WR64({a}, c->x[{xmm_index(insn, s)}].q[{half}]);"]
        if m == "movhlps":
            return [f"{self.dst(insn, ops[0])}.q[0] = c->x[{xmm_index(insn, ops[1])}].q[1];"]
        if m == "movlhps":
            return [f"{self.dst(insn, ops[0])}.q[1] = c->x[{xmm_index(insn, ops[1])}].q[0];"]
        if m in ("movmskps", "movmskpd", "pmovmskb"):
            s = f"c->x[{xmm_index(insn, ops[1])}]"
            if m == "movmskps":
                expr = " | ".join(f"(({s}.d[{i}] >> 31) << {i})" for i in range(4))
            elif m == "movmskpd":
                expr = f"(({s}.q[0] >> 63) | (({s}.q[1] >> 63) << 1))"
            else:
                expr = " | ".join(f"((uint64_t)({s}.b[{i}] >> 7) << {i})" for i in range(16))
            return [tr.write(ops[0], insn, pre, f"({expr})")]

        # ---- float arithmetic ----
        for opname, cop in (("add", "+"), ("sub", "-"), ("mul", "*"), ("div", "/"),
                            ("min", None), ("max", None), ("sqrt", None)):
            for suf, (lane, n) in LANES.items():
                if m != opname + suf:
                    continue
                d = self.dst(insn, ops[0])
                body = self.load(insn, ops[1], pre)
                for i in range(n):
                    a, b = f"{d}.{lane}[{i}]", f"s.{lane}[{i}]"
                    if cop:
                        e = f"{a} {cop} {b}"
                    elif opname == "min":
                        e = f"({a} < {b} ? {a} : {b})"
                    elif opname == "max":
                        e = f"({a} > {b} ? {a} : {b})"
                    else:
                        e = f"__builtin_sqrt{'f' if lane == 'f' else ''}({b})"
                    body.append(f"{a} = {e};")
                return body

        # ---- bitwise (all lanes) ----
        bit = {"andps": "a & b", "andpd": "a & b", "pand": "a & b",
               "andnps": "~a & b", "andnpd": "~a & b", "pandn": "~a & b",
               "orps": "a | b", "orpd": "a | b", "por": "a | b",
               "xorps": "a ^ b", "xorpd": "a ^ b", "pxor": "a ^ b"}
        if m in bit:
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre)
            for i in range(2):
                e = bit[m].replace("a", f"{d}.q[{i}]").replace("b", f"s.q[{i}]")
                body.append(f"{d}.q[{i}] = {e};")
            return body

        # ---- compares ----
        if m in ("comiss", "ucomiss", "comisd", "ucomisd"):
            lane = "f" if m.endswith("ss") else "g"
            body = self.load(insn, ops[1], pre)
            return body + [f"F_UCOMI(c, c->x[{xmm_index(insn, ops[0])}].{lane}[0], s.{lane}[0]);"]
        if m.startswith("cmp") and m[-2:] in LANES:
            pred = m[3:-2]
            if pred == "" and len(ops) == 3:
                pred = CMP_IMM[ops[2].imm & 7]
            if pred in CMP_PRED:
                lane, n = LANES[m[-2:]]
                ilane = "d" if lane == "f" else "q"
                ones = "0xffffffffu" if lane == "f" else "~0ull"
                d = self.dst(insn, ops[0])
                body = self.load(insn, ops[1], pre)
                for i in range(n):
                    cond = CMP_PRED[pred].replace("a", f"{d}.{lane}[{i}]").replace("b", f"s.{lane}[{i}]")
                    body.append(f"{d}.{ilane}[{i}] = ({cond}) ? {ones} : 0;")
                return body

        # ---- conversions ----
        if m in ("cvtsi2ss", "cvtsi2sd"):
            v, bits = tr.read(ops[1], insn, pre)
            cast = "(int64_t)" if bits == 64 else "(int32_t)"
            lane = "f" if m.endswith("ss") else "g"
            typ = "float" if lane == "f" else "double"
            return [f"{self.dst(insn, ops[0])}.{lane}[0] = ({typ})({cast}({v}));"]
        if m in ("cvttss2si", "cvttsd2si", "cvtss2si", "cvtsd2si"):
            lane = "f" if "ss" in m else "g"
            body = self.load(insn, ops[1], pre)
            bits = ops[0].size * 8
            v = f"(double)s.{lane}[0]"
            if not m.startswith("cvtt"):
                v = f"RNE({v})"
            body.append(tr.write(ops[0], insn, pre, f"CVT_I{bits}({v})"))
            return body
        conv = {
            "cvtss2sd": [("g", 0, "(double)s.f[0]")],
            "cvtsd2ss": [("f", 0, "(float)s.g[0]")],
        }
        if m in conv:
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre)
            for lane, i, e in conv[m]:
                body.append(f"{d}.{lane}[{i}] = {e};")
            return body
        packed = {
            "cvtdq2ps": (4, "f", "(float)s.sd[{i}]", False),
            "cvttps2dq": (4, "d", "(uint32_t)CVT_I32((double)s.f[{i}])", False),
            "cvtps2dq": (4, "d", "(uint32_t)CVT_I32(RNE((double)s.f[{i}]))", False),
            "cvtdq2pd": (2, "g", "(double)s.sd[{i}]", False),
            "cvtps2pd": (2, "g", "(double)s.f[{i}]", False),
            "cvtpd2ps": (2, "f", "(float)s.g[{i}]", True),
            "cvttpd2dq": (2, "d", "(uint32_t)CVT_I32(s.g[{i}])", True),
            "cvtpd2dq": (2, "d", "(uint32_t)CVT_I32(RNE(s.g[{i}]))", True),
        }
        if m in packed:
            n, lane, e, zero_high = packed[m]
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre) + ["Xmm r = XZERO();" if zero_high else f"Xmm r = {d};"]
            for i in range(n):
                body.append(f"r.{lane}[{i}] = {e.format(i=i)};")
            body.append(f"{d} = r;")
            return body

        # ---- shuffles and unpacks ----
        if m in ("shufps", "pshufd", "shufpd", "pshuflw", "pshufhw"):
            d = self.dst(insn, ops[0])
            imm = ops[2].imm & 0xFF
            body = self.load(insn, ops[1], pre) + [f"Xmm a = {d}; Xmm r = a;"]
            if m == "shufps":
                sel = [("a", imm & 3), ("a", (imm >> 2) & 3), ("s", (imm >> 4) & 3), ("s", (imm >> 6) & 3)]
                body += [f"r.d[{i}] = {v}.d[{k}];" for i, (v, k) in enumerate(sel)]
            elif m == "pshufd":
                body += [f"r.d[{i}] = s.d[{(imm >> (2 * i)) & 3}];" for i in range(4)]
            elif m == "shufpd":
                body += [f"r.q[0] = a.q[{imm & 1}];", f"r.q[1] = s.q[{(imm >> 1) & 1}];"]
            elif m == "pshuflw":
                body += ["r = s;"] + [f"r.w[{i}] = s.w[{(imm >> (2 * i)) & 3}];" for i in range(4)]
            else:
                body += ["r = s;"] + [f"r.w[{4 + i}] = s.w[{4 + ((imm >> (2 * i)) & 3)}];" for i in range(4)]
            body.append(f"{d} = r;")
            return body
        unpack = {"unpcklps": ("d", 4, 0), "unpckhps": ("d", 4, 1), "unpcklpd": ("q", 2, 0),
                  "unpckhpd": ("q", 2, 1), "punpcklbw": ("b", 16, 0), "punpckhbw": ("b", 16, 1),
                  "punpcklwd": ("w", 8, 0), "punpckhwd": ("w", 8, 1), "punpckldq": ("d", 4, 0),
                  "punpckhdq": ("d", 4, 1), "punpcklqdq": ("q", 2, 0), "punpckhqdq": ("q", 2, 1)}
        if m in unpack:
            lane, n, high = unpack[m]
            d = self.dst(insn, ops[0])
            base = n // 2 * high
            body = self.load(insn, ops[1], pre) + [f"Xmm a = {d}, r;"]
            for i in range(n // 2):
                body.append(f"r.{lane}[{2 * i}] = a.{lane}[{base + i}]; r.{lane}[{2 * i + 1}] = s.{lane}[{base + i}];")
            body.append(f"{d} = r;")
            return body
        if m in ("packsswb", "packuswb", "packssdw"):
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre) + [f"Xmm a = {d}, r;"]
            if m == "packssdw":
                for i in range(8):
                    src = f"{'a' if i < 4 else 's'}.sd[{i % 4}]"
                    body.append(f"r.sw[{i}] = (int16_t)({src} > 32767 ? 32767 : {src} < -32768 ? -32768 : {src});")
            else:
                lo, hi, lane = (-128, 127, "sb") if m == "packsswb" else (0, 255, "b")
                for i in range(16):
                    src = f"{'a' if i < 8 else 's'}.sw[{i % 8}]"
                    body.append(f"r.{lane}[{i}] = ({src} > {hi} ? {hi} : {src} < {lo} ? {lo} : {src});")
            body.append(f"{d} = r;")
            return body

        # ---- integer lanes ----
        for prefix, kind in (("padd", "+"), ("psub", "-")):
            if m.startswith(prefix) and m[len(prefix):] in INT_LANES:
                lane, _, n, _ = INT_LANES[m[len(prefix):]]
                d = self.dst(insn, ops[0])
                return self.load(insn, ops[1], pre) + [
                    f"{d}.{lane}[{i}] = {d}.{lane}[{i}] {kind} s.{lane}[{i}];" for i in range(n)]
        for prefix, cmp in (("pcmpeq", "=="), ("pcmpgt", ">")):
            if m.startswith(prefix) and m[len(prefix):] in ("b", "w", "d"):
                lane, slane, n, bits = INT_LANES[m[len(prefix):]]
                d = self.dst(insn, ops[0])
                ones = hex((1 << bits) - 1)
                return self.load(insn, ops[1], pre) + [
                    f"{d}.{lane}[{i}] = ({d}.{slane}[{i}] {cmp} s.{slane}[{i}]) ? {ones} : 0;" for i in range(n)]
        minmax = {"pminub": ("b", "<"), "pmaxub": ("b", ">"), "pminsw": ("sw", "<"), "pmaxsw": ("sw", ">")}
        if m in minmax:
            lane, op = minmax[m]
            n = 16 if lane == "b" else 8
            d = self.dst(insn, ops[0])
            return self.load(insn, ops[1], pre) + [
                f"if (!({d}.{lane}[{i}] {op} s.{lane}[{i}])) {d}.{lane}[{i}] = s.{lane}[{i}];" for i in range(n)]
        if m == "pmuludq":
            d = self.dst(insn, ops[0])
            return self.load(insn, ops[1], pre) + [
                f"{d}.q[{i}] = (uint64_t){d}.d[{2 * i}] * s.d[{2 * i}];" for i in range(2)]
        if m in ("pmullw", "pmulhw", "pmulhuw"):
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre)
            for i in range(8):
                if m == "pmullw":
                    body.append(f"{d}.w[{i}] = (uint16_t)({d}.w[{i}] * s.w[{i}]);")
                elif m == "pmulhw":
                    body.append(f"{d}.w[{i}] = (uint16_t)(((int32_t){d}.sw[{i}] * s.sw[{i}]) >> 16);")
                else:
                    body.append(f"{d}.w[{i}] = (uint16_t)(((uint32_t){d}.w[{i}] * s.w[{i}]) >> 16);")
            return body
        if m in ("psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"):
            lane, slane, n, bits = INT_LANES[m[-1]]
            d = self.dst(insn, ops[0])
            if ops[1].type == X.X86_OP_IMM:
                body = [f"uint64_t k = {ops[1].imm & 0xFF};"]
            else:
                body = self.load(insn, ops[1], pre) + ["uint64_t k = s.q[0];"]
            for i in range(n):
                if m.startswith("psll"):
                    body.append(f"{d}.{lane}[{i}] = k >= {bits} ? 0 : {d}.{lane}[{i}] << k;")
                elif m.startswith("psrl"):
                    body.append(f"{d}.{lane}[{i}] = k >= {bits} ? 0 : {d}.{lane}[{i}] >> k;")
                else:
                    body.append(f"{d}.{slane}[{i}] = {d}.{slane}[{i}] >> (k >= {bits} ? {bits - 1} : k);")
            return body
        if m in ("pslldq", "psrldq"):
            d = self.dst(insn, ops[0])
            k = ops[1].imm & 0xFF
            body = [f"Xmm a = {d}, r = XZERO();"]
            for i in range(16):
                j = i - k if m == "pslldq" else i + k
                if 0 <= j < 16:
                    body.append(f"r.b[{i}] = a.b[{j}];")
            body.append(f"{d} = r;")
            return body
        if m == "pextrw":
            k = ops[2].imm & 7
            return [tr.write(ops[0], insn, pre, f"c->x[{xmm_index(insn, ops[1])}].w[{k}]")]
        if m == "pinsrw":
            k = ops[2].imm & 7
            v, _ = tr.read(ops[1], insn, pre)
            return [f"{self.dst(insn, ops[0])}.w[{k}] = (uint16_t)({v});"]
        raise NotSSE("not handled")
