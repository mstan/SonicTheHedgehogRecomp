#!/usr/bin/env python3
"""Stream-diff of the FM register-write sequence from two mem-write-log
captures.  Strips every timing field (wall_frame, internal_frame, cycle, a7,
ret_chain) and compares just the ordered stream of (addr, value) pairs.

If the sequences match: the recompiled 68K is producing semantically
identical FM writes; any audio divergence is downstream (clownmdemu mixing,
SDL queue, sample-rate generation).

If they diverge: the first mismatch locates which 68K code path produced
the wrong FM value — look up the ret_chain on that specific line of the
native log for the caller.

Usage:
    python diff_fm_stream.py mem_write_log_native.log mem_write_log_oracle.log
"""
import sys

def load_stream(path):
    """Return list of (addr, value, ret0) tuples in write order.
    ret0 is carried for diagnostics only; not compared."""
    out = []
    with open(path) as f:
        for line in f:
            if line.startswith('#'): continue
            p = line.split()
            if len(p) < 7: continue
            addr = int(p[3], 16)
            val  = int(p[4], 16)
            ret0 = int(p[6], 16) if len(p) > 6 else 0
            out.append((addr, val, ret0, line.rstrip()))
    return out

def main():
    if len(sys.argv) != 3:
        sys.exit("Usage: diff_fm_stream.py native.log oracle.log")
    nat = load_stream(sys.argv[1])
    ora = load_stream(sys.argv[2])
    print(f"# native writes: {len(nat)}")
    print(f"# oracle writes: {len(ora)}")
    n = min(len(nat), len(ora))
    first_diff = -1
    for i in range(n):
        if nat[i][0] != ora[i][0] or nat[i][1] != ora[i][1]:
            first_diff = i
            break
    if first_diff < 0 and len(nat) == len(ora):
        print("# IDENTICAL STREAMS — every (addr, value) pair matches in order.")
        print("# Recompiler is semantically correct for the FM port writes;")
        print("# audio divergence must be in downstream processing.")
        return 0
    if first_diff < 0:
        first_diff = n
        print(f"# Streams agree for the first {n} writes; lengths differ "
              f"(nat={len(nat)} ora={len(ora)}).")
    print(f"\n# First divergence at stream index {first_diff}:")
    # Context window around the divergence
    lo = max(0, first_diff - 4)
    hi = min(n, first_diff + 4)
    print(f"# context native[{lo}:{hi}]:")
    for i in range(lo, hi):
        marker = "  <-- first diff" if i == first_diff else ""
        if i < len(nat):
            print(f"  nat[{i}]: addr=0x{nat[i][0]:06X} val=0x{nat[i][1]:02X}  ret0=0x{nat[i][2]:06X}{marker}")
    print(f"# context oracle[{lo}:{hi}]:")
    for i in range(lo, hi):
        marker = "  <-- first diff" if i == first_diff else ""
        if i < len(ora):
            print(f"  ora[{i}]: addr=0x{ora[i][0]:06X} val=0x{ora[i][1]:02X}  ret0=0x{ora[i][2]:06X}{marker}")
    # Count total mismatches over the common prefix
    mismatches = 0
    last_match_i = first_diff
    for i in range(first_diff, n):
        if nat[i][0] != ora[i][0] or nat[i][1] != ora[i][1]:
            mismatches += 1
    print(f"\n# over indices [{first_diff}:{n}]: {mismatches} / {n - first_diff} pairs disagree"
          f" ({100.0 * mismatches / max(n - first_diff, 1):.1f}%)")
    return 1

if __name__ == '__main__':
    sys.exit(main())
