#!/usr/bin/env python3
"""Confirmatory: ratio of native target_cycle / oracle target_cycle on
paired identical FM writes.

If T2 is correct, native's reported master-cycle for each FM write is a
~constant fraction of oracle's across the run — and that fraction is
visible even when normalized per-wall because BOTH walls have a single
VBla/music tick at ~85%-position. So matching writes should show a
tight ratio distribution centered well below 1.0.

Pairs: for each wall frame W present in both logs, find the first
matching (part, reg, val) write in each and compare target_cycles.
"""
import sys
from collections import defaultdict


def parse(path):
    rows = []
    with open(path) as f:
        for ln in f:
            if ln.startswith('#') or not ln.strip(): continue
            p = ln.split()
            wall = int(p[0]); addr = int(p[3], 16); val = int(p[4], 16) & 0xFF
            tc = int(p[-1])
            rows.append((wall, addr, val, tc))
    latch = {1: None, 2: None}
    by_wall = defaultdict(list)
    for wall, addr, val, tc in rows:
        if addr == 0xA04000: latch[1] = val
        elif addr == 0xA04002 and latch[1] is not None:
            by_wall[wall].append((1, latch[1], val, tc))
        elif addr == 0xA04004: latch[2] = val
        elif addr == 0xA04006 and latch[2] is not None:
            by_wall[wall].append((2, latch[2], val, tc))
    return by_wall


def main():
    nat = parse(sys.argv[1]); ora = parse(sys.argv[2])
    # Match walls by INTERNAL game frame, not wall frame — demo position.
    # For simplicity, match walls by (wall, write_ordinal) within each and
    # pick pairs where the write content matches.
    # Flatten by ordinal position (stream order). Pair ith write on each.
    ratios = []
    nat_flat = []; ora_flat = []
    for w in sorted(nat): nat_flat.extend(nat[w])
    for w in sorted(ora): ora_flat.extend(ora[w])
    n = min(len(nat_flat), len(ora_flat))
    for i in range(n):
        a = nat_flat[i]; b = ora_flat[i]
        if a[:3] != b[:3]: break
        if a[3] > 0 and b[3] > 0:
            ratios.append(a[3] / b[3])
    if not ratios: print("no matching pairs"); return
    ratios.sort(); n = len(ratios)
    print(f"matched pairs on same wall: {n}")
    print(f"ratio native_tc / oracle_tc:")
    print(f"  min    : {ratios[0]:.4f}")
    print(f"  p5     : {ratios[n//20]:.4f}")
    print(f"  p25    : {ratios[n//4]:.4f}")
    print(f"  median : {ratios[n//2]:.4f}")
    print(f"  p75    : {ratios[3*n//4]:.4f}")
    print(f"  p95    : {ratios[n*19//20]:.4f}")
    print(f"  max    : {ratios[-1]:.4f}")
    print(f"  mean   : {sum(ratios)/n:.4f}")
    tight = sum(1 for r in ratios if 0.28 <= r <= 0.40)
    print(f"  within [0.28, 0.40]: {tight} ({100*tight/n:.1f}%)")


if __name__ == '__main__':
    main()
