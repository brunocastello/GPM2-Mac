#!/usr/bin/env python3
"""Appends to an .icns written by iconutil the legacy chunks Mac OS X 10.6 can read (is32/s8mk/il32/l8mk/ih32/h8mk/it32/t8mk
and the 256/512 PNG), produced by make_icns.py without --plain (same white squircle style). Types already present are skipped
and the total length in the header is fixed. stdlib only.

usage: python3 -I tools/add_legacy_icns.py <Gpm2.exe|GPM.ICO> <file.icns>
"""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def chunks(d):
    out, o = [], 8
    while o + 8 <= len(d):
        t, n = d[o:o + 4], struct.unpack_from(">I", d, o + 4)[0]
        if n < 8:
            break
        out.append((t, d[o:o + n]))
        o += n
    return out


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    target = Path(sys.argv[2])
    cur = target.read_bytes()
    if cur[:4] != b"icns":
        raise SystemExit("not an icns: " + str(target))
    have = {t for t, _ in chunks(cur)}
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td) / "legacy.icns"
        subprocess.run([sys.executable, "-I", str(Path(__file__).with_name("make_icns.py")), sys.argv[1], str(tmp)], check=True)
        add = [c for t, c in chunks(tmp.read_bytes()) if t not in have]
    body = cur[8:] + b"".join(add)
    target.write_bytes(b"icns" + struct.pack(">I", len(body) + 8) + body)
    print("icns: added %d legacy chunk(s)" % len(add))


if __name__ == "__main__":
    main()
