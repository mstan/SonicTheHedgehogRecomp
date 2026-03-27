#!/usr/bin/env python3
"""
audit_sound_splits.py — Find suspiciously small functions in the sound driver
range ($071900-$072F00) that might be interior label splits.

Reads generated/sonic_full.c from the v1 tree.
"""
import re
import sys

GENERATED = "F:/Projects/segagenesisrecomp/sonicthehedgehog/generated/sonic_full.c"

with open(GENERATED) as f:
    text = f.read()

# Find all function bodies
pattern = r'void (func_([0-9A-Fa-f]+))\(void\) \{(.*?)\n\}'
matches = re.findall(pattern, text, re.DOTALL)

sound_funcs = []
for name, addr_str, body in matches:
    addr = int(addr_str, 16)
    if addr < 0x071900 or addr > 0x072F00:
        continue

    lines = body.strip().split('\n')
    # Count bus accesses (m68k_read/write calls)
    bus_ops = len(re.findall(r'm68k_(read|write)', body))
    # Count function calls
    calls = len(re.findall(r'func_[0-9A-Fa-f]+\(\)', body))
    # Check for return/goto at end
    has_return = 'return;' in body
    has_goto = 'goto ' in body
    # Count total meaningful lines
    meaningful = [l.strip() for l in lines
                  if l.strip()
                  and not l.strip().startswith('//')
                  and not l.strip().startswith('/*')
                  and 'g_cycle_accumulator' not in l
                  and 'glue_check_vblank' not in l]

    sound_funcs.append({
        'name': name,
        'addr': addr,
        'total_lines': len(lines),
        'meaningful': len(meaningful),
        'bus_ops': bus_ops,
        'calls': calls,
        'has_return': has_return,
        'body_preview': body.strip()[:200],
    })

# Sort by address
sound_funcs.sort(key=lambda f: f['addr'])

print(f"Sound driver functions: {len(sound_funcs)}")
print()

# Flag suspicious ones
print("=== SUSPICIOUS (<=3 meaningful lines, likely splits) ===")
for f in sound_funcs:
    if f['meaningful'] <= 3:
        print(f"\n{f['name']} (0x{f['addr']:06X}): {f['meaningful']} meaningful, {f['bus_ops']} bus ops, {f['calls']} calls")
        print(f"  Preview: {f['body_preview'][:150]}")

print()
print("=== VERY SHORT (4-8 meaningful lines) ===")
for f in sound_funcs:
    if 4 <= f['meaningful'] <= 8:
        print(f"  {f['name']} (0x{f['addr']:06X}): {f['meaningful']} meaningful, {f['bus_ops']} bus ops, {f['calls']} calls")

print()
print("=== ALL FUNCTIONS BY SIZE ===")
for f in sound_funcs:
    flag = " *** TINY" if f['meaningful'] <= 3 else ""
    print(f"  0x{f['addr']:06X}  {f['meaningful']:>3} lines  {f['bus_ops']:>2} bus  {f['calls']:>2} calls{flag}")
