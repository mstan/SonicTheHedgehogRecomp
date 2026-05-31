#!/usr/bin/env python3
"""Align native/oracle monkey traces by sonic_x and print monkey-parent
state transitions (routine, render_flags, timer, coarse-back camera)."""
import json, sys
d = sys.argv[1] if len(sys.argv) > 1 else "_mk_cap"
nat = {r['sx']: r for r in json.load(open(f"{d}/native_trace.json"))}
ora = {r['sx']: r for r in json.load(open(f"{d}/oracle_trace.json"))}
common = sorted(set(nat) & set(ora))
print("native sx range %d..%d (%d frames)" % (min(nat), max(nat), len(nat)))
print("oracle sx range %d..%d (%d frames)" % (min(ora), max(ora), len(ora)))
print("common %d (%d..%d)" % (len(common), min(common), max(common)))
print("  sx   | rout n/o | rend n/o  | t2e n/o | cback n/o   | cam n/o     | my n/o")
prev = None
for sx in common:
    n = nat[sx]; o = ora[sx]
    sig = (n['rout'], o['rout'], n['rend'], o['rend'])
    if prev is None or sig != prev:
        print("  %5d |  %d   %d   | 0x%02X 0x%02X | %3d %3d | %5d %5d | %4d %4d | %4d %4d" % (
            sx, n['rout'], o['rout'], n['rend'], o['rend'], n['t2e'], o['t2e'],
            n['cback'], o['cback'], n['cam'], o['cam'], n['my'], o['my']))
        prev = sig
