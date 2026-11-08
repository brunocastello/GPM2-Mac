"""x86 instruction -> C: one function per mnemonic/form.

Only the instructions that do not alter control flow; call/jmp/jcc/ret/switch stay in emit.py,
which reuses the operand helpers from here. Each translation returns a list
of C lines (a `{ ... }` block when it needs local variables). A mnemonic or
form with no translation raises `Untranslated`.

The semantics of flags and shifts live in runtime/machine.h|c (`alu_*`,
`op_*` functions); the generator only picks size and operands.
"""

from __future__ import annotations

from .disasm import R8, R16, R32, FAMILY

TRANSLATOR_VERSION = "1.0"


class Untranslated(Exception):
    pass


LOW8 = frozenset("al cl dl bl".split())
HIGH8 = frozenset("ah ch dh bh".split())

JCC = {
    "je": "c_e", "jz": "c_e", "jne": "c_ne", "jnz": "c_ne",
    "jl": "c_l", "jnge": "c_l", "jge": "c_ge", "jnl": "c_ge",
    "jle": "c_le", "jng": "c_le", "jg": "c_g", "jnle": "c_g",
    "ja": "c_a", "jnbe": "c_a", "jae": "c_ae", "jnb": "c_ae", "jnc": "c_ae",
    "jb": "c_b", "jnae": "c_b", "jc": "c_b", "jbe": "c_be", "jna": "c_be",
    "js": "c_s", "jns": "c_ns",
}
SETCC = {"set" + k[1:]: v for k, v in JCC.items()}

ALU2 = {"add": "alu_add", "sub": "alu_sub", "and": "alu_and", "or": "alu_or",
        "xor": "alu_xor", "adc": "alu_adc", "sbb": "alu_sbb"}
SHIFTS = {"shl": "op_shl", "sal": "op_shl", "shr": "op_shr", "sar": "op_sar"}


def bits(op) -> int:
    if op.kind == "r":
        n = op.reg
        if n in R32:
            return 32
        if n in R16:
            return 16
        if n in R8:
            return 8
        raise Untranslated("register " + n)
    if op.size not in (1, 2, 4, 8):
        raise Untranslated("operand size %r" % (op.size,))
    return op.size * 8


def hexu(v: int) -> str:
    return "0x%Xu" % (v & 0xFFFFFFFF)


def mask_of(sz: int) -> int:
    return (1 << sz) - 1


def mem_ea(op, fs=False) -> str:
    """C expression (uint32_t) of the effective address, with modulo 2^32 arithmetic."""
    parts = []
    if op.base:
        parts.append("g." + op.base)
    if op.index:
        parts.append("g.%s * %du" % (op.index, op.scale) if op.scale != 1 else "g." + op.index)
    if fs:
        parts.append("FS_BASE")
    if op.disp or not parts:
        parts.append(hexu(op.disp))
    return "(uint32_t)(%s)" % " + ".join(parts)


def rd(op, sz=None) -> str:
    """C read expression (value already truncated to the size). Memory uses the variable `ea`."""
    if op.kind == "i":
        return hexu(op.imm & mask_of(sz or bits(op)))
    if op.kind == "r":
        n = op.reg
        fam = "g." + FAMILY[n]
        if n in R32:
            return fam
        if n in R16:
            return "GET16(%s)" % fam
        return ("GET8L(%s)" if n in LOW8 else "GET8H(%s)") % fam
    b = bits(op)
    if b == 8:
        return "R8(ea)"
    if b == 16:
        return "R16(ea)"
    if b == 32:
        return "R32(ea)"
    raise Untranslated("%d-bit read" % b)


def wr(op, val: str) -> str:
    """C statement that writes `val` (uint32_t) to the operand."""
    if op.kind == "r":
        n = op.reg
        fam = "g." + FAMILY[n]
        if n in R32:
            return "%s = %s;" % (fam, val)
        if n in R16:
            return "SET16(%s, %s);" % (fam, val)
        return ("SET8L(%s, %s);" if n in LOW8 else "SET8H(%s, %s);") % (fam, val)
    if op.kind == "m":
        b = bits(op)
        if b in (8, 16, 32):
            return "W%d(ea, %s);" % (b, val)
    raise Untranslated("write to operand " + op.kind)


def block(decls, stmts):
    """Groups declarations and statements on one `{ ... }` line (or just the statements)."""
    if not decls:
        return [" ".join(stmts)]
    return ["{ %s %s }" % (" ".join(decls), " ".join(stmts))]


def _ea_decl(ops, fs):
    for o in ops:
        if o.kind == "m":
            return ["uint32_t ea = %s;" % mem_ea(o, fs)]
    return []


def _check_prefixes(ins, allowed):
    for p in ins.pfx:
        if p not in allowed:
            raise Untranslated("prefix " + p)


def _need(ins, n):
    if len(ins.ops) != n:
        raise Untranslated("%s with %d operands" % (ins.mn, len(ins.ops)))


def translate(ins) -> list:
    mn = ins.mn
    fn = _TABLE.get(mn)
    if fn is not None:
        return fn(ins)
    if mn in ALU2:
        return _alu(ins, ALU2[mn], True)
    if mn in SHIFTS:
        return _shift(ins, SHIFTS[mn])
    if mn in SETCC:
        return _setcc(ins)
    raise Untranslated(ins.form)


def _fsflag(ins) -> bool:
    return "fs" in ins.pfx


def _alu(ins, fn, write):
    _check_prefixes(ins, ("fs",))
    _need(ins, 2)
    d, s = ins.ops
    sz = bits(d)
    expr = "%s(%s, %s, %d)" % (fn, rd(d), rd(s, sz), sz)
    decls = _ea_decl(ins.ops, _fsflag(ins))
    return block(decls, [wr(d, expr) if write else expr + ";"])


def _cmp(ins):
    return _alu(ins, "alu_sub", False)


def _test(ins):
    return _alu(ins, "alu_and", False)


def _mov(ins):
    _check_prefixes(ins, ("fs",))
    _need(ins, 2)
    d, s = ins.ops
    sz = bits(d)
    return block(_ea_decl(ins.ops, _fsflag(ins)), [wr(d, rd(s, sz))])


def _movx(ins):
    _check_prefixes(ins, ("fs",))
    _need(ins, 2)
    d, s = ins.ops
    ssz = bits(s)
    if bits(d) not in (16, 32) or ssz not in (8, 16):
        raise Untranslated(ins.form)
    v = rd(s)
    if ins.mn == "movsx":
        v = ("SX8(%s)" if ssz == 8 else "SX16(%s)") % v
    return block(_ea_decl(ins.ops, _fsflag(ins)), [wr(d, v)])


def _lea(ins):
    _need(ins, 2)
    d, s = ins.ops
    if d.kind != "r" or bits(d) != 32 or s.kind != "m":
        raise Untranslated(ins.form)
    return [wr(d, mem_ea(s))]


def _push(ins):
    _need(ins, 1)
    o = ins.ops[0]
    if o.kind != "i" and bits(o) != 32:
        raise Untranslated(ins.form)
    return block(_ea_decl(ins.ops, _fsflag(ins)), ["PUSH(%s);" % rd(o, 32)])


def _pop(ins):
    _need(ins, 1)
    o = ins.ops[0]
    if o.kind == "i" or bits(o) != 32:
        raise Untranslated(ins.form)
    if o.kind == "r":
        return ["{ uint32_t v = R32(g.esp); g.esp += 4u; %s }" % wr(o, "v")]
    # the address of a memory destination is computed after the esp increment
    return ["{ uint32_t v = R32(g.esp), ea; g.esp += 4u; ea = %s; %s }" % (mem_ea(o, _fsflag(ins)), wr(o, "v"))]


def _leave(ins):
    return ["g.esp = g.ebp; g.ebp = R32(g.esp); g.esp += 4u;"]


def _nop(ins):
    return []


def _incdec(fn):
    def t(ins):
        _need(ins, 1)
        d = ins.ops[0]
        sz = bits(d)
        return block(_ea_decl(ins.ops, _fsflag(ins)), [wr(d, "%s(%s, %d)" % (fn, rd(d), sz))])
    return t


def _neg(ins):
    _need(ins, 1)
    d = ins.ops[0]
    sz = bits(d)
    return block(_ea_decl(ins.ops, _fsflag(ins)), [wr(d, "alu_neg(%s, %d)" % (rd(d), sz))])


def _not(ins):
    _need(ins, 1)
    d = ins.ops[0]
    sz = bits(d)
    v = "~%s" % rd(d) if sz == 32 else "(~%s) & %s" % (rd(d), hexu(mask_of(sz)))
    return block(_ea_decl(ins.ops, _fsflag(ins)), [wr(d, v)])


def _shift(ins, fn):
    if len(ins.ops) not in (1, 2):
        raise Untranslated(ins.form)
    d = ins.ops[0]
    c = ins.ops[1] if len(ins.ops) == 2 else None
    sz = bits(d)
    decls = _ea_decl([d], _fsflag(ins))
    if c is None:
        return block(decls, [wr(d, "%s(%s, 1u, %d)" % (fn, rd(d), sz))])
    if c.kind == "i":
        cnt = c.imm & 31
        if cnt == 0:
            return ["/* contagem 0: sem efeito */"]
        return block(decls, [wr(d, "%s(%s, %du, %d)" % (fn, rd(d), cnt, sz))])
    if c.kind == "r" and c.reg == "cl":
        return block(decls + ["uint32_t c = GET8L(g.ecx) & 31u;"],
                     ["if (c) { %s }" % wr(d, "%s(%s, c, %d)" % (fn, rd(d), sz))])
    raise Untranslated(ins.form)


def _imul(ins):
    if len(ins.ops) == 2:
        d, s = ins.ops
        src = d
    elif len(ins.ops) == 3:
        d, src, s = ins.ops
    else:
        raise Untranslated(ins.form)
    if d.kind != "r" or bits(d) != 32:
        raise Untranslated(ins.form)
    return block(_ea_decl(ins.ops, _fsflag(ins)), [wr(d, "op_imul32(%s, %s)" % (rd(src), rd(s, 32)))])


def _divide(fn):
    def t(ins):
        _need(ins, 1)
        o = ins.ops[0]
        if bits(o) != 32:
            raise Untranslated(ins.form)
        return block(_ea_decl(ins.ops, _fsflag(ins)), ["%s(%s, %s);" % (fn, rd(o), hexu(ins.va))])
    return t


def _cdq(ins):
    return ["g.edx = (g.eax & 0x80000000u) ? 0xFFFFFFFFu : 0u;"]


def _setcc(ins):
    _need(ins, 1)
    d = ins.ops[0]
    if bits(d) != 8:
        raise Untranslated(ins.form)
    return block(_ea_decl(ins.ops, _fsflag(ins)), [wr(d, "(uint32_t)%s()" % SETCC[ins.mn])])


# -- strings (DF = 0) ----------------------------------------------------------------------
def _string(kind):
    def t(ins):
        n = {"b": 1, "w": 2, "d": 4}[ins.mn[-1]]
        _check_prefixes(ins, ("rep", "repne"))
        pf = ins.pfx[0] if ins.pfx else ""
        if kind == "movs":
            if pf == "rep":
                return ["str_rep_movs(%d);" % n]
            if pf == "":
                return ["str_movs(%d);" % n]
        elif kind == "stos":
            if pf == "rep":
                return ["str_rep_stos(%d);" % n]
            if pf == "":
                return ["str_stos(%d);" % n]
        elif kind == "scas" and n == 1 and pf == "repne":
            return ["str_repne_scasb();"]
        raise Untranslated(ins.form)
    return t


# -- x87 (only 64-bit memory and fild m32) ---------------------------------------------------
def _fmem(sz):
    def deco(body):
        def t(ins):
            _need(ins, 1)
            o = ins.ops[0]
            if o.kind != "m" or o.size != sz:
                raise Untranslated(ins.form)
            return block(["uint32_t ea = %s;" % mem_ea(o, _fsflag(ins))], body)
        return t
    return deco


def _fnstsw(ins):
    _need(ins, 1)
    if ins.ops[0].kind != "r" or ins.ops[0].reg != "ax":
        raise Untranslated(ins.form)
    return ["SET16(g.eax, fx_status());"]


def _fchs(ins):
    return ["fx_chs();"]


_TABLE = {
    "mov": _mov, "movzx": _movx, "movsx": _movx, "lea": _lea,
    "push": _push, "pop": _pop, "leave": _leave, "nop": _nop,
    "cmp": _cmp, "test": _test,
    "inc": _incdec("alu_inc"), "dec": _incdec("alu_dec"), "neg": _neg, "not": _not,
    "imul": _imul, "div": _divide("op_div32"), "idiv": _divide("op_idiv32"), "cdq": _cdq,
    "movsb": _string("movs"), "movsw": _string("movs"), "movsd": _string("movs"),
    "stosb": _string("stos"), "stosw": _string("stos"), "stosd": _string("stos"),
    "scasb": _string("scas"),
    "fld": _fmem(8)(["fx_push(R64D(ea));"]),
    "fild": _fmem(4)(["fx_push((double)(int32_t)R32(ea));"]),
    "fst": _fmem(8)(["W64D(ea, fx_st0());"]),
    "fstp": _fmem(8)(["W64D(ea, fx_st0()); fx_pop();"]),
    "fmul": _fmem(8)(["fx_mul(R64D(ea));"]),
    "fdiv": _fmem(8)(["fx_div(R64D(ea));"]),
    "fcom": _fmem(8)(["fx_compare(R64D(ea));"]),
    "fcomp": _fmem(8)(["fx_compare(R64D(ea)); fx_pop();"]),
    "fchs": _fchs, "fnstsw": _fnstsw,
}
