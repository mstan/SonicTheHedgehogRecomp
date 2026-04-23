#!/usr/bin/env python3
"""T1: Diff cycle-within-wall (target_cycle) for matching FM writes.

For each (wall, part, reg_latch, val) that matches content-wise between
native and oracle at the same relative ordinal position, compare their
target_cycle column to measure sub-wall-frame phase drift.
"""
import sys


def parse_log(path):
    rows = []
    with open(path) as f:
        for ln in f:
            if ln.startswith('#') or not ln.strip(): continue
            parts = ln.split()
            wall = int(parts[0])
            addr = int(parts[3], 16)
            val  = int(parts[4], 16) & 0xFF
            # target_cycle = last column
            tc = int(parts[-1])
            rows.append((wall, addr, val, tc))
    return rows


def extract_fm(rows):
    latch = {1: None, 2: None}
    out = []
    for wall, addr, val, tc in rows:
        if addr == 0xA04000: latch[1] = val
        elif addr == 0xA04002 and latch[1] is not None:
            out.append((wall, 1, latch[1], val, tc))
        elif addr == 0xA04004: latch[2] = val
        elif addr == 0xA04006 and latch[2] is not None:
            out.append((wall, 2, latch[2], val, tc))
    # Normalize tc to phase fraction [0,1) within each wall frame.
    by_wall_max = {}
    for wall, _, _, _, tc in out:
        by_wall_max[wall] = max(by_wall_max.get(wall, 0), tc)
    normed = []
    for wall, p, r, v, tc in out:
        m = by_wall_max[wall] or 1
        normed.append((wall, p, r, v, tc / m))
    return normed


def main():
    nat = extract_fm(parse_log(sys.argv[1]))
    ora = extract_fm(parse_log(sys.argv[2]))
    n = min(len(nat), len(ora))
    print(f"native writes: {len(nat)}, oracle: {len(ora)}, comparing {n}")

    matched = 0; diffs = []
    first_content_div = None
    for i in range(n):
        a = nat[i]; b = ora[i]
        if a[1] == b[1] and a[2] == b[2] and a[3] == b[3]:
            matched += 1
            diffs.append(a[4] - b[4])
        else:
            if first_content_div is None: first_content_div = i
    print(f"content-matching writes: {matched}")
    if first_content_div is not None:
        print(f"first content mismatch at ordinal {first_content_div}")
    if diffs:
        diffs.sort()
        n = len(diffs)
        absd = [abs(d) for d in diffs]; absd.sort()
        print(f"phase-delta (native_frac - oracle_frac) per matching write:")
        print(f"  min    : {diffs[0]:+.4f}")
        print(f"  p5     : {diffs[n//20]:+.4f}")
        print(f"  median : {diffs[n//2]:+.4f}")
        print(f"  p95    : {diffs[n*19//20]:+.4f}")
        print(f"  max    : {diffs[-1]:+.4f}")
        print(f"  mean   : {sum(diffs)/n:+.4f}")
        print(f"  |delta| median : {absd[n//2]:.4f}")
        print(f"  |delta| p95    : {absd[n*19//20]:.4f}")
        tight = sum(1 for d in absd if d < 0.01)
        loose = sum(1 for d in absd if d < 0.05)
        print(f"  within 1% of wall : {tight} ({100*tight/n:.1f}%)")
        print(f"  within 5% of wall : {loose} ({100*loose/n:.1f}%)")


if __name__ == '__main__':
    main()
