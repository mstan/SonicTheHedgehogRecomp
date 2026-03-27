#!/usr/bin/env python3
"""Diff interpreter coverage.log against native's dispatch table.
Shows functions the interpreter called that native can't handle."""
import re, os, sys

SONIC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Read dispatch table (what native has compiled)
dispatch = set()
disp_path = os.path.join(SONIC_DIR, "generated", "sonic_dispatch.c")
with open(disp_path) as f:
    for m in re.finditer(r'0x([0-9A-Fa-f]+)u, func_', f.read()):
        dispatch.add(int(m.group(1), 16))

# Read coverage (what interpreter actually called)
cov_path = os.path.join(SONIC_DIR, "run", "coverage.log")
if not os.path.exists(cov_path):
    print("No coverage.log found in run/")
    sys.exit(1)

covered = set()
with open(cov_path) as f:
    for line in f:
        m = re.match(r'\s*extra_func\s+(?:0x)?([0-9A-Fa-f]+)', line)
        if m:
            addr = int(m.group(1), 16)
            if addr < 0x80000:
                covered.add(addr)

# Read blacklist
blacklist = set()
bl_path = os.path.join(SONIC_DIR, "blacklist.txt")
if os.path.exists(bl_path):
    with open(bl_path) as f:
        for line in f:
            line = line.split("#")[0].strip()
            if line:
                blacklist.add(int(line, 16))

# What the interpreter called that native doesn't have
missing = sorted(covered - dispatch - blacklist)

print(f"Interpreter called: {len(covered)} unique functions")
print(f"Native has compiled: {len(dispatch)} functions")
print(f"Blacklisted: {len(blacklist)} addresses")
print(f"Native is MISSING: {len(missing)} functions")

if missing:
    cfg_path = os.path.join(SONIC_DIR, "game.cfg")
    in_cfg = set()
    with open(cfg_path) as f:
        for line in f:
            m = re.match(r'\s*extra_func\s+(?:0x)?([0-9A-Fa-f]+)', line)
            if m:
                in_cfg.add(int(m.group(1), 16))

    new_to_add = [a for a in missing if a not in in_cfg]
    already_in_cfg = [a for a in missing if a in in_cfg]

    if already_in_cfg:
        print(f"\n  Already in game.cfg but not compiled (interior labels?): {len(already_in_cfg)}")

    if new_to_add:
        print(f"\n  NEW — need to add to game.cfg: {len(new_to_add)}")
        for a in new_to_add:
            print(f"    0x{a:06X}")

        # Write them out for easy ingestion
        out_path = os.path.join(SONIC_DIR, "run", "new_from_coverage.log")
        with open(out_path, "w") as f:
            for a in new_to_add:
                f.write(f"extra_func 0x{a:06X}\n")
        print(f"\n  Written to {out_path}")
    else:
        print("\n  Nothing new to add — all missing functions are interior labels")
else:
    print("\nFull coverage! Native has everything the interpreter called.")
