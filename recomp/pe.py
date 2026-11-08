"""PE32 reader (stdlib only): sections, imports, relocations, VA<->offset mapping."""

from __future__ import annotations

import struct
from pathlib import Path


def u16(d, o):
    return struct.unpack_from("<H", d, o)[0]


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


class PE:
    def __init__(self, data: bytes):
        self.data = data
        d = data
        if d[:2] != b"MZ":
            raise ValueError("not an MZ executable")
        pe = u32(d, 0x3C)
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("missing PE signature")
        fh = pe + 4
        self.machine = u16(d, fh)
        nsec = u16(d, fh + 2)
        opt_size = u16(d, fh + 16)
        opt = fh + 20
        if u16(d, opt) != 0x10B:
            raise ValueError("not PE32")
        self.entry_rva = u32(d, opt + 16)
        self.image_base = u32(d, opt + 28)
        self.entry_va = self.image_base + self.entry_rva
        self.data_dirs = [(u32(d, opt + 96 + 8 * i), u32(d, opt + 100 + 8 * i)) for i in range(16)]
        self.sections = []
        so = opt + opt_size
        for i in range(nsec):
            h = so + 40 * i
            name = d[h:h + 8].rstrip(b"\0").decode("latin-1")
            vsize, vrva, rawsize, rawptr = struct.unpack_from("<IIII", d, h + 8)
            self.sections.append({"name": name, "va": self.image_base + vrva, "rva": vrva,
                                  "vsize": vsize, "raw": rawptr, "rawsize": rawsize,
                                  "chars": u32(d, h + 36)})
        self._iat = None
        self._relocs = None

    @classmethod
    def load(cls, path) -> "PE":
        return cls(Path(path).read_bytes())

    # -- address mapping -------------------------------------------
    def section_of_va(self, va: int):
        for s in self.sections:
            if s["va"] <= va < s["va"] + max(s["vsize"], s["rawsize"]):
                return s
        return None

    def section_by_name(self, name: str):
        for s in self.sections:
            if s["name"] == name:
                return s
        raise KeyError(name)

    def va_to_off(self, va: int):
        """File offset of a VA, or None if there is no data in the file."""
        for s in self.sections:
            if s["va"] <= va < s["va"] + s["rawsize"]:
                return s["raw"] + (va - s["va"])
        return None

    def off_to_va(self, off: int):
        for s in self.sections:
            if s["raw"] <= off < s["raw"] + s["rawsize"]:
                return s["va"] + (off - s["raw"])
        return None

    def rva_to_off(self, rva: int):
        return self.va_to_off(self.image_base + rva)

    def read(self, va: int, n: int) -> bytes:
        """Up to n bytes from va, without crossing the end of the section (b'' if no data)."""
        for s in self.sections:
            if s["va"] <= va < s["va"] + s["rawsize"]:
                avail = s["va"] + s["rawsize"] - va
                off = s["raw"] + (va - s["va"])
                return self.data[off:off + min(n, avail)]
        return b""

    def read_u32(self, va: int):
        b = self.read(va, 4)
        return u32(b, 0) if len(b) == 4 else None

    # -- imports -------------------------------------------------------------
    def imports(self) -> list:
        d = self.data
        rva, _ = self.data_dirs[1]
        out = []
        if not rva:
            return out
        o = self.rva_to_off(rva)
        while True:
            oft, _, _, name_rva, ft = struct.unpack_from("<IIIII", d, o)
            if name_rva == 0 and ft == 0:
                break
            no = self.rva_to_off(name_rva)
            dll = d[no:d.find(b"\0", no)].decode("latin-1")
            lookup = self.rva_to_off(oft or ft)
            i = 0
            while True:
                th = u32(d, lookup + 4 * i)
                if th == 0:
                    break
                if th & 0x80000000:
                    name = "#%d" % (th & 0xFFFF)
                else:
                    ho = self.rva_to_off(th)
                    name = d[ho + 2:d.find(b"\0", ho + 2)].decode("latin-1")
                out.append({"dll": dll, "name": name, "iat": self.image_base + ft + 4 * i})
                i += 1
            o += 20
        return out

    def iat_map(self) -> dict:
        """IAT entry VA -> "DLL!name"."""
        if self._iat is None:
            self._iat = {i["iat"]: "%s!%s" % (i["dll"], i["name"]) for i in self.imports()}
        return self._iat

    # -- relocations ----------------------------------------------------------
    def relocs(self) -> list:
        """List of (location_va, value) for HIGHLOW relocations that have data in the file."""
        if self._relocs is not None:
            return self._relocs
        d = self.data
        rva, size = self.data_dirs[5]
        out = []
        if rva:
            o = self.rva_to_off(rva)
            end = o + size
            while o + 8 <= end:
                page, bsize = struct.unpack_from("<II", d, o)
                if bsize < 8:
                    break
                for k in range((bsize - 8) // 2):
                    e = u16(d, o + 8 + 2 * k)
                    if e >> 12 != 3:
                        continue
                    loc = self.image_base + page + (e & 0xFFF)
                    val = self.read_u32(loc)
                    if val is not None:
                        out.append((loc, val))
                o += bsize
        self._relocs = out
        return out
