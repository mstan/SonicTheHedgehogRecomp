#!/usr/bin/env python3
"""
audit_all_splits.py — Find ALL suspiciously small functions in the generated
code that might be interior label splits, regardless of whether they came
from game.cfg or the recompiler's static analysis.

A function that is just "return;" or has <=2 meaningful operations and no
function calls is likely an interior label split that cuts off the parent.
"""
import re
import sys

GENERATED = "F:/Projects/segagenesisrecomp/sonicthehedgehog/generated/sonic_full.c"

with open(GENERATED) as f:
    text = f.read()

pattern = r'void (func_([0-9A-Fa-f]+))\(void\) \{(.*?)\n\}'
matches = re.findall(pattern, text, re.DOTALL)

suspects = []
for name, addr_str, body in matches:
    addr = int(addr_str, 16)
    lines = body.strip().split('\n')

    # Count meaningful lines (not cycle checks, not comments)
    meaningful = [l.strip() for l in lines
                  if l.strip()
                  and not l.strip().startswith('//')
                  and not l.strip().startswith('/*')
                  and 'g_cycle_accumulator' not in l
                  and 'glue_check_vblank' not in l]

    bus_ops = len(re.findall(r'm68k_(read|write)', body))
    calls = re.findall(r'func_([0-9A-Fa-f]+)\(\)', body)
    has_return = 'return;' in body
    has_goto = 'goto ' in body

    # Classify
    is_bare_rts = len(meaningful) == 1 and meaningful[0] == 'return;'
    is_trampoline = len(meaningful) == 1 and len(calls) == 1  # just calls one func
    is_tiny = len(meaningful) <= 3 and not calls

    # Check if function body ends WITHOUT return (falls off end = missing tail)
    body_stripped = body.strip()
    last_meaningful = None
    for l in reversed(body_stripped.split('\n')):
        l = l.strip()
        if l and 'g_cycle_accumulator' not in l and 'glue_check_vblank' not in l:
            last_meaningful = l
            break
    falls_off = last_meaningful and 'return' not in last_meaningful and 'func_' not in last_meaningful and 'goto' not in last_meaningful

    if is_bare_rts or is_tiny or falls_off:
        category = "BARE_RTS" if is_bare_rts else ("FALLS_OFF" if falls_off else "TINY")
        suspects.append({
            'name': name,
            'addr': addr,
            'category': category,
            'meaningful': len(meaningful),
            'bus_ops': bus_ops,
            'calls': len(calls),
            'falls_off': falls_off,
            'preview': body.strip()[:300],
        })

# Exclude known-legitimate trampolines (JMP table entries that just call another func)
# These have exactly 1 meaningful line which is a function call + return
legit_trampolines = [s for s in suspects if s['meaningful'] == 1 and s['calls'] == 1]
real_suspects = [s for s in suspects if not (s['meaningful'] == 1 and s['calls'] == 1)]

print(f"Total functions: {len(matches)}")
print(f"Legitimate trampolines (1 call + return): {len(legit_trampolines)}")
print(f"Suspicious splits: {len(real_suspects)}")
print()

print("=== FALLS OFF END (most dangerous — parent function is truncated) ===")
for s in real_suspects:
    if s['category'] == 'FALLS_OFF':
        print(f"\n{s['name']} (0x{s['addr']:06X}): {s['meaningful']} lines, {s['bus_ops']} bus, {s['calls']} calls")
        print(f"  {s['preview'][:200]}")

print()
print("=== BARE RTS (may truncate preceding function) ===")
for s in real_suspects:
    if s['category'] == 'BARE_RTS':
        print(f"  {s['name']} (0x{s['addr']:06X})")

print()
print("=== TINY (<=3 lines, no calls — likely interior) ===")
for s in real_suspects:
    if s['category'] == 'TINY' and not s['falls_off']:
        print(f"\n  {s['name']} (0x{s['addr']:06X}): {s['meaningful']} lines, {s['bus_ops']} bus")
        print(f"    {s['preview'][:150]}")
