"""Discovered functions -> C files.

Control flow (labels, goto, switch, call/ret) lives here; the simple instructions
come from `translate.py`. Nothing is written if any instruction is left untranslated.
"""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

from .discover import NORETURN_IMPORTS
from .hooks import Hooks
from .importsig import pop_bytes
from .translate import (JCC, TRANSLATOR_VERSION, Untranslated, bits, hexu, mem_ea, rd,
                        translate)

N_FILES = 64


def fname(va: int) -> str:
    return "f_%08x" % va


def lname(va: int) -> str:
    return "L_%08x" % va


def _san(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


class FuncMeta:
    __slots__ = ("calls", "imports", "indirect", "regjmp", "forms", "insns")

    def __init__(self):
        self.calls = set()      # functions (not thunks) reached by rel call / tail
        self.imports = False    # calls an import (thunk or [IAT])
        self.indirect = False   # indirect call/jump (dispatch)
        self.regjmp = False     # `jmp reg` (return by jump from _chkstk)
        self.forms = set()
        self.insns = []         # (va, form)


class Emitter:
    def __init__(self, disc, hooks_path=None):
        self.disc = disc
        self.pe = disc.pe
        self.funcs = disc.funcs
        self.hooks = Hooks.load(self.pe, hooks_path)
        self.hooks.verify(self.pe, self.funcs)
        self.errors = {}        # form -> [count, first va, message]
        self.soft_errors = []   # (va, message) of unknown targets etc.
        self.meta = {}
        self.texts = {}         # va -> C text of the function
        self.n_insns = 0
        self._build_imports()

    # -- imports ------------------------------------------------------------------
    def _build_imports(self):
        self.imports = self.pe.imports()
        self.imp_ident = []     # by index (same order as pe.imports())
        self.imp_by_iat = {}
        self.imp_by_label = {}
        used = set()
        for i, imp in enumerate(self.imports):
            dll = imp["dll"]
            base = dll.rsplit(".", 1)[0] if "." in dll else dll
            nm = imp["name"]
            nm = "ord" + nm[1:] if nm.startswith("#") else nm
            ident = "imp_%s_%s" % (_san(base), _san(nm))
            if ident in used:
                ident += "_%d" % i
            used.add(ident)
            label = "%s!%s" % (dll, imp["name"])
            self.imp_ident.append((ident, label))
            self.imp_by_iat[imp["iat"]] = (ident, label)
            self.imp_by_label[label] = ident

    # -- targets ------------------------------------------------------------------------
    def _target(self, meta, va, site):
        """(C identifier, is_import) of a call/tail target, or None."""
        f = self.funcs.get(va)
        if f is None:
            self.soft_errors.append((site, "target %#x is not a function" % va))
            return None
        if f.thunk:
            meta.imports = True
            return self.imp_by_label[f.thunk], True
        meta.calls.add(va)
        return fname(va), False

    def _record(self, ins, msg):
        e = self.errors.setdefault(ins.form, [0, ins.va, msg])
        e[0] += 1

    # -- one function -----------------------------------------------------------------------
    def emit_function(self, f):
        meta = FuncMeta()
        self.meta[f.va] = meta
        name = fname(f.va)
        if f.va in self.hooks.funcs:
            name += "_orig"
        if f.thunk:
            meta.imports = True
            ins = f.insns[f.va]
            meta.forms.add(ins.form)
            meta.insns.append((f.va, ins.form))
            self.n_insns += 1
            return "void %s(void) {\n    // 0x%08x: %s\n    %s();\n}\n" % (
                name, f.va, ins.text(), self.imp_by_label[f.thunk])
        vas = sorted(f.insns)
        sw_by_site = {s["site"]: s for s in f.switches}
        labels = set()
        body = []
        for idx, va in enumerate(vas):
            ins = f.insns[va]
            self.n_insns += 1
            meta.forms.add(ins.form)
            meta.insns.append((va, ins.form))
            lines, terminal = self._ins(f, ins, meta, labels, sw_by_site)
            hp = self.hooks.points.get(va)
            if hp:
                if hp.get("skip"):
                    lines.insert(0, "if (hookpt_%s()) goto %s;" % (hp["name"], lname(hp["skip"])))
                    labels.add(hp["skip"])
                else:
                    lines.insert(0, "hookpt_%s();" % hp["name"])
            nextva = vas[idx + 1] if idx + 1 < len(vas) else None
            if not terminal:
                nxt = ins.end
                if nxt in f.insns:
                    if nextva != nxt:
                        lines.append("goto %s;" % lname(nxt))
                        labels.add(nxt)
                elif nxt in self.funcs:
                    t = self._target(meta, nxt, ins.va)
                    if t:
                        lines.append("%s(); return;" % t[0])
                else:
                    lines.append('guest_trap("fall-through", %s);' % hexu(ins.va))
            body.append((va, ins, lines))
        out = ["void %s(void) {" % name]
        for va, ins, lines in body:
            if va in labels:
                out.append("%s:" % lname(va))
            out.append("    // 0x%08x: %s" % (va, ins.text()))
            for ln in lines:
                out.append("    " + ln)
        out.append("}")
        return "\n".join(out) + "\n"

    def _ins(self, f, ins, meta, labels, sw_by_site):
        """(lines, terminal)"""
        mn = ins.mn
        try:
            if mn in ("ret",):
                n = ins.ops[0].imm & 0xFFFF if ins.ops else 0
                return ["{ uint32_t ra = R32(g.esp); g.esp += %du; RETCHK(ra); return; }" % (4 + n)], True
            if mn == "call":
                return self._call(ins, meta)
            if mn == "jmp":
                return self._jmp(f, ins, meta, labels, sw_by_site), True
            if ins.is_jcc():
                return self._jcc(f, ins, meta, labels), False
            if mn in ("int3", "hlt", "ud2"):
                return ['guest_trap("%s", %s);' % (mn, hexu(ins.va))], True
            return translate(ins), False
        except Untranslated as ex:
            self._record(ins, str(ex))
            return ["/* no translation */"], True

    def _call(self, ins, meta):
        nxt = hexu(ins.end)
        o = ins.ops[0] if ins.ops else None
        if o is None:
            raise Untranslated(ins.form)
        noret = False
        if o.kind == "i":
            t = self._target(meta, ins.rel_target(), ins.va)
            if t is None:
                return ["guest_trap(\"call\", %s);" % hexu(ins.va)], True
            ident, is_imp = t
            if is_imp:
                noret = self.disc.thunk_name(ins.rel_target()).split("!")[-1] in NORETURN_IMPORTS
            lines = ["CALLPUSH(%s); %s();" % (nxt, ident)]
        else:
            imp = self.disc.iat_operand(ins)
            if imp:
                meta.imports = True
                noret = imp.split("!")[-1] in NORETURN_IMPORTS
                lines = ["CALLPUSH(%s); %s();" % (nxt, self.imp_by_label[imp])]
            else:
                meta.indirect = True
                if o.kind == "m":
                    lines = ["{ uint32_t ea = %s, t = R32(ea); CALLPUSH(%s); dispatch(t); }"
                             % (mem_ea(o, "fs" in ins.pfx), nxt)]
                elif o.kind == "r" and bits(o) == 32:
                    lines = ["{ uint32_t t = %s; CALLPUSH(%s); dispatch(t); }" % (rd(o), nxt)]
                else:
                    raise Untranslated(ins.form)
        if noret:
            lines.append("return;")
        return lines, noret

    def _tail(self, meta, t, site):
        r = self._target(meta, t, site)
        if r is None:
            return 'guest_trap("tail", %s);' % hexu(site)
        return "%s(); return;" % r[0]

    def _jmp(self, f, ins, meta, labels, sw_by_site):
        o = ins.ops[0] if ins.ops else None
        if o is None:
            raise Untranslated(ins.form)
        if o.kind == "i":
            t = ins.rel_target()
            if t in f.insns and not (t != f.va and t in self.funcs):
                labels.add(t)
                return ["goto %s;" % lname(t)]
            return [self._tail(meta, t, ins.va)]
        if o.kind == "m":
            imp = self.disc.iat_operand(ins)
            if imp:
                meta.imports = True
                return ["%s(); return;" % self.imp_by_label[imp]]
            sw = sw_by_site.get(ins.va)
            if sw is not None:
                return self._switch(f, ins, sw, labels)
            meta.indirect = True
            return ["{ uint32_t ea = %s; dispatch_jump(R32(ea)); return; }" % mem_ea(o, "fs" in ins.pfx)]
        if o.kind == "r" and bits(o) == 32:
            # `jmp reg`: target known in the function -> goto; otherwise return by jump (_chkstk) or tail
            meta.regjmp = True
            starts = sorted(b[0] for b in f.blocks)
            lines = ["{ uint32_t t = %s;" % rd(o), "  switch (t) {"]
            for s in starts:
                labels.add(s)
                lines.append("  case %s: goto %s;" % (hexu(s), lname(s)))
            lines += ["  default: dispatch_jump(t); return;", "  } }"]
            return lines
        raise Untranslated(ins.form)

    def _switch(self, f, ins, sw, labels):
        groups = {}
        order = []
        for k, t in enumerate(sw["targets"]):
            if t not in f.insns:
                self.soft_errors.append((ins.va, "switch target %#x outside the function" % t))
                continue
            if t not in groups:
                groups[t] = []
                order.append(t)
            groups[t].append(k)
            labels.add(t)
        reg = "g." + sw["reg"]
        lines = ["switch (%s) {" % reg]
        for t in order:
            cases = " ".join("case %du:" % k for k in groups[t])
            lines.append("%s goto %s;" % (cases, lname(t)))
        lines.append('default: guest_trap("switch", %s);' % reg)
        lines.append("}")
        return lines

    def _jcc(self, f, ins, meta, labels):
        cond = JCC.get(ins.mn)
        t = ins.rel_target()
        if cond is None or t is None:
            raise Untranslated(ins.form)
        if t in f.insns and not (t != f.va and t in self.funcs):
            labels.add(t)
            return ["if (%s()) goto %s;" % (cond, lname(t))]
        return ["if (%s()) { %s }" % (cond, self._tail(meta, t, ins.va))]

    # -- everything -----------------------------------------------------------------------------------
    def run(self):
        for va in sorted(self.funcs):
            self.texts[va] = self.emit_function(self.funcs[va])
            hk = self.hooks.funcs.get(va)
            if hk:
                self.texts[va] += "\nvoid %s(void) { hook_%s(); }\n" % (fname(va), hk["name"])
        return self

    def failure_report(self) -> str:
        lines = ["forms without translation: %d" % len(self.errors)]
        for form, (cnt, first, msg) in sorted(self.errors.items()):
            lines.append("  %-32s x%-6d first 0x%08x  %s" % (form, cnt, first, msg))
        for site, msg in self.soft_errors[:50]:
            lines.append("  0x%08x: %s" % (site, msg))
        return "\n".join(lines)

    # -- output -------------------------------------------------------------------------------------
    def _closure_ok(self, bad_fn):
        """va -> True if neither the function nor the transitive closure of its calls has `bad_fn`."""
        bad = {va: bad_fn(m) for va, m in self.meta.items()}
        changed = True
        while changed:
            changed = False
            for va, m in self.meta.items():
                if not bad[va] and any(bad.get(c, True) for c in m.calls):
                    bad[va] = True
                    changed = True
        return {va: not b for va, b in bad.items()}

    def write(self, outdir, exe_sha256: str):
        out = Path(outdir)
        out.mkdir(parents=True, exist_ok=True)
        for old in out.glob("*.c"):
            old.unlink()
        vas = sorted(self.texts)
        total = sum(len(self.texts[v]) for v in vas)
        target = total // N_FILES + 1
        files = []     # (name, [vas])
        cur, size = [], 0
        for v in vas:
            cur.append(v)
            size += len(self.texts[v])
            if size >= target and len(files) < N_FILES - 1:
                files.append(cur)
                cur, size = [], 0
        if cur:
            files.append(cur)
        n_lines = 0
        names = []
        for i, group in enumerate(files):
            nm = "f%02d.c" % i
            names.append(nm)
            text = '#include "machine.h"\n#include "funcs.h"\n\n' + "\n".join(self.texts[v] for v in group)
            (out / nm).write_text(text)
            n_lines += text.count("\n")
        # funcs.h
        h = ["/* generated by recomp; do not edit */", "#ifndef GPM2_FUNCS_H", "#define GPM2_FUNCS_H", ""]
        h += ["void %s(void);" % fname(v) for v in vas]
        h += ["void %s_orig(void);" % fname(v) for v in sorted(self.hooks.funcs)]
        h += ["void hook_%s(void);" % e["name"] for e in self.hooks.funcs.values()]
        h += ["int hookpt_%s(void);" % e["name"] for e in self.hooks.points.values()]
        h += [""] + ["void %s(void);" % ident for ident, _ in self.imp_ident]
        h += ["", "#endif", ""]
        (out / "funcs.h").write_text("\n".join(h))
        # dispatch_table.c
        d = ['#include "machine.h"', '#include "funcs.h"', "", "const DispatchEntry dispatch_table[] = {"]
        d += ["    {0x%08xu, %s}," % (v, fname(v)) for v in vas]
        d += ["};", "const unsigned dispatch_count = %d;" % len(vas), ""]
        (out / "dispatch_table.c").write_text("\n".join(d))
        # imports.c
        c = ['#include "machine.h"', '#include "funcs.h"', ""]
        self.unknown_arity = []
        for i, (imp, (ident, label)) in enumerate(zip(self.imports, self.imp_ident)):
            pop, known = pop_bytes(imp["dll"], imp["name"])
            if not known:
                self.unknown_arity.append(label)
            c.append('void %s(void) { guest_import(%d, "%s", %d); }' % (ident, i, label, pop))
        c += ["", "const ImportEntry import_table[] = {"]
        for imp, (ident, label) in zip(self.imports, self.imp_ident):
            c.append('    {0x%08xu, %s, "%s"},' % (imp["iat"], ident, label))
        c += ["};", "const unsigned import_count = %d;" % len(self.imports), ""]
        (out / "imports.c").write_text("\n".join(c))
        # fuzzinfo.json (input for tests/fuzz.py)
        # strict: no import, no indirect call/jump in the closure;
        # extended: accepts imports (emulated by an identical opaque model on both sides) and `jmp reg`
        strict = self._closure_ok(lambda m: m.imports or m.indirect or m.regjmp)
        elig = self._closure_ok(lambda m: m.indirect)
        forms = sorted({f for m in self.meta.values() for f in m.forms})
        fidx = {f: i for i, f in enumerate(forms)}
        info = {"forms": forms, "functions": {}}
        for va in vas:
            m = self.meta[va]
            e = bool(elig[va]) and not self.funcs[va].thunk
            ent = {"eligible": e, "strict": bool(strict[va]) and not self.funcs[va].thunk, "calls": sorted(m.calls), "forms": sorted(fidx[f] for f in m.forms),
                   "n": len(m.insns)}
            if e:
                ent["insns"] = [[a, fidx[fm]] for a, fm in m.insns]
            info["functions"]["%d" % va] = ent
        (out / "fuzzinfo.json").write_text(json.dumps(info, separators=(",", ":")))
        manifest = {
            "translator_version": TRANSLATOR_VERSION,
            "exe_sha256": exe_sha256,
            "functions": len(vas),
            "import_thunks": sum(1 for v in vas if self.funcs[v].thunk),
            "imports": len(self.imports),
            "instructions": self.n_insns,
            "forms": len(forms),
            "switches": sum(len(f.switches) for f in self.funcs.values()),
            "eligible_functions": sum(1 for v in vas if elig[v] and not self.funcs[v].thunk),
            "strict_eligible_functions": sum(1 for v in vas if strict[v] and not self.funcs[v].thunk),
            "imports_arity_guessed": len(self.unknown_arity),
            "files": names,
            "c_lines": n_lines,
            "c_bytes": sum((out / nm).stat().st_size for nm in names),
            "soft_errors": len(self.soft_errors),
        }
        (out / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
        return manifest
