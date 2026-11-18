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
# The 32 AVX compare predicates (imm 0-31); 16-31 only differ in which NaNs
# raise exceptions, which is not modelled.
AVX_PRED = ["a == b", "a < b", "a <= b", "(a != a || b != b)", "!(a == b)", "!(a < b)", "!(a <= b)",
            "(a == a && b == b)", "(a == b || a != a || b != b)", "!(a >= b)", "!(a > b)", "0",
            "(a < b || a > b)", "a >= b", "a > b", "1"]
AVX_PRED_NAMES = ["eq_oq", "lt_os", "le_os", "unord_q", "neq_uq", "nlt_us", "nle_us", "ord_q",
                  "eq_uq", "nge_us", "ngt_us", "false_oq", "neq_oq", "ge_os", "gt_os", "true_uq",
                  "eq_os", "lt_oq", "le_oq", "unord_s", "neq_us", "nlt_uq", "nle_uq", "ord_s",
                  "eq_us", "nge_uq", "ngt_uq", "false_os", "neq_os", "ge_oq", "gt_oq", "true_us"]
for _i, _n in enumerate(AVX_PRED_NAMES):
    CMP_PRED.setdefault(_n, AVX_PRED[_i % 16])
for _n, _e in (("nge", AVX_PRED[9]), ("ngt", AVX_PRED[10]), ("false", "0"), ("ge", AVX_PRED[13]),
               ("gt", AVX_PRED[14]), ("true", "1")):
    CMP_PRED.setdefault(_n, _e)


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
                pred = AVX_PRED_NAMES[ops[2].imm & 31]
            if pred in CMP_PRED:
                lane, n = LANES[m[-2:]]
                ilane = "d" if lane == "f" else "q"
                ones = "0xffffffffu" if lane == "f" else "~0ull"
                d = self.dst(insn, ops[0])
                body = self.load(insn, ops[1], pre)
                body.append(f"Xmm t = {d};")
                for i in range(n):
                    cond = CMP_PRED[pred].replace("a", f"t.{lane}[{i}]").replace("b", f"s.{lane}[{i}]")
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
        r = self.translate_sse4(insn, m, ops, pre)
        if r is not None:
            return r
        raise NotSSE("not handled")

    def translate_sse4(self, insn, m, ops, pre):
        """SSE3, SSSE3 and SSE4.1: in every x86-64 CPU of the last ten years,
        and in the PS4 and PS5 CPUs."""
        tr = self.tr
        imm = ops[-1].imm & 0xFF if ops and ops[-1].type == X.X86_OP_IMM else None

        def two(lane, n, expr):
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre) + [f"Xmm a = {d};"]
            for i in range(n):
                body.append(f"{d}.{lane}[{i}] = " + expr.format(a=f"a.{lane}[{i}]", b=f"s.{lane}[{i}]", i=i) + ";")
            return body

        minmax = {}
        for t, (lane, n) in {"sb": ("sb", 16), "ub": ("b", 16), "sw": ("sw", 8), "uw": ("w", 8),
                             "sd": ("sd", 4), "ud": ("d", 4)}.items():
            minmax["pmin" + t] = (lane, n, "<")
            minmax["pmax" + t] = (lane, n, ">")
        if m in minmax:
            lane, n, op = minmax[m]
            return two(lane, n, "({a} %s {b} ? {a} : {b})" % op)
        if m == "pmulld":
            return two("d", 4, "(uint32_t)({a} * {b})")
        if m == "pmuldq":
            d = self.dst(insn, ops[0])
            return self.load(insn, ops[1], pre) + [
                f"{d}.sq[{i}] = (int64_t){d}.sd[{2 * i}] * s.sd[{2 * i}];" for i in range(2)]
        if m in ("pcmpeqq", "pcmpgtq"):
            op = "==" if m == "pcmpeqq" else ">"
            return two("sq", 2, "(({a} %s {b}) ? -1 : 0)" % op)
        if m in ("pabsb", "pabsw", "pabsd"):
            lane, n = {"pabsb": ("sb", 16), "pabsw": ("sw", 8), "pabsd": ("sd", 4)}[m]
            ulane = lane[1:]
            d = self.dst(insn, ops[0])
            return self.load(insn, ops[1], pre) + [
                f"{d}.{ulane}[{i}] = s.{lane}[{i}] < 0 ? 0u - (uint64_t)s.{lane}[{i}] : (uint64_t)s.{lane}[{i}];"
                for i in range(n)]
        if m == "pshufb":
            d = self.dst(insn, ops[0])
            return self.load(insn, ops[1], pre) + [f"Xmm a = {d};"] + [
                f"{d}.b[{i}] = (s.b[{i}] & 0x80) ? 0 : a.b[s.b[{i}] & 15];" for i in range(16)]
        if m == "palignr":
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre) + [f"Xmm a = {d};", "uint8_t t[32];",
                                                   "memcpy(t, s.b, 16); memcpy(t + 16, a.b, 16);"]
            for i in range(16):
                j = i + imm
                body.append(f"{d}.b[{i}] = {'t[%d]' % j if j < 32 else '0'};")
            return body
        if m in ("blendps", "blendpd", "pblendw"):
            lane, n = {"blendps": ("d", 4), "blendpd": ("q", 2), "pblendw": ("w", 8)}[m]
            d = self.dst(insn, ops[0])
            return self.load(insn, ops[1], pre) + [
                f"{d}.{lane}[{i}] = s.{lane}[{i}];" for i in range(n) if (imm >> i) & 1]
        if m in ("blendvps", "blendvpd", "pblendvb"):
            # The mask is XMM0 (legacy form) or the fourth operand (AVX form, see avx.py).
            lane, n, sign = {"blendvps": ("d", 4, 31), "blendvpd": ("q", 2, 63), "pblendvb": ("b", 16, 7)}[m]
            mask = f"c->x[{xmm_index(insn, ops[2])}]" if len(ops) > 2 else "c->x[0]"
            d = self.dst(insn, ops[0])
            return self.load(insn, ops[1], pre) + [f"Xmm k = {mask};"] + [
                f"if ((k.{lane}[{i}] >> {sign}) & 1) {d}.{lane}[{i}] = s.{lane}[{i}];" for i in range(n)]
        if m in ("roundss", "roundsd", "roundps", "roundpd"):
            lane, n = LANES[m[-2:]]
            fnm = "ROUNDF" if lane == "f" else "ROUNDD"
            mode = 0 if imm & 4 else imm & 3
            d = self.dst(insn, ops[0])
            return self.load(insn, ops[1], pre) + [f"{d}.{lane}[{i}] = {fnm}(s.{lane}[{i}], {mode});" for i in range(n)]
        if m in ("movddup", "movshdup", "movsldup"):
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre) + ["Xmm r;"]
            if m == "movddup":
                body.append("r.q[0] = r.q[1] = s.q[0];")
            else:
                k = 1 if m == "movshdup" else 0
                body.append(f"r.d[0] = r.d[1] = s.d[{k}]; r.d[2] = r.d[3] = s.d[{2 + k}];")
            return body + [f"{d} = r;"]
        if m in ("haddps", "haddpd", "hsubps", "hsubpd", "phaddd", "phaddw", "phsubd", "phsubw"):
            lane, n = {"ps": ("f", 4), "pd": ("g", 2), "dd": ("d", 4), "dw": ("w", 8),
                       "bd": ("d", 4), "bw": ("w", 8)}[m[-2:]]
            op = "-" if "sub" in m else "+"
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre) + [f"Xmm a = {d}, r;"]
            half = n // 2
            for i in range(half):
                body.append(f"r.{lane}[{i}] = a.{lane}[{2 * i}] {op} a.{lane}[{2 * i + 1}];")
                body.append(f"r.{lane}[{half + i}] = s.{lane}[{2 * i}] {op} s.{lane}[{2 * i + 1}];")
            return body + [f"{d} = r;"]
        if m in ("addsubps", "addsubpd"):
            lane, n = LANES[m[-2:]]
            return self._addsub(insn, ops, pre, lane, n)
        if m == "pmaddubsw":
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre) + [f"Xmm a = {d};"]
            for i in range(8):
                body.append(f"{{ int32_t v = (int32_t)a.b[{2 * i}] * s.sb[{2 * i}] + (int32_t)a.b[{2 * i + 1}] * s.sb[{2 * i + 1}]; "
                            f"{d}.sw[{i}] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }}")
            return body
        if m in ("psllvd", "psllvq", "psrlvd", "psrlvq", "psravd"):
            lane, slane, n, bits = INT_LANES[m[-1]]
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre)
            for i in range(n):
                k = f"s.{lane}[{i}]"
                if m.startswith("psll"):
                    body.append(f"{d}.{lane}[{i}] = {k} >= {bits} ? 0 : {d}.{lane}[{i}] << {k};")
                elif m.startswith("psrl"):
                    body.append(f"{d}.{lane}[{i}] = {k} >= {bits} ? 0 : {d}.{lane}[{i}] >> {k};")
                else:
                    body.append(f"{d}.{slane}[{i}] = {d}.{slane}[{i}] >> ({k} >= {bits} ? {bits - 1} : {k});")
            return body
        if m == "pmaddwd":
            d = self.dst(insn, ops[0])
            return self.load(insn, ops[1], pre) + [f"Xmm a = {d};"] + [
                f"{d}.sd[{i}] = (int32_t)((int64_t)a.sw[{2 * i}] * s.sw[{2 * i}] + (int64_t)a.sw[{2 * i + 1}] * s.sw[{2 * i + 1}]);"
                for i in range(4)]
        if m == "packusdw":
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre) + [f"Xmm a = {d}, r;"]
            for i in range(8):
                src = f"{'a' if i < 4 else 's'}.sd[{i % 4}]"
                body.append(f"r.w[{i}] = ({src} > 65535 ? 65535 : {src} < 0 ? 0 : {src});")
            return body + [f"{d} = r;"]
        if m.startswith(("pmovzx", "pmovsx")) and len(m) == 8:
            sizes = {"b": 1, "w": 2, "d": 4, "q": 8}
            src_size, dst_size = sizes[m[6]], sizes[m[7]]
            n = 16 // dst_size
            signed = m[4] == "s"
            slane = {1: "b", 2: "w", 4: "d"}[src_size]
            dlane = {2: "w", 4: "d", 8: "q"}[dst_size]
            if signed:
                slane = "s" + slane
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre) + ["Xmm r;"]
            for i in range(n):
                body.append(f"r.{dlane}[{i}] = (uint64_t)(int64_t)s.{slane}[{i}];" if signed
                            else f"r.{dlane}[{i}] = s.{slane}[{i}];")
            return body + [f"{d} = r;"]
        if m == "ptest":
            body = self.load(insn, ops[1], pre)
            a = f"c->x[{xmm_index(insn, ops[0])}]"
            return body + [f"c->zf = (({a}.q[0] & s.q[0]) | ({a}.q[1] & s.q[1])) == 0;",
                           f"c->cf = ((~{a}.q[0] & s.q[0]) | (~{a}.q[1] & s.q[1])) == 0;",
                           "c->of = c->sf = c->pf = 0;"]
        if m in ("pextrb", "pextrd", "pextrq", "extractps"):
            lane = {"pextrb": "b", "pextrd": "d", "pextrq": "q", "extractps": "d"}[m]
            n = {"b": 16, "d": 4, "q": 2}[lane]
            v = f"c->x[{xmm_index(insn, ops[1])}].{lane}[{imm % n}]"
            return [tr.write(ops[0], insn, pre, v)]
        if m in ("pinsrb", "pinsrd", "pinsrq"):
            lane = m[-1]
            n = {"b": 16, "d": 4, "q": 2}[lane]
            v, _ = tr.read(ops[1], insn, pre) if not self.is_xmm(insn, ops[1]) else (None, 0)
            return [f"{self.dst(insn, ops[0])}.{lane}[{imm % n}] = {v};"]
        if m == "insertps":
            d = self.dst(insn, ops[0])
            if self.is_xmm(insn, ops[1]):
                src = f"c->x[{xmm_index(insn, ops[1])}].d[{(imm >> 6) & 3}]"
            else:
                src = f"RD32({tr.addr_var(ops[1], insn, pre)})"
            body = [f"{d}.d[{(imm >> 4) & 3}] = {src};"]
            body += [f"{d}.d[{i}] = 0;" for i in range(4) if (imm >> i) & 1]
            return body
        if m in ("dpps", "dppd"):
            lane, n = LANES[m[-2:]]
            d = self.dst(insn, ops[0])
            body = self.load(insn, ops[1], pre) + [f"Xmm a = {d};", f"{'float' if lane == 'f' else 'double'} t = 0;"]
            for i in range(n):
                if (imm >> (4 + i)) & 1:
                    body.append(f"t += a.{lane}[{i}] * s.{lane}[{i}];")
            for i in range(n):
                body.append(f"{d}.{lane}[{i}] = {'t' if (imm >> i) & 1 else '0'};")
            return body
        return None

    def _addsub(self, insn, ops, pre, lane, n):
        d = self.dst(insn, ops[0])
        return self.load(insn, ops[1], pre) + [
            f"{d}.{lane}[{i}] = {d}.{lane}[{i}] {'+' if i & 1 else '-'} s.{lane}[{i}];" for i in range(n)]
