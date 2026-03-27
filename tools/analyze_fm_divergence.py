#!/usr/bin/env python3
"""
analyze_fm_divergence.py — Deep analysis of FM register value differences.

Pairs up address-port / data-port writes to reconstruct actual FM register
operations, then compares interp vs native to find exactly which registers
have wrong values and by how much.

In Sonic 1 SMPS, FM writes are byte pairs via m68k_write8:
  write $A04000, reg_addr   (or $A04002 for port 1)
  write $A04000, reg_data   (or $A04002 for port 1)

The trace captures both as address 0xA04000/0xA04002.
Consecutive pairs = (register, value).
"""
import sys
from collections import defaultdict

def parse_log(path):
    entries = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            frame = int(parts[0])
            cycle = int(parts[1])
            addr = int(parts[2], 16)
            val = int(parts[3], 16)
            entries.append((frame, cycle, addr, val))
    return entries

def pair_writes(entries):
    """Pair consecutive writes into (port, register, value, frame, cycle) tuples."""
    pairs = []
    i = 0
    while i + 1 < len(entries):
        f1, c1, a1, v1 = entries[i]
        f2, c2, a2, v2 = entries[i + 1]
        # Same port, consecutive = register + data
        if a1 == a2 and f1 == f2:
            port = 0 if a1 == 0xA04000 else 1
            pairs.append({
                'port': port,
                'reg': v1,
                'val': v2,
                'frame': f1,
                'cycle': c1,
            })
            i += 2
        else:
            i += 1
    return pairs

def reg_name(port, reg):
    """Human-readable FM register name."""
    names = {
        0x28: "KeyOnOff",
        0x22: "LFO",
        0x27: "Ch3Mode",
        0x2B: "DACEnable",
    }
    if reg in names:
        return names[reg]
    if 0x30 <= reg <= 0x9F:
        op = (reg >> 2) & 3
        ch = reg & 3
        base = reg & 0xFC
        op_names = {0x30: "DT/MUL", 0x40: "TL", 0x50: "AR/KS", 0x60: "D1R",
                     0x70: "D2R", 0x80: "D1L/RR", 0x90: "SSG-EG"}
        name = op_names.get(base, f"Op{base:02X}")
        return f"{name} ch{ch} op{op}"
    if 0xA0 <= reg <= 0xA6:
        ch = reg & 3
        return f"FreqLo ch{ch}" if reg < 0xA4 else f"FreqHi ch{ch}"
    if 0xB0 <= reg <= 0xB6:
        ch = reg & 3
        return f"Algo/FB ch{ch}" if reg < 0xB4 else f"Pan/AMS/FMS ch{ch}"
    return f"Reg${reg:02X}"

def main():
    if len(sys.argv) < 3:
        print("Usage: analyze_fm_divergence.py <interp.log> <native.log>")
        sys.exit(1)

    interp = parse_log(sys.argv[1])
    native = parse_log(sys.argv[2])

    i_pairs = pair_writes(interp)
    n_pairs = pair_writes(native)

    print(f"Interp: {len(i_pairs)} register writes")
    print(f"Native: {len(n_pairs)} register writes")

    # Group by frame
    i_by_frame = defaultdict(list)
    n_by_frame = defaultdict(list)
    for p in i_pairs:
        i_by_frame[p['frame']].append(p)
    for p in n_pairs:
        n_by_frame[p['frame']].append(p)

    # Find common frames
    common = sorted(set(i_by_frame.keys()) & set(n_by_frame.keys()))
    print(f"Common frames: {len(common)}")
    print()

    # Compare register writes in common frames
    divergent_regs = defaultdict(list)  # reg_name -> list of (interp_val, native_val, frame)
    match_count = 0
    diff_count = 0

    for frame in common:
        iw = i_by_frame[frame]
        nw = n_by_frame[frame]

        # Match by register address sequence
        min_len = min(len(iw), len(nw))
        for j in range(min_len):
            ip, np = iw[j], nw[j]
            if ip['port'] == np['port'] and ip['reg'] == np['reg']:
                if ip['val'] == np['val']:
                    match_count += 1
                else:
                    diff_count += 1
                    rn = reg_name(ip['port'], ip['reg'])
                    divergent_regs[rn].append((ip['val'], np['val'], frame))

    print(f"Matching writes: {match_count}")
    print(f"Divergent writes: {diff_count}")
    print()

    if not divergent_regs:
        print("No divergences found in common frames!")
        return

    print("=== DIVERGENT REGISTERS (sorted by frequency) ===")
    for rn, diffs in sorted(divergent_regs.items(), key=lambda x: -len(x[1])):
        print(f"\n  {rn}: {len(diffs)} divergences")
        # Show value distribution
        deltas = [n - i for i, n, f in diffs]
        for i_val, n_val, frame in diffs[:5]:
            print(f"    frame {frame}: interp=0x{i_val:02X} native=0x{n_val:02X} (delta={n_val-i_val:+d})")
        if len(diffs) > 5:
            print(f"    ... and {len(diffs)-5} more")

        # Check if delta is consistent
        unique_deltas = set(deltas)
        if len(unique_deltas) == 1:
            print(f"    ** CONSISTENT delta: {deltas[0]:+d} **")
        elif len(unique_deltas) <= 3:
            print(f"    ** Near-consistent deltas: {sorted(unique_deltas)} **")

    # Frequency-specific analysis
    print()
    print("=== FREQUENCY REGISTER ANALYSIS ===")
    freq_regs = {k: v for k, v in divergent_regs.items() if 'Freq' in k}
    if freq_regs:
        for rn, diffs in sorted(freq_regs.items()):
            print(f"\n  {rn}:")
            for i_val, n_val, frame in diffs[:10]:
                # For FreqHi: bits 5-3 = octave, bits 2-0 + FreqLo = F-number
                if 'Hi' in rn:
                    i_oct = (i_val >> 3) & 7
                    n_oct = (n_val >> 3) & 7
                    i_fhi = i_val & 7
                    n_fhi = n_val & 7
                    print(f"    frame {frame}: interp=0x{i_val:02X} (oct={i_oct} fhi={i_fhi}) "
                          f"native=0x{n_val:02X} (oct={n_oct} fhi={n_fhi})")
                else:
                    print(f"    frame {frame}: interp=0x{i_val:02X} native=0x{n_val:02X}")
    else:
        print("  No frequency register divergences in common frames")

if __name__ == '__main__':
    main()
