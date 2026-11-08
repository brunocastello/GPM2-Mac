"""Discovery of functions, blocks, switches and indirect jumps in .text.

Depends on `capstone` (via recomp.disasm).
"""

from __future__ import annotations

import bisect

from .disasm import Disasm, FAMILY

NORETURN_IMPORTS = frozenset(["ExitProcess", "exit", "_exit"])
TERMINATORS = frozenset(["int3", "hlt", "ud2"])
RET_MNEMONICS = frozenset(["ret", "retf", "iret"])

CODE, JUMPTABLE, INDEXTABLE = 1, 2, 3
CLASS_NAMES = {0: "gap", CODE: "code", JUMPTABLE: "jumptable", INDEXTABLE: "indextable"}

MAX_UNBOUNDED_TABLE = 4096
PROLOGUES = (b"\x55\x8b\xec", b"\x55\x89\xe5", b"\xff\x25")


class Func:
    def __init__(self, va):
        self.va = va
        self.insns = {}          # va -> Ins
        self.leaders = {va}
        self.case = set()        # switch targets (never cut by E)
        self.calls = set()       # call rel32 targets (VAs)
        self.tailcalls = []      # (site, target, kind)
        self.switches = []       # dicts, see _switch
        self.indirects = []      # (site, 'call'|'jmp', operand)
        self.errors = []         # (va, bytes_hex, context)
        self.has_ret = False
        self.thunk = ""
        self.origin = set()
        self.blocks = []         # (start, end, n_insns, terminator)

    @property
    def end(self) -> int:
        return max(i.end for i in self.insns.values()) if self.insns else self.va

    @property
    def size_bytes(self) -> int:
        return sum(i.size for i in self.insns.values())

    @property
    def returns(self) -> bool:
        if self.thunk:
            return self.thunk.split("!")[-1] not in NORETURN_IMPORTS
        return self.has_ret or bool(self.tailcalls)


class Discovery:
    def __init__(self, pe):
        self.pe = pe
        self.D = Disasm(pe)
        t = pe.section_by_name(".text")
        self.t_lo = t["va"]
        self.t_hi = t["va"] + t["vsize"]
        self.iat = pe.iat_map()
        self.relocs = pe.relocs()
        self.reloc_locs = frozenset(l for l, _ in self.relocs)
        self.tables = {}         # T -> n_entries (jump tables)
        self.itables = {}        # T2 -> size (byte index tables)
        self.case_targets = set()
        self.funcs = {}
        self.E = {}
        self.coverage = bytearray()
        self.rounds = 0

    # -- utilities ---------------------------------------------------------
    def in_text(self, va) -> bool:
        return self.t_lo <= va < self.t_hi

    def thunk_name(self, va) -> str:
        """\"DLL!name\" if va starts with `jmp dword ptr [IAT]`, else ''."""
        ins = self.D.at(va)
        if ins is not None and ins.mn == "jmp" and ins.ops and ins.ops[0].kind == "m":
            o = ins.ops[0]
            if not o.base and not o.index and o.disp in self.iat:
                return self.iat[o.disp]
        return ""

    def iat_operand(self, ins):
        """Import name if operand 0 is an absolute `[IAT]`, else ''."""
        if ins.ops and ins.ops[0].kind == "m":
            o = ins.ops[0]
            if not o.base and not o.index and o.disp in self.iat:
                return self.iat[o.disp]
        return ""

    def _bytes_hex(self, va, n=8) -> str:
        return self.pe.read(va, n).hex()

    # -- exploring a function ---------------------------------------------
    def explore(self, entry, E) -> Func:
        f = Func(entry)
        insns, leaders = f.insns, f.leaders
        pending = [entry]
        while pending:
            a = pending.pop()
            recent = []
            while True:
                if a in insns:
                    break
                if a != entry and a in E and a not in f.case:
                    f.tailcalls.append((recent[-1].va if recent else a, a, "fall"))
                    break
                if not self.in_text(a):
                    f.errors.append((a, self._bytes_hex(a), "target outside .text"))
                    break
                ins = self.D.at(a)
                if ins is None:
                    f.errors.append((a, self._bytes_hex(a), "invalid instruction in reached code"))
                    break
                insns[a] = ins
                recent.append(ins)
                if len(recent) > 24:
                    del recent[0]
                mn = ins.mn
                nxt = ins.end
                if mn in RET_MNEMONICS:
                    f.has_ret = True
                    break
                if mn in TERMINATORS:
                    break
                if mn == "jmp":
                    t = ins.rel_target()
                    if t is not None:
                        if self.in_text(t) and t != entry and self.thunk_name(t):
                            f.calls.add(t)   # jmp to an import thunk = tail call
                            f.tailcalls.append((a, t, "jmp"))
                        elif t in E and t != entry:
                            f.tailcalls.append((a, t, "jmp"))
                        elif self.in_text(t):
                            leaders.add(t)
                            pending.append(t)
                        else:
                            f.errors.append((a, self._bytes_hex(a), "jmp outside .text"))
                    else:
                        self._indirect_jmp(f, ins, recent, pending)
                    break
                if ins.is_jcc():
                    t = ins.rel_target()
                    leaders.add(nxt)
                    if t is not None:
                        if t in E and t != entry:
                            f.tailcalls.append((a, t, "jcc"))
                        elif self.in_text(t):
                            leaders.add(t)
                            pending.append(t)
                        else:
                            f.errors.append((a, self._bytes_hex(a), "jcc outside .text"))
                    a = nxt
                    continue
                if mn == "call":
                    t = ins.rel_target()
                    noret = False
                    if t is not None:
                        if self.in_text(t):
                            f.calls.add(t)
                            noret = self.thunk_name(t).split("!")[-1] in NORETURN_IMPORTS
                        else:
                            f.errors.append((a, self._bytes_hex(a), "call outside .text"))
                    else:
                        imp = self.iat_operand(ins)
                        if imp:
                            noret = imp.split("!")[-1] in NORETURN_IMPORTS
                        else:
                            f.indirects.append((a, "call", ins.op_str))
                    if noret:
                        break
                a = nxt
        if len(f.insns) == 1:
            only = f.insns[entry]
            nm = self.iat_operand(only) if only.mn == "jmp" else ""
            if nm:
                f.thunk = nm
                f.tailcalls = []
        self._blocks(f)
        return f

    def _indirect_jmp(self, f, ins, recent, pending):
        o = ins.ops[0] if ins.ops else None
        if o is not None and o.kind == "m":
            imp = self.iat_operand(ins)
            if imp:
                f.tailcalls.append((ins.va, imp, "import"))
                return
            if not o.base and o.index and o.scale == 4 and self.in_text(o.disp):
                self._switch(f, ins, o, recent, pending)
                return
        f.indirects.append((ins.va, "jmp", ins.op_str))

    def _switch(self, f, ins, o, recent, pending):
        """`jmp dword ptr [4*reg + T]`: finds the bound and the targets."""
        T = o.disp
        idx_fam = FAMILY.get(o.index, o.index)
        bound = None      # N from `cmp reg, N` + ja
        bound_n = None    # number of entries indicated by the cmp
        itable = None
        # search (backwards) for the byte load and the cmp/ja pair
        seen_ja = None
        for r in reversed(recent[:-1]):
            if itable is None and r.mn in ("movzx", "mov", "movsx") and len(r.ops) == 2 \
                    and r.ops[0].kind == "r" and r.ops[1].kind == "m" and r.ops[1].size == 1 \
                    and FAMILY.get(r.ops[0].reg) == idx_fam and self.in_text(r.ops[1].disp):
                itable = r.ops[1].disp
                continue
            if r.is_jcc() and seen_ja is None and r.mn in ("ja", "jae", "jnb", "jg", "jge"):
                seen_ja = r.mn
                continue
            if seen_ja is not None and r.mn == "cmp" and len(r.ops) == 2 \
                    and r.ops[1].kind == "i":
                size = r.ops[0].size or 4
                n = r.ops[1].imm & ((1 << (8 * size)) - 1)
                bound = n
                bound_n = n + 1 if seen_ja in ("ja", "jg") else n
                break
        index_size = 0
        if itable is not None and bound_n is not None:
            index_size = bound_n
            idx = self.pe.read(itable, index_size)
            limit = (max(idx) + 1) if idx else 0
            if T < itable:
                # the compiler appends the default entry after the cases
                limit = max(limit, (itable - T) // 4)
            src = "cmp"
        elif bound_n is not None:
            limit, src = bound_n, "cmp"
        else:
            limit, src = MAX_UNBOUNDED_TABLE, "reloc"
        targets = []
        for k in range(limit):
            loc = T + 4 * k
            v = self.pe.read_u32(loc)
            if loc not in self.reloc_locs or v is None or not self.in_text(v):
                if src == "cmp" and k < (bound_n or 0) and itable is None:
                    src = "reloc"   # table ends before the cmp bound
                break
            targets.append(v)
        if itable is not None and index_size == 0:
            itable = None
        f.switches.append({"site": ins.va, "reg": o.index, "table": T, "bound_source": src,
                           "index_table": itable, "index_size": index_size, "targets": targets,
                           "cmp_bound": bound})
        self.tables[T] = max(self.tables.get(T, 0), len(targets))
        if itable is not None:
            self.itables[itable] = max(self.itables.get(itable, 0), index_size)
        for t in targets:
            self.case_targets.add(t)
            f.leaders.add(t)
            f.case.add(t)
            pending.append(t)

    def _blocks(self, f):
        blocks = []
        cur = None
        prev_end = None
        for a in sorted(f.insns):
            ins = f.insns[a]
            if cur is None or a in f.leaders or a != prev_end:
                if cur is not None:
                    blocks.append(cur)
                cur = [a, a, 0, None]
            cur[1] = ins.end
            cur[2] += 1
            cur[3] = ins
            prev_end = ins.end
        if cur is not None:
            blocks.append(cur)
        out = []
        for start, end, n, last in blocks:
            ctrl = (last.mn in RET_MNEMONICS or last.mn in TERMINATORS or last.mn in ("jmp", "call")
                    or last.is_jcc())
            out.append((start, end, n, last.mn if ctrl else "fall"))
        f.blocks = out

    # -- seeds -------------------------------------------------------------
    def _table_ranges(self):
        rs = [(t, t + 4 * n) for t, n in self.tables.items()]
        rs += [(t, t + n) for t, n in self.itables.items()]
        rs.sort()
        return [r[0] for r in rs], rs

    def reloc_seeds(self):
        starts, rs = self._table_ranges()

        def in_table(va):
            i = bisect.bisect_right(starts, va) - 1
            return i >= 0 and rs[i][0] <= va < rs[i][1]

        out = set()
        for loc, val in self.relocs:
            if not self.in_text(val) or val in self.case_targets:
                continue
            if in_table(loc) or in_table(val):
                continue
            out.add(val)
        return out

    def scan_seeds(self, covered):
        """Prologues/thunks not yet covered (dead code, unreferenced)."""
        lo = self.t_lo
        text = self.pe.read(lo, self.t_hi - lo)
        for t, n in self.tables.items():
            covered[t - lo:t - lo + 4 * n] = b"\x02" * (4 * n)
        for t, n in self.itables.items():
            covered[t - lo:t - lo + n] = b"\x03" * n
        hits = set()
        for pat in PROLOGUES:
            i = text.find(pat)
            while i >= 0:
                hits.add(i)
                i = text.find(pat, i + 1)
        out = []
        for i in sorted(hits):
            if covered[i]:
                continue
            if not (i == 0 or text[i - 1] in (0xCC, 0x90, 0x00) or covered[i - 1]):
                continue
            a = lo + i
            if text[i] == 0xFF:
                v = self.pe.read_u32(a + 2)
                if v not in self.iat:
                    continue
            if self.D.at(a) is None:
                continue
            out.append(a)
        return out

    # -- fixed point -----------------------------------------------------------
    def run(self):
        entry = self.pe.entry_va
        while True:
            self.rounds += 1
            snap = (dict(self.tables), dict(self.itables))
            E = {entry: {"entry"}}
            work = [entry]
            seen = set()
            covered = bytearray(self.t_hi - self.t_lo)
            while True:
                while work:
                    va = work.pop()
                    if va in seen:
                        continue
                    seen.add(va)
                    f = self.explore(va, E)
                    for i in f.insns.values():
                        covered[i.va - self.t_lo:i.end - self.t_lo] = b"\x01" * i.size
                    for t in f.calls:
                        if t in E:
                            E[t].add("call")
                        else:
                            E[t] = {"call"}
                            work.append(t)
                for v in sorted(self.reloc_seeds()):
                    if v in E:
                        E[v].add("reloc")
                    else:
                        E[v] = {"reloc"}
                        work.append(v)
                if work:
                    continue
                for v in self.scan_seeds(covered):
                    if v not in E:
                        E[v] = {"scan"}
                        work.append(v)
                if not work:
                    break
            funcs = {}
            for va in sorted(E):
                f = self.explore(va, E)
                f.origin = E[va]
                funcs[va] = f
                for t in f.calls:
                    if t in E:
                        E[t].add("call")
            if (dict(self.tables), dict(self.itables)) == snap:
                self.funcs, self.E = funcs, E
                break
        self._coverage()
        return self

    def _coverage(self):
        cov = bytearray(self.t_hi - self.t_lo)
        self.coverage_errors = []

        def mark(lo, hi, cls, what):
            for a in range(lo, hi):
                if not (self.t_lo <= a < self.t_hi):
                    continue
                cur = cov[a - self.t_lo]
                if cur and cur != cls:
                    self.coverage_errors.append((a, self._bytes_hex(a, 4),
                                                 "%s over %s" % (what, CLASS_NAMES[cur])))
                    return
                cov[a - self.t_lo] = cls

        for f in self.funcs.values():
            for i in f.insns.values():
                mark(i.va, i.end, CODE, "code")
        for t, n in self.tables.items():
            mark(t, t + 4 * n, JUMPTABLE, "jumptable")
        for t, n in self.itables.items():
            mark(t, t + n, INDEXTABLE, "indextable")
        self.coverage = cov

    def class_at(self, va) -> str:
        return CLASS_NAMES[self.coverage[va - self.t_lo]]

    def gaps(self):
        """Unclassified ranges: (start, end, kind, first_bytes_hex)."""
        out = []
        cov, base = self.coverage, self.t_lo
        i, n = 0, len(cov)
        while i < n:
            if cov[i] != 0:
                i += 1
                continue
            j = i
            while j < n and cov[j] == 0:
                j += 1
            raw = self.pe.read(base + i, j - i)
            kind = "padding" if all(b in (0xCC, 0x90, 0x00) for b in raw) else "unknown"
            out.append((base + i, base + j, kind, raw[:16].hex()))
            i = j
        return out
