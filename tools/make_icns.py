#!/usr/bin/env python3
"""Generates the .icns of the PowerPC app using only the stdlib: there is no iconutil in CI (Linux).

The source is Gpm2.exe itself (RT_GROUP_ICON resource; the 32x32 image bytes are identical to the installed
GPM.ICO) or an .ico. Same style as make_icon.py (white squircle). Sizes for Tiger (16/32/48/128: RGB with RLE + 8-bit
mask) and Leopard (256/512: PNG).

usage: python3 -I tools/make_icns.py [--plain] [--rez out.r] <Gpm2.exe|GPM.ICO> <out.icns>
  --plain: no background; only GPM.ICO (alpha preserved), enlarged by nearest neighbor with an integer factor (Mac OS X PPC and Mac OS 9).
  --rez:   (with --plain) also writes a .r (Rez) for Mac OS 9: icns + ICN#/icl4/icl8/ics#/ics8 + BNDL/FREF/'GPM2' signature.
"""
import importlib.util
import struct
import sys
import tempfile
from pathlib import Path


def load_make_icon():
    spec = importlib.util.spec_from_file_location("make_icon", Path(__file__).with_name("make_icon.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def ico_from_exe(path):
    """Builds an .ico (bytes) from the first RT_GROUP_ICON in the PE .rsrc."""
    d = Path(path).read_bytes()
    u16 = lambda o: struct.unpack_from("<H", d, o)[0]
    u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    pe = u32(0x3C)
    nsec, optsz = u16(pe + 6), u16(pe + 20)
    opt = pe + 24
    rsrc_rva = u32(opt + 96 + 16)
    secs = [(u32(h + 12), u32(h + 8), u32(h + 20)) for h in (opt + optsz + 40 * i for i in range(nsec))]

    def off(rva):
        for va, vs, raw in secs:
            if va <= rva < va + max(vs, 1):
                return raw + rva - va
        raise SystemExit("RVA outside the sections")

    base = off(rsrc_rva)

    def entries(o):
        n = u16(o + 12) + u16(o + 14)
        return [(u32(o + 16 + 8 * i), u32(o + 20 + 8 * i)) for i in range(n)]

    def leaf(o):                                         # descends name -> language down to the IMAGE_RESOURCE_DATA_ENTRY
        while True:
            e = entries(o)[0]
            if e[1] & 0x80000000:
                o = base + (e[1] & 0x7FFFFFFF)
            else:
                de = base + e[1]
                return off(u32(de)), u32(de + 4)

    types = {k: base + (v & 0x7FFFFFFF) for k, v in entries(base)}
    if 14 not in types or 3 not in types:
        raise SystemExit("exe has no icon")
    icons = {k: base + (v & 0x7FFFFFFF) for k, v in entries(types[3])}
    go, gs = leaf(base + (entries(types[14])[0][1] & 0x7FFFFFFF))
    cnt = struct.unpack_from("<H", d, go + 4)[0]
    hdr, body, pos = struct.pack("<HHH", 0, 1, cnt), b"", 6 + 16 * cnt
    for i in range(cnt):
        e = d[go + 6 + 14 * i: go + 6 + 14 * i + 14]
        w, h, cc, res, pl, bpp, size, iid = struct.unpack("<BBBBHHIH", e)
        io, isz = leaf(icons[iid])
        hdr += struct.pack("<BBBBHHII", w, h, cc, res, pl, bpp, isz, pos)
        body += d[io:io + isz]
        pos += isz
    return hdr + body


def resample(src, s, n):
    """area average (premultiplied alpha) from s x s to n x n, with fractional weights."""
    out = bytearray(n * n * 4)
    f = s / n
    for y in range(n):
        y0, y1 = y * f, (y + 1) * f
        for x in range(n):
            x0, x1 = x * f, (x + 1) * f
            r = g = b = a = wsum = 0.0
            for sy in range(int(y0), min(s, int(y1) + 1)):
                wy = min(y1, sy + 1) - max(y0, sy)
                for sx in range(int(x0), min(s, int(x1) + 1)):
                    w = wy * (min(x1, sx + 1) - max(x0, sx))
                    o = (sy * s + sx) * 4
                    al = src[o + 3] / 255.0
                    r += src[o] * al * w; g += src[o + 1] * al * w; b += src[o + 2] * al * w; a += al * w; wsum += w
            o = (y * n + x) * 4
            if a > 0:
                out[o], out[o + 1], out[o + 2] = int(r / a + 0.5), int(g / a + 0.5), int(b / a + 0.5)
            out[o + 3] = int(255 * a / wsum + 0.5)
    return out


def plain_sizes(ico_path):
    """no background: the original icon (RGBA) at each size; integer factors only (nearest neighbor), 48 = 1x centered,
    16 = exact 2x2 average of the 32x32 image (integer reduction)."""
    gw, gh, rows = load_make_icon().read_ico(ico_path)
    src = bytearray()
    for r in rows:
        for p in r:
            src += bytes(p)
    out = {}
    for n in (32, 64, 128, 256, 512, 1024):
        k = n // gw
        px = bytearray(n * n * 4)
        for y in range(n):
            for x in range(n):
                o = ((y // k) * gw + x // k) * 4
                px[(y * n + x) * 4:(y * n + x) * 4 + 4] = src[o:o + 4]
        out[n] = px
    c = bytearray(48 * 48 * 4)
    for y in range(gh):
        for x in range(gw):
            o = ((y + 8) * 48 + x + 8) * 4
            c[o:o + 4] = src[(y * gw + x) * 4:(y * gw + x) * 4 + 4]
    out[48] = c
    out[16] = resample(src, 32, 16)
    return out


def clut8():
    t = []
    for i in range(215):
        t.append(((5 - i // 36) * 51, (5 - (i // 6) % 6) * 51, (5 - i % 6) * 51))
    ramp = (0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11)
    for c in range(4):
        for v in ramp:
            t.append((v if c == 0 else 0, v if c == 1 else 0, v if c == 2 else 0) if c < 3 else (v, v, v))
    t.append((0, 0, 0))
    return t


def classic(px, n):
    """ICN#/icl4/icl8 (n=32) and ics#/ics4/ics8 (n=16) from RGBA: returns dict type -> bytes."""
    t8 = clut8()
    # 4 bpp: Mac 16-color palette (indices into clut8)
    t4 = [(255, 255, 255), (255, 255, 0), (255, 102, 0), (221, 0, 0), (255, 0, 153), (51, 0, 153), (0, 0, 204), (0, 153, 255),
          (0, 170, 0), (0, 102, 0), (102, 51, 0), (153, 102, 51), (187, 187, 187), (136, 136, 136), (68, 68, 68), (0, 0, 0)]

    def near(pal, c):
        return min(range(len(pal)), key=lambda i: sum((pal[i][k] - c[k]) ** 2 for k in range(3)))
    b8, b4, bw, mk = bytearray(), bytearray(), bytearray(), bytearray()
    for y in range(n):
        bits_i = bits_m = 0
        row4 = []
        for x in range(n):
            o = (y * n + x) * 4
            c, a = tuple(px[o:o + 3]), px[o + 3]
            op = a >= 128
            b8.append(near(t8, c) if op else 0)
            row4.append(near(t4, c) if op else 0)
            lum = (c[0] * 3 + c[1] * 6 + c[2]) // 10
            bits_i = bits_i * 2 + (1 if op and lum < 160 else 0)
            bits_m = bits_m * 2 + (1 if op else 0)
        for i in range(0, n, 2):
            b4.append(row4[i] * 16 + row4[i + 1])
        bw += bits_i.to_bytes(n // 8, "big")
        mk += bits_m.to_bytes(n // 8, "big")
    sfx = "N#" if n == 32 else "s#"
    if n == 32:
        return {"ICN#": bytes(bw + mk), "icl4": bytes(b4), "icl8": bytes(b8)}
    return {"ics#": bytes(bw + mk), "ics4": bytes(b4), "ics8": bytes(b8)}


def hexdata(b):
    return "\n".join('    $"' + " ".join("%02X" % v for v in b[i:i + 16]) + '"' for i in range(0, len(b), 16))


def write_rez(path, icns, px):
    res = {}
    res.update(classic(px[32], 32))
    res.update(classic(px[16], 16))
    r = "/* generated by tools/make_icns.py --plain --rez: GPM.ICO icon without background (do not commit) */\n#include \"Types.r\"\n\n"
    r += 'data \'GPM2\' (0, "GPM2 0.1") {\n    $"0D47 504D 3220 4D61 6320 4F53 2039"\n};\n\n'
    r += "data 'BNDL' (128) {\n    $\"4750 4D32 0000 0001 4943 4E23 0000 0000 0080 4652 4546 0000 0000 0080\"\n};\n\n"
    r += "data 'FREF' (128) {\n    $\"4150 504C 0000 00\"\n};\n\n"
    for t, d in res.items():
        r += "data '%s' (128) {\n%s\n};\n\n" % (t, hexdata(d))
    r += "data 'icns' (128) {\n%s\n};\n" % hexdata(icns)
    Path(path).write_text(r)


def rle(ch):
    """icns RLE: 0..127 = n+1 literals; 128..255 = (n-125) repeats of the next byte."""
    out, i, n = bytearray(), 0, len(ch)
    while i < n:
        j = i
        while j + 1 < n and ch[j + 1] == ch[i] and j + 1 - i < 129:
            j += 1
        run = j - i + 1
        if run >= 3:
            out += bytes([run + 125, ch[i]])
            i += run
            continue
        k = i
        while k < n and k - i < 128 and not (k + 2 < n and ch[k] == ch[k + 1] == ch[k + 2]):
            k += 1
        if k == i:
            k = i + 1
        out += bytes([k - i - 1]) + bytes(ch[i:k])
        i = k
    return bytes(out)


def chunk(t, data):
    return t + struct.pack(">I", len(data) + 8) + data


def main():
    args = sys.argv[1:]
    plain = "--plain" in args
    rez = None
    if plain:
        args.remove("--plain")
    if "--rez" in args:
        i = args.index("--rez")
        rez = args[i + 1]
        del args[i:i + 2]
    sys.argv = [sys.argv[0]] + args
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    mi = load_make_icon()
    src = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as td:
        ico = Path(td) / "g.ico"
        ico.write_bytes(ico_from_exe(src) if src.suffix.lower() == ".exe" else src.read_bytes())
        png = lambda n, px: (mi.png(Path(td, "p.png"), n, n, px), Path(td, "p.png").read_bytes())[1]
        if plain:
            sizes = plain_sizes(ico)
            s256 = sizes[256]
        else:
            gw, gh, rows = mi.read_ico(ico)
            big = mi.master(gw, gh, rows)
            s256 = mi.shrink(big, 1024, 4)[0]
            sizes = {512: mi.shrink(big, 1024, 2)[0], 256: s256}
            for n in (128, 48, 32, 16):
                sizes[n] = resample(s256, 256, n)
        out = bytearray()
        for n, t3, tm in ((16, b"is32", b"s8mk"), (32, b"il32", b"l8mk"), (48, b"ih32", b"h8mk"), (128, b"it32", b"t8mk")):
            px = sizes[n]
            planes = [bytes(px[c::4]) for c in range(3)]
            body = b"".join(rle(p) for p in planes)
            out += chunk(t3, (b"\0\0\0\0" if n == 128 else b"") + body)
            out += chunk(tm, bytes(px[3::4]))
        out += chunk(b"ic08", png(256, sizes[256]))
        out += chunk(b"ic09", png(512, sizes[512]))
        icns = b"icns" + struct.pack(">I", len(out) + 8) + bytes(out)
        Path(sys.argv[2]).write_bytes(icns)
        if rez:
            write_rez(rez, icns, sizes)


if __name__ == "__main__":
    main()
