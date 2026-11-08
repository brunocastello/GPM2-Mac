#!/usr/bin/env python3
"""Rename the file inside a MacBinary (name in the header + header CRC16): macbin_rename.py in.bin "new name" """
import sys
p, name = sys.argv[1], sys.argv[2].encode("mac_roman")
d = bytearray(open(p, "rb").read())
d[1] = len(name); d[2:65] = name.ljust(63, b"\0")
crc = 0
for b in d[:124]:
    crc ^= b << 8
    for _ in range(8):
        crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
d[124:126] = crc.to_bytes(2, "big")
open(p, "wb").write(d)
