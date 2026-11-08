"""Table of bug-fix hooks: recomp/hooks.json.

  {"exe_sha256": "...", "hooks": [
     {"kind": "func",  "va": "0x...", "sha256": "<function bytes>", "name": "x", "bug": "BUG-nnn"},
     {"kind": "point", "va": "0x...", "bytes": "<instruction hex>", "name": "x", "bug": "BUG-nnn",
      "skip": "0x..."}]}

`func`: f_<va> becomes a caller of hook_<name>(), which chooses between f_<va>_orig (the generated function) and the
fixed version. `point`: before the instruction at <va> the generated C calls hookpt_<name>(); if it returns
nonzero and there is a `skip`, it jumps to that VA (otherwise flow continues). The table only applies to the
EXE with the given sha256 (test PEs ignore it). Hash or bytes that do not match -> the build fails.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

DEFAULT_PATH = Path(__file__).with_name("hooks.json")


class HookError(Exception):
    pass


class Hooks:
    def __init__(self, funcs=None, points=None):
        self.funcs = funcs or {}     # va -> entry
        self.points = points or {}   # va -> entry

    @classmethod
    def load(cls, pe, path=None):
        path = Path(path) if path else DEFAULT_PATH
        if not path.exists():
            return cls()
        doc = json.loads(path.read_text())
        data = getattr(pe, "data", None)
        if data is None or doc.get("exe_sha256") != hashlib.sha256(data).hexdigest():
            return cls()
        h = cls()
        for e in doc["hooks"]:
            e = dict(e)
            e["va"] = int(e["va"], 0)
            if e.get("skip"):
                e["skip"] = int(e["skip"], 0)
            kind = e["kind"]
            if kind not in ("func", "point"):
                raise HookError("hook %s: invalid kind %r" % (e["name"], kind))
            d = h.funcs if kind == "func" else h.points
            if e["va"] in d:
                raise HookError("duplicate hook at 0x%08x" % e["va"])
            d[e["va"]] = e
        return h

    def verify(self, pe, funcs):
        """Confere hashes/bytes contra o EXE e a existencia da funcao/instrucao descoberta."""
        for va, e in self.funcs.items():
            f = funcs.get(va)
            if f is None or f.thunk:
                raise HookError("hook %s: 0x%08x is not a translated function" % (e["name"], va))
            end = max(i.end for i in f.insns.values())
            got = hashlib.sha256(pe.read(va, end - va)).hexdigest()
            if got != e["sha256"]:
                raise HookError("hook %s: sha256 of function 0x%08x differs (%s)" % (e["name"], va, got))
        for va, e in self.points.items():
            want = bytes.fromhex(e["bytes"])
            if pe.read(va, len(want)) != want:
                raise HookError("hook %s: bytes at 0x%08x differ" % (e["name"], va))
            owner = [f for f in funcs.values() if not f.thunk and va in f.insns]
            if len(owner) != 1 or owner[0].insns[va].end - va != len(want):
                raise HookError("hook %s: 0x%08x is not an instruction start of a function" % (e["name"], va))
            if e.get("skip") and e["skip"] not in owner[0].insns:
                raise HookError("hook %s: skip 0x%08x outside the function" % (e["name"], e["skip"]))
