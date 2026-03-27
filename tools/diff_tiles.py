#!/usr/bin/env python3
"""Show hex diffs of VRAM tiles between native and interpreter."""
import sys

with open("vram_native.bin", "rb") as f:
    native = f.read()
with open("vram_interp.bin", "rb") as f:
    interp = f.read()

TILE = 32
native_zero = 0
interp_zero = 0
partial = 0

for t in range(860, 895):
    off = t * TILE
    tn = native[off:off+TILE]
    ti = interp[off:off+TILE]
    if tn == ti:
        continue
    n_zero = all(b == 0 for b in tn)
    i_zero = all(b == 0 for b in ti)
    tag = ""
    if n_zero:
        native_zero += 1
        tag = " <<< NATIVE ALL ZEROS (art missing!)"
    elif i_zero:
        interp_zero += 1
        tag = " <<< INTERP ALL ZEROS"
    else:
        partial += 1
        tag = " (both have data, content differs)"
    print(f"Tile {t} (0x{off:04X}):{tag}")
    if t <= 865 or n_zero:
        print(f"  native: {tn[:16].hex()} ...")
        print(f"  interp: {ti[:16].hex()} ...")

print(f"\nSummary: native_zero={native_zero}, interp_zero={interp_zero}, partial_diff={partial}")
