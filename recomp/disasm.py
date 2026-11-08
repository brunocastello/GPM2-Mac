"""Capstone wrapper (x86-32, detail=True) with a per-VA cache.

`capstone` is imported only inside `Disasm.__init__`, so the rest of the package runs without it.
"""

from __future__ import annotations

from collections import namedtuple

# kind: 'r' register, 'i' immediate, 'm' memory
Op = namedtuple("Op", "kind size reg base index scale disp imm")

R8 = frozenset("al cl dl bl ah ch dh bh".split())
R16 = frozenset("ax cx dx bx sp bp si di".split())
R32 = frozenset("eax ecx edx ebx esp ebp esi edi".split())
SEG = frozenset("es cs ss ds fs gs".split())
# sub-register -> 32-bit register
FAMILY = {}
for _r32, _r16, _lo, _hi in (("eax", "ax", "al", "ah"), ("ecx", "cx", "cl", "ch"),
                             ("edx", "dx", "dl", "dh"), ("ebx", "bx", "bl", "bh")):
    FAMILY.update({_r32: _r32, _r16: _r32, _lo: _r32, _hi: _r32})
for _r32, _r16 in (("esp", "sp"), ("ebp", "bp"), ("esi", "si"), ("edi", "di")):
    FAMILY.update({_r32: _r32, _r16: _r32})

JCC_PREFIX = "j"
BRANCH_REL = frozenset(["call", "jmp", "jecxz", "jcxz", "loop", "loope", "loopne"])
PREFIX_NAMES = {0xF0: "lock", 0xF2: "repne", 0xF3: "rep"}
SEG_PREFIX = {0x26: "es", 0x2E: "cs", 0x36: "ss", 0x3E: "ds", 0x64: "fs", 0x65: "gs"}


class Ins:
    __slots__ = ("va", "size", "mn", "pfx", "ops", "op_str", "form")

    def __init__(self, va, size, mn, pfx, ops, op_str, form):
        self.va = va
        self.size = size
        self.mn = mn
        self.pfx = pfx
        self.ops = ops
        self.op_str = op_str
        self.form = form

    @property
    def end(self) -> int:
        return self.va + self.size

    def is_jcc(self) -> bool:
        return self.mn[0] == "j" and self.mn not in ("jmp",)

    def rel_target(self):
        """Absolute target of call/jmp/jcc with an immediate operand, or None."""
        if self.ops and self.ops[0].kind == "i" and (self.mn in BRANCH_REL or self.is_jcc()):
            return self.ops[0].imm & 0xFFFFFFFF
        return None

    def text(self) -> str:
        return ("%s %s" % (self.mn, self.op_str)).strip()


def _optype(op: Op, mn: str) -> str:
    if op.kind == "r":
        n = op.reg
        if n in R32:
            return "r32"
        if n in R16:
            return "r16"
        if n in R8:
            return "r8"
        if n.startswith("st"):
            return "st"
        if n in SEG:
            return "sreg"
        return "reg"
    if op.kind == "i":
        if mn in BRANCH_REL or mn[0] == "j":
            return "rel"
        return "imm%d" % (op.size * 8)
    return "m%d" % (op.size * 8) if op.size else "m"


class Disasm:
    def __init__(self, pe):
        from capstone import Cs, CS_ARCH_X86, CS_MODE_32
        from capstone.x86 import X86_OP_REG, X86_OP_IMM, X86_OP_MEM
        self._kinds = {X86_OP_REG: "r", X86_OP_IMM: "i", X86_OP_MEM: "m"}
        self.pe = pe
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.cache = {}

    def at(self, va: int):
        """Instruction at va, or None if invalid/no data. Result is cached."""
        if va in self.cache:
            return self.cache[va]
        ins = None
        code = self.pe.read(va, 15)
        if code:
            for ci in self.md.disasm(code, va, 1):
                ins = self._convert(ci, code)
                break
        self.cache[va] = ins
        return ins

    def _convert(self, ci, code) -> Ins:
        words = ci.mnemonic.split()
        mn = words[-1]
        pfx = list(words[:-1])
        if not pfx:
            p = ci.prefix
            if p[0] in PREFIX_NAMES:
                pfx.append(PREFIX_NAMES[p[0]])
            if p[1] in SEG_PREFIX:
                pfx.append(SEG_PREFIX[p[1]])
        ops = []
        for o in ci.operands:
            kind = self._kinds.get(o.type, "?")
            if kind == "r":
                ops.append(Op("r", o.size, ci.reg_name(o.reg), "", "", 0, 0, 0))
            elif kind == "i":
                ops.append(Op("i", o.size, "", "", "", 0, 0, o.imm))
            else:
                m = o.mem
                ops.append(Op("m", o.size, "",
                              ci.reg_name(m.base) if m.base else "",
                              ci.reg_name(m.index) if m.index else "",
                              m.scale, m.disp & 0xFFFFFFFF, 0))
        ops = tuple(ops)
        form = " ".join(pfx + [mn]) if not ops else (
            " ".join(pfx + [mn]) + " " + ",".join(_optype(o, mn) for o in ops))
        return Ins(ci.address, ci.size, mn, tuple(pfx), ops, ci.op_str, form)
