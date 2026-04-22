#!/usr/bin/env python3
"""Extract pure (part, reg, val) sequence from each log and diff.

Intent: remove wall-timing from the comparison. If the YM2612 register
write sequence is content-identical, any audible divergence must be
below this level (sub-wall phase, DAC, PSG, resampler).
"""
import sys


def extract(path):
    latch = {1: None, 2: None}
    seq = []
    with open(path) as f:
        for ln in f:
            if ln.startswith('#') or not ln.strip(): continue
            parts = ln.split()
            addr = int(parts[3], 16); val = int(parts[4], 16) & 0xFF
            if addr == 0xA04000:
                latch[1] = val
            elif addr == 0xA04002:
                if latch[1] is not None:
                    seq.append((1, latch[1], val))
            elif addr == 0xA04004:
                latch[2] = val
            elif addr == 0xA04006:
                if latch[2] is not None:
                    seq.append((2, latch[2], val))
    return seq


def find_sfx_init_start(seq):
    """Find the position of 0xB0 write to reg 0x30 — the ring SFX init signature."""
    for i, (p, r, v) in enumerate(seq):
        if p == 1 and r == 0x30 and v == 0xB0:
            return i
    return None


def main():
    nat_seq = extract(sys.argv[1])
    ora_seq = extract(sys.argv[2])
    print(f"native total (part,reg,val) writes: {len(nat_seq)}")
    print(f"oracle total (part,reg,val) writes: {len(ora_seq)}")

    # Find first divergence in pure content.
    n_common = min(len(nat_seq), len(ora_seq))
    first_div = None
    for k in range(n_common):
        if nat_seq[k] != ora_seq[k]:
            first_div = k; break
    if first_div is None:
        print(f"no content divergence in first {n_common} writes")
        return
    print(f"\nfirst content divergence at index {first_div}")
    lo = max(0, first_div - 5); hi = min(n_common, first_div + 15)
    for k in range(lo, hi):
        a = nat_seq[k] if k < len(nat_seq) else None
        b = ora_seq[k] if k < len(ora_seq) else None
        mark = "  <<<" if a != b else ""
        print(f"  [{k:4}]  N: {a}  O: {b}{mark}")

    ni = find_sfx_init_start(nat_seq)
    oi = find_sfx_init_start(ora_seq)
    print(f"\nnative first SFX-init marker at pos {ni}")
    print(f"oracle first SFX-init marker at pos {oi}")

    # Compare 100 entries starting from each anchor.
    N = 100
    print(f"\n=== paired content diff, next {N} entries from SFX init ===")
    mismatch = 0
    for k in range(N):
        a = nat_seq[ni + k] if ni is not None and ni + k < len(nat_seq) else None
        b = ora_seq[oi + k] if oi is not None and oi + k < len(ora_seq) else None
        if a != b:
            mismatch += 1
            print(f"  [{k:3}]  N: {a}  O: {b}")
    print(f"\nmismatches in first {N}: {mismatch}")

    # Also diff the PRE-SFX context (music continuation).
    W = 50
    print(f"\n=== pre-SFX context (last {W} entries before init) ===")
    pre_n = nat_seq[max(0, ni-W):ni] if ni else []
    pre_o = ora_seq[max(0, oi-W):oi] if oi else []
    print(f"  native pre-SFX count: {len(pre_n)}")
    print(f"  oracle pre-SFX count: {len(pre_o)}")
    if pre_n == pre_o:
        print("  pre-SFX sequences match exactly")
    else:
        print("  pre-SFX sequences DIFFER")
        for k in range(max(len(pre_n), len(pre_o))):
            a = pre_n[-k-1] if k < len(pre_n) else None
            b = pre_o[-k-1] if k < len(pre_o) else None
            if a != b:
                print(f"  (reverse idx {k})  N: {a}  O: {b}")
                if k > 15: break


if __name__ == '__main__':
    main()
