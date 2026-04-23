#!/usr/bin/env python3
"""T2: Per-wall cycle totals and distribution from mem_write_log.

target_cycle is the absolute cycle count WITHIN the wall frame at which
the write happened. The max target_cycle per wall approximates
"cycles consumed on that wall" in each build's cycle-counting unit.
"""
import sys
from collections import defaultdict


def parse(path):
    per_wall = defaultdict(int)
    with open(path) as f:
        for ln in f:
            if ln.startswith('#') or not ln.strip(): continue
            parts = ln.split()
            wall = int(parts[0])
            tc = int(parts[-1])
            if tc > per_wall[wall]: per_wall[wall] = tc
    return per_wall


def stats(name, per_wall):
    vals = [v for v in per_wall.values() if v > 0]
    vals.sort()
    if not vals:
        print(f"{name}: no data"); return
    n = len(vals)
    print(f"{name}:")
    print(f"  walls w/ writes   : {n}")
    print(f"  min cycles/wall   : {vals[0]}")
    print(f"  p5                : {vals[n//20]}")
    print(f"  median            : {vals[n//2]}")
    print(f"  p95               : {vals[n*19//20]}")
    print(f"  max               : {vals[-1]}")
    print(f"  mean              : {sum(vals)/n:.0f}")


nat = parse(sys.argv[1]); ora = parse(sys.argv[2])
stats('native', nat); stats('oracle', ora)

# Compare wall-frame-by-wall-frame the cycles-per-wall
common = sorted(set(nat) & set(ora))
ratios = []
for w in common:
    if nat[w] > 0 and ora[w] > 0: ratios.append(nat[w] / ora[w])
if ratios:
    ratios.sort(); n = len(ratios)
    print(f"\nratio native_cycles_per_wall / oracle_cycles_per_wall across {n} common walls:")
    print(f"  min    : {ratios[0]:.3f}")
    print(f"  median : {ratios[n//2]:.3f}")
    print(f"  max    : {ratios[-1]:.3f}")
    print(f"  mean   : {sum(ratios)/n:.3f}")
