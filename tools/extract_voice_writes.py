#!/usr/bin/env python3
"""Extract voice/operator register writes from FM trace."""
import sys

path = sys.argv[1] if len(sys.argv) > 1 else 'fm_trace_native.log'
with open(path) as f:
    lines = [l.strip().split() for l in f if l.strip() and not l.startswith('#')]

i = 0
while i + 1 < len(lines):
    f1, c1, a1, v1 = lines[i]
    f2, c2, a2, v2 = lines[i+1]
    if a1 == a2 and f1 == f2:
        reg = int(v1, 16)
        val = int(v2, 16)
        port = 0 if a1 == '0xA04000' else 1
        # Voice/operator regs: $30-$9F, Algo/FB: $B0-$B3, Pan: $B4-$B7
        if 0x30 <= reg <= 0xB7:
            ch = reg & 3
            op_names = {0x30:"DT/MUL",0x40:"TL",0x50:"AR/KS",0x60:"D1R",
                        0x70:"D2R",0x80:"D1L/RR",0x90:"SSG",0xB0:"Algo/FB",0xB4:"Pan/AMS"}
            base = reg & 0xFC
            name = op_names.get(base, f"${reg:02X}")
            op = (reg >> 2) & 3
            print(f"  frame={f1:>3} port={port} ch={ch} {name:>8} op{op} = ${val:02X}")
        # Freq regs
        elif reg in (0xA0,0xA1,0xA2,0xA4,0xA5,0xA6):
            ch = reg & 3
            hi = "Hi" if reg >= 0xA4 else "Lo"
            print(f"  frame={f1:>3} port={port} ch={ch} Freq{hi:>2}      = ${val:02X}")
        # Key on/off
        elif reg == 0x28:
            ch_bits = val & 7
            ops = (val >> 4) & 0xF
            onoff = "ON" if ops else "OFF"
            print(f"  frame={f1:>3}              Key {onoff:>3} ch={ch_bits} ops=${ops:X}")
        i += 2
    else:
        i += 1
