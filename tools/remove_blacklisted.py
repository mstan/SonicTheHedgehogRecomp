#!/usr/bin/env python3
"""Remove blacklisted entries from game.cfg."""
import sys

BLACKLIST = "F:/Projects/segagenesisrecomp-v2/sonicthehedgehog/blacklist.txt"
GAMECFG = "F:/Projects/segagenesisrecomp/sonicthehedgehog/game.cfg"

# Parse blacklist
blacklist = set()
with open(BLACKLIST) as f:
    for line in f:
        addr = line.split('#')[0].strip()
        if addr:
            # Normalize to uppercase, no prefix
            addr = addr.upper().replace('0X', '')
            blacklist.add(addr)

print(f"Blacklist has {len(blacklist)} entries: {sorted(blacklist)}")

# Process game.cfg
removed = 0
lines_out = []
with open(GAMECFG) as f:
    for line in f:
        stripped = line.strip()
        if stripped.startswith('extra_func'):
            parts = stripped.split()
            if len(parts) >= 2:
                addr = parts[1].upper().replace('0X', '')
                if addr in blacklist:
                    removed += 1
                    lines_out.append(f"# REMOVED (blacklisted split): {stripped}\n")
                    continue
        lines_out.append(line)

with open(GAMECFG, 'w') as f:
    f.writelines(lines_out)

print(f"Removed {removed} blacklisted entries from game.cfg")
