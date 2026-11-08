#!/usr/bin/env python3
"""Generates the app icon from the game's own icon (GPM.ICO), using only the stdlib.

Current macOS style: 1024x1024 canvas with a centered 824x824 "squircle", #008080 background (Windows 98 teal), subtle
inner border and shadow; the game icon goes in the center, enlarged by nearest neighbor with an integer factor (32x32 x16 = 512).
The icon comes from the user's file at build time and is NOT committed.

usage: python3 -I tools/make_icon.py <GPM.ICO> <output.iconset> [master.png]
"""
import struct
import sys
import zlib
from pathlib import Path


def read_ico(path):
    """Picks the image with the largest area and depth; returns (w, h, RGBA pixels as a list of rows)."""
    d = Path(path).read_bytes()
    n = struct.unpack_from("<H", d, 4)[0]
    best = None
    for i in range(n):
        w, h, cc, _, _, bpp, size, off = struct.unpack_from("<BBBBHHII", d, 6 + 16 * i)
        w = w or 256
        h = h or 256
        hdr = struct.unpack_from("<IiiHHIIiiII", d, off)
        bits = hdr[4] * hdr[3] if False else hdr[4]
        if hdr[0] != 40:
            continue                                   # PNG inside the ICO: not used by the game
        key = (w * h, bits == 4, bits)      # the 8 bpp version (e.g. in APPICON1.ICO) has a broken palette (corrupted car): prefer the 16-color one
        if best is None or key > best[0]:
            best = (key, off, w, h, bits, hdr)
    if best is None:
        raise SystemExit("ICO has no BMP image")
    _, off, w, h, bits, hdr = best
    ncol = hdr[9] or (1 << bits if bits <= 8 else 0)
    p = off + hdr[0]
    pal = [tuple(d[p + 4 * k + 2: p + 4 * k + 5 - 0][::1]) for k in range(0)]
    pal = []
    for k in range(ncol if bits <= 8 else 0):
        b, g, r = d[p + 4 * k], d[p + 4 * k + 1], d[p + 4 * k + 2]
        pal.append((r, g, b))
    p += 4 * len(pal)
    stride = ((w * bits + 31) // 32) * 4
    xor = d[p:p + stride * h]
    p += stride * h
    mstride = ((w + 31) // 32) * 4
    andm = d[p:p + mstride * h]
    rows = []
    for y in range(h):
        sy = h - 1 - y                                  # BMP is bottom-up
        row = []
        for x in range(w):
            if bits == 8:
                r, g, b = pal[xor[sy * stride + x]]
            elif bits == 4:
                v = xor[sy * stride + x // 2]
                r, g, b = pal[(v >> 4) if x % 2 == 0 else (v & 15)]
            elif bits == 1:
                r, g, b = pal[(xor[sy * stride + x // 8] >> (7 - x % 8)) & 1]
            elif bits in (24, 32):
                o = sy * stride + x * bits // 8
                b, g, r = xor[o], xor[o + 1], xor[o + 2]
            else:
                raise SystemExit("unsupported depth %d" % bits)
            a = 0 if (andm[sy * mstride + x // 8] >> (7 - x % 8)) & 1 else 255
            row.append((r, g, b, a))
        rows.append(row)
    return w, h, rows


def png(path, w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, data):
        c = struct.pack(">I", len(data)) + t + data
        return c + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)

    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def master(gw, gh, grows):
    S, A, N = 1024, 412.0, 5.0                          # canvas, squircle half-width (824/2), exponent of the continuous curve
    buf = bytearray(S * S * 4)
    cx = cy = S / 2.0

    def sd(x, y, oy=0.0):
        return (abs((x - cx) / A) ** N + abs((y - cy - oy) / A) ** N) ** (1.0 / N)

    k = max(1, min(512 // gw, 512 // gh))               # integer factor: 32x32 -> 16x = 512
    ox, oy2 = (S - gw * k) // 2, (S - gh * k) // 2
    for y in range(S):
        base = y * S * 4
        for x in range(S):
            px, py = x + 0.5, y + 0.5
            r = g = b = 0.0
            a = 0.0
            # template default shadow: down 10 px, blurred
            ds = sd(px, py, 10.0)
            sh = 0.32 * min(1.0, max(0.0, (1.0 - ds) * A / 22.0 + 0.5)) if ds < 1.12 else 0.0
            if sh > 0:
                a = sh
            d = sd(px, py)
            cov = min(1.0, max(0.0, (1.0 - d) * A + 0.5)) if d < 1.01 else 0.0
            if cov > 0:
                t = (py - (cy - A)) / (2 * A)
                t = min(1.0, max(0.0, t))
                br, bg, bb = 0.0, 128.0, 128.0                               # Windows 98 teal background (#008080)
                edge = (1.0 - d) * A                                          # inner distance to the border, in px
                if 0 <= edge < 3.0:                                           # subtle inner border
                    dk = 0.14 * (1.0 - edge / 3.0)                            # light gray inner border
                    br, bg, bb = br * (1 - dk), bg * (1 - dk), bb * (1 - dk)
                gx, gy = (x - ox) // k, (y - oy2) // k
                if 0 <= gx < gw and 0 <= gy < gh:
                    ir, ig, ib, ia = grows[gy][gx]
                    if ia:
                        br, bg, bb = ir, ig, ib
                # composite over the shadow
                oa = cov + a * (1 - cov)
                r = (br * cov + 0.0 * a * (1 - cov)) / oa
                g = (bg * cov) / oa
                b = (bb * cov) / oa
                a = oa
            o = base + x * 4
            buf[o], buf[o + 1], buf[o + 2], buf[o + 3] = int(r + 0.5), int(g + 0.5), int(b + 0.5), int(a * 255 + 0.5)
    return buf


def shrink(src, s, f):
    """downscales by f x f block average (with premultiplied alpha)."""
    n = s // f
    out = bytearray(n * n * 4)
    for y in range(n):
        for x in range(n):
            ra = ga = ba = aa = 0
            for dy in range(f):
                row = ((y * f + dy) * s + x * f) * 4
                for dx in range(f):
                    o = row + dx * 4
                    a = src[o + 3]
                    ra += src[o] * a; ga += src[o + 1] * a; ba += src[o + 2] * a; aa += a
            o = (y * n + x) * 4
            if aa:
                out[o], out[o + 1], out[o + 2] = ra // aa, ga // aa, ba // aa
            out[o + 3] = aa // (f * f)
    return out, n


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    gw, gh, rows = read_ico(sys.argv[1])
    big = master(gw, gh, rows)
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    if len(sys.argv) > 3:
        png(sys.argv[3], 1024, 1024, big)
    sizes = {}
    f = 1
    while 1024 // f >= 16:
        sizes[1024 // f] = shrink(big, 1024, f)[0] if f > 1 else big
        f *= 2
    for base in (16, 32, 128, 256, 512):
        png(out / ("icon_%dx%d.png" % (base, base)), base, base, sizes[base])
        png(out / ("icon_%dx%d@2x.png" % (base, base)), base * 2, base * 2, sizes[base * 2])


if __name__ == "__main__":
    main()
