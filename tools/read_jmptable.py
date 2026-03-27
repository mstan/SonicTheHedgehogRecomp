#!/usr/bin/env python3
import struct
with open("sonic.bin", "rb") as f:
    # AniArt_Index at $01C03E, 7 entries (6 zones + ending)
    f.seek(0x1C03E)
    data = f.read(14)
    print("AnimateLevelGfx JMP table ($01C03E):")
    base = 0x1C046  # base for offset calculation (table + 8)
    for i in range(0, 14, 2):
        off = struct.unpack(">h", data[i:i+2])[0]  # signed 16-bit BE
        target = 0x1C03E + off  # PC-relative from table start
        print(f"  Zone {i//2}: offset={off:+d} target=${target:06X}")
