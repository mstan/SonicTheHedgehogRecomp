#!/usr/bin/env python3
"""Remove extra_func entries with addresses beyond ROM size."""
import re, sys

cfg = sys.argv[1] if len(sys.argv) > 1 else "game.cfg"
max_addr = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x080000

lines = open(cfg).readlines()
out = []
removed = 0
for line in lines:
    m = re.match(r'\s*extra_func\s+(?:0x)?([0-9A-Fa-f]+)', line)
    if m:
        addr = int(m.group(1), 16)
        if addr > max_addr:
            removed += 1
            continue
    out.append(line)
open(cfg, "w").writelines(out)
print(f"Removed {removed} entries with addr > 0x{max_addr:X}")
print(f"Remaining extra_func: {sum(1 for l in out if 'extra_func' in l)}")
