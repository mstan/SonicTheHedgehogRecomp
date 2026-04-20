#!/usr/bin/env python3
"""Compare native vs oracle mem_write_log_*.log files.

Buckets writes by (game_mode, internal_frame) — the canonical Sonic 1
sync key — so differences reflect in-game state divergence rather than
wall-clock phase offset.

For each watched address, for each matched (gm, if) bucket present on
BOTH sides, report:
  - write count per side
  - the sequence of values written on each side
  - first bucket where the sequences disagree (by value OR by count)

Usage:
  python compare_mem_write_log.py [--native F] [--oracle F] [--addr 0xFFF001]
                                  [--max-diffs N] [--show-chain]
"""
import argparse, os, sys
from collections import defaultdict


def parse(path):
    """Return list of dicts: {wall, if, gm, addr, val, a7, ret0..ret3, tc}."""
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            toks = line.split()
            if len(toks) < 10:
                continue
            rows.append({
                'wall':  int(toks[0]),
                'if':    int(toks[1]),
                'gm':    int(toks[2]),
                'addr':  int(toks[3], 16),
                'val':   int(toks[4], 16),
                'a7':    int(toks[5], 16),
                'ret0':  int(toks[6], 16),
                'ret1':  int(toks[7], 16),
                'ret2':  int(toks[8], 16),
                'ret3':  int(toks[9], 16),
                'tc':    int(toks[10]) if len(toks) > 10 else 0,
            })
    return rows


def bucket(rows, addr_filter):
    """Map (gm, if) -> list of rows, filtered to addr."""
    b = defaultdict(list)
    for r in rows:
        if addr_filter is not None and r['addr'] != addr_filter:
            continue
        b[(r['gm'], r['if'])].append(r)
    return b


def fmt_seq(rows, show_chain):
    if show_chain:
        return ', '.join(f"{r['val']:02X}@ret0={r['ret0']:06X}" for r in rows)
    return ' '.join(f"{r['val']:02X}" for r in rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--native', default='mem_write_log_native.log')
    ap.add_argument('--oracle', default='mem_write_log_oracle.log')
    ap.add_argument('--addr', default=None,
                    help='hex address to compare (default: all watched)')
    ap.add_argument('--max-diffs', type=int, default=10)
    ap.add_argument('--show-chain', action='store_true',
                    help='show ret0 with each value')
    args = ap.parse_args()

    if not os.path.exists(args.native):
        print(f'missing: {args.native}'); sys.exit(1)
    if not os.path.exists(args.oracle):
        print(f'missing: {args.oracle}'); sys.exit(1)

    n = parse(args.native)
    o = parse(args.oracle)
    print(f'native: {len(n)} writes, oracle: {len(o)} writes')

    addrs = sorted({r['addr'] for r in n} | {r['addr'] for r in o})
    if args.addr:
        addrs = [int(args.addr, 0)]
    print(f'addrs: {[f"0x{a:06X}" for a in addrs]}')

    for addr in addrs:
        print(f'\n=== 0x{addr:06X} ===')
        nb = bucket(n, addr)
        ob = bucket(o, addr)

        # Per-game_mode overlapping-window restriction.  The two sides' loggers
        # are armed at different wall_frames; because native runs ~20x faster
        # headless, its (gm, if) capture window is shifted vs oracle's.  Only
        # (gm, if) tuples where BOTH sides were actively recording are
        # trustworthy — outside that, "side-only" just means the other side's
        # logger wasn't armed yet (or had stopped).
        n_gms = {gm for gm, _ in nb}
        o_gms = {gm for gm, _ in ob}
        common_gms = sorted(n_gms & o_gms)
        windows = {}  # gm -> (lo_if, hi_if)
        for gm in common_gms:
            n_ifs = [f for g, f in nb if g == gm]
            o_ifs = [f for g, f in ob if g == gm]
            lo = max(min(n_ifs), min(o_ifs))
            hi = min(max(n_ifs), max(o_ifs))
            if lo <= hi:
                windows[gm] = (lo, hi)
        print(f'  overlapping windows: {[(gm, windows[gm]) for gm in sorted(windows)]}')

        def in_window(key):
            gm, f = key
            w = windows.get(gm)
            return w is not None and w[0] <= f <= w[1]

        all_keys = (set(nb) | set(ob))
        keys = [k for k in all_keys if in_window(k)]
        common = sorted(k for k in keys if k in nb and k in ob)
        only_n = sorted(k for k in keys if k in nb and k not in ob)
        only_o = sorted(k for k in keys if k in ob and k not in nb)
        print(f'  in-window: common={len(common)}, native-only={len(only_n)}, oracle-only={len(only_o)}')
        if only_n[:3]:
            print(f'  first in-window native-only: {only_n[:3]}')
        if only_o[:3]:
            print(f'  first in-window oracle-only: {only_o[:3]}')

        diffs = 0
        for key in common:
            nv = [r['val'] for r in nb[key]]
            ov = [r['val'] for r in ob[key]]
            if nv != ov:
                diffs += 1
                if diffs <= args.max_diffs:
                    gm, f = key
                    print(f'  diff  (gm={gm}, if={f})')
                    print(f'    native({len(nv)}): {fmt_seq(nb[key], args.show_chain)}')
                    print(f'    oracle({len(ov)}): {fmt_seq(ob[key], args.show_chain)}')
        print(f'  total diffs: {diffs} / {len(common)} common buckets')


if __name__ == '__main__':
    main()
