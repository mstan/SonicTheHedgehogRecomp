#!/usr/bin/env python3
"""Correlate FM register writes around the first ring-SFX (0xB5) dispatch.

Reads both native and oracle mem-write logs that capture:
  - YM2612 bus: $A04000 (part1 addr), $A04002 (part1 data),
                $A04004 (part2 addr), $A04006 (part2 data)
  - Game sound queue: $FFF00B (v_soundqueue1)

Finds the first write of 0xB5 to $FFF00B in each log, then reconstructs
the FM (part, register, value) stream for the window [wall-1 .. wall+K].
Diff the two streams side-by-side.
"""
import argparse, sys, collections


def parse(path):
    rows = []
    with open(path) as f:
        for ln in f:
            if ln.startswith('#') or not ln.strip(): continue
            parts = ln.split()
            wall = int(parts[0]); internal = int(parts[1])
            gm = int(parts[2]); addr = int(parts[3], 16)
            val = int(parts[4], 16)
            rows.append((wall, internal, gm, addr, val))
    return rows


def first_b5_wall(rows):
    for r in rows:
        if r[3] == 0xFFF00B and r[4] == 0xB5:
            return r[0]
    return None


def fm_stream_in_window(rows, w_lo, w_hi):
    """Return list of (wall, part, reg_latched, value) for bus writes in [w_lo, w_hi]."""
    out = []
    latch = {1: None, 2: None}   # latched register address per YM2612 part
    for w, i, gm, addr, val in rows:
        if w < w_lo or w > w_hi: continue
        if addr == 0xA04000:
            latch[1] = val & 0xFF
        elif addr == 0xA04002:
            if latch[1] is not None:
                out.append((w, 1, latch[1], val & 0xFF))
        elif addr == 0xA04004:
            latch[2] = val & 0xFF
        elif addr == 0xA04006:
            if latch[2] is not None:
                out.append((w, 2, latch[2], val & 0xFF))
    return out


def summarize(stream):
    cnt = collections.Counter((s[1], s[2]) for s in stream)
    return cnt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('native_log')
    ap.add_argument('oracle_log')
    ap.add_argument('--window-before', type=int, default=2)
    ap.add_argument('--window-after',  type=int, default=20)
    args = ap.parse_args()

    nr = parse(args.native_log); orr = parse(args.oracle_log)
    wn = first_b5_wall(nr); wo = first_b5_wall(orr)
    print(f"native: first 0xB5 at wall {wn}")
    print(f"oracle: first 0xB5 at wall {wo}")
    if wn is None or wo is None:
        print("no ring in one of the logs; try longer run"); return 2

    sn = fm_stream_in_window(nr, wn - args.window_before, wn + args.window_after)
    so = fm_stream_in_window(orr, wo - args.window_before, wo + args.window_after)

    print(f"\nnative FM writes in window [{wn-args.window_before},{wn+args.window_after}]: {len(sn)}")
    print(f"oracle FM writes in window [{wo-args.window_before},{wo+args.window_after}]: {len(so)}")

    cn = summarize(sn); co = summarize(so)
    all_keys = sorted(set(cn) | set(co))
    print("\n(part, reg_hex)  native_count  oracle_count  delta")
    print("-" * 52)
    diffs = 0
    for k in all_keys:
        a = cn.get(k, 0); b = co.get(k, 0)
        if a != b: diffs += 1
        mark = "  <<<" if a != b else ""
        print(f"  ({k[0]}, 0x{k[1]:02X})       {a:5}         {b:5}       {a-b:+d}{mark}")
    print(f"\nregisters with count mismatch: {diffs}/{len(all_keys)}")

    # Paired diff: align so B5 queue wall is index 0 for each.
    def rel(stream, anchor):
        return [(s[0] - anchor, s[1], s[2], s[3]) for s in stream]
    sn_r = rel(sn, wn); so_r = rel(so, wo)
    print("\n=== paired diff (dwall, part, reg, val)  N | O ===")
    i = j = 0; mismatches = 0
    while i < len(sn_r) or j < len(so_r):
        a = sn_r[i] if i < len(sn_r) else None
        b = so_r[j] if j < len(so_r) else None
        same = a == b
        if same:
            print(f"   {a}")
            i += 1; j += 1
        else:
            mismatches += 1
            print(f"  *N: {a}")
            print(f"   O: {b}")
            # advance the one that's behind by wall index
            if a is None: j += 1
            elif b is None: i += 1
            else:
                if a[0] < b[0]: i += 1
                elif b[0] < a[0]: j += 1
                else: i += 1; j += 1
        if mismatches > 40:
            print("  ... (more mismatches elided)"); break
    print(f"\nmismatches: {mismatches}")

    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
