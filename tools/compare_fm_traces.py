#!/usr/bin/env python3
"""
compare_fm_traces.py — Compare FM register write timing between two trace logs.

Usage:
    python compare_fm_traces.py fm_trace_interp.log fm_trace_native.log

Each log line: frame master_cycle address value
Header line starts with #.

Outputs:
  1. Per-frame summary: how many FM writes and total cycle span
  2. Per-write delta: cycle gap between consecutive FM writes
  3. Divergence report: where native timing differs from interp
"""
import sys
from collections import defaultdict

def parse_log(path):
    """Parse FM trace log. Returns list of (frame, cycle, addr, val) tuples."""
    entries = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) != 4:
                continue
            frame = int(parts[0])
            cycle = int(parts[1])
            addr = int(parts[2], 16)
            val = int(parts[3], 16)
            entries.append((frame, cycle, addr, val))
    return entries

def frame_stats(entries):
    """Group entries by frame, compute stats."""
    by_frame = defaultdict(list)
    for frame, cycle, addr, val in entries:
        by_frame[frame].append((cycle, addr, val))

    stats = {}
    for frame in sorted(by_frame.keys()):
        writes = by_frame[frame]
        cycles = [w[0] for w in writes]
        min_c = min(cycles)
        max_c = max(cycles)

        # Compute inter-write gaps
        gaps = []
        for i in range(1, len(writes)):
            gaps.append(writes[i][0] - writes[i-1][0])

        stats[frame] = {
            'count': len(writes),
            'min_cycle': min_c,
            'max_cycle': max_c,
            'span': max_c - min_c,
            'gaps': gaps,
            'avg_gap': sum(gaps) / len(gaps) if gaps else 0,
        }
    return stats

def main():
    if len(sys.argv) < 3:
        print("Usage: compare_fm_traces.py <interp.log> <native.log>")
        sys.exit(1)

    interp_path = sys.argv[1]
    native_path = sys.argv[2]

    interp = parse_log(interp_path)
    native = parse_log(native_path)

    print(f"Interp: {len(interp)} FM writes")
    print(f"Native: {len(native)} FM writes")
    print()

    i_stats = frame_stats(interp)
    n_stats = frame_stats(native)

    # Find common frames (skip first few for init)
    common = sorted(set(i_stats.keys()) & set(n_stats.keys()))
    if not common:
        print("No common frames found!")
        sys.exit(1)

    # Skip frames before music starts (look for frames with >5 writes)
    music_frames = [f for f in common if i_stats[f]['count'] > 5]
    if not music_frames:
        print("No music frames found (need frames with >5 FM writes)")
        sys.exit(1)

    print(f"=== Per-Frame Comparison (first 20 music frames) ===")
    print(f"{'Frame':>6} | {'Interp':>18} | {'Native':>18} | {'Ratio':>8}")
    print(f"{'':>6} | {'writes  span  avg':>18} | {'writes  span  avg':>18} | {'span':>8}")
    print("-" * 75)

    span_ratios = []
    gap_ratios = []

    for frame in music_frames[:20]:
        ist = i_stats[frame]
        nst = n_stats[frame]
        ratio = nst['span'] / ist['span'] if ist['span'] > 0 else 0
        span_ratios.append(ratio)
        if ist['avg_gap'] > 0 and nst['avg_gap'] > 0:
            gap_ratios.append(nst['avg_gap'] / ist['avg_gap'])

        print(f"{frame:>6} | {ist['count']:>5}  {ist['span']:>5}  {ist['avg_gap']:>5.0f} | "
              f"{nst['count']:>5}  {nst['span']:>5}  {nst['avg_gap']:>5.0f} | {ratio:>7.3f}x")

    print()
    if span_ratios:
        avg_ratio = sum(span_ratios) / len(span_ratios)
        print(f"Average cycle span ratio (native/interp): {avg_ratio:.3f}x")
    if gap_ratios:
        avg_gap_ratio = sum(gap_ratios) / len(gap_ratios)
        print(f"Average inter-write gap ratio: {avg_gap_ratio:.3f}x")

    print()
    print(f"=== Cycle Range Comparison ===")
    for frame in music_frames[:5]:
        ist = i_stats[frame]
        nst = n_stats[frame]
        print(f"\nFrame {frame}:")
        print(f"  Interp: cycles {ist['min_cycle']}-{ist['max_cycle']} (span {ist['span']})")
        print(f"  Native: cycles {nst['min_cycle']}-{nst['max_cycle']} (span {nst['span']})")

    # Detailed gap distribution
    print()
    print(f"=== Inter-Write Gap Distribution (all music frames) ===")
    i_all_gaps = []
    n_all_gaps = []
    for frame in music_frames:
        i_all_gaps.extend(i_stats[frame]['gaps'])
        n_all_gaps.extend(n_stats[frame]['gaps'])

    if i_all_gaps and n_all_gaps:
        i_all_gaps.sort()
        n_all_gaps.sort()
        for pct in [10, 25, 50, 75, 90]:
            i_val = i_all_gaps[len(i_all_gaps) * pct // 100]
            n_val = n_all_gaps[len(n_all_gaps) * pct // 100]
            ratio = n_val / i_val if i_val > 0 else 0
            print(f"  P{pct:>2}: interp={i_val:>6}  native={n_val:>6}  ratio={ratio:.3f}x")

if __name__ == '__main__':
    main()
