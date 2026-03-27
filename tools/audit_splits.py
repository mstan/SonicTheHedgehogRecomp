#!/usr/bin/env python3
"""
Audit extra_func entries for interior label splits.

Compares each short generated function against the disassembly to
determine if it's a legitimate JMP table trampoline or a bad split
of an existing function.

A legitimate trampoline: calls one function and returns (1 instruction)
A bad split: does partial work (writes a register, reads a byte)
  that's clearly the middle of a larger operation.
"""
import re, os

SONIC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Read generated code
with open(os.path.join(SONIC_DIR, "generated", "sonic_full.c")) as f:
    lines = f.readlines()

# Find all function starts and their sizes
funcs = []
for i, line in enumerate(lines):
    m = re.match(r'^void (func_([0-9A-Fa-f]+))\(void\) \{', line)
    if m:
        funcs.append((int(m.group(2), 16), m.group(1), i))

# Read extra_func from game.cfg
extra = set()
with open(os.path.join(SONIC_DIR, "game.cfg")) as f:
    for line in f:
        m = re.match(r'\s*extra_func\s+(?:0x)?([0-9A-Fa-f]+)', line)
        if m:
            extra.add(int(m.group(1), 16))

# Read blacklist
blacklist = set()
bl_path = os.path.join(SONIC_DIR, "blacklist.txt")
if os.path.exists(bl_path):
    with open(bl_path) as f:
        for line in f:
            line = line.split("#")[0].strip()
            if line:
                blacklist.add(int(line, 16))

# Classify each short extra_func function
trampolines = []  # Legitimate: calls one func and returns
bare_returns = []  # Just "return;" — possibly end-of-table marker
partial_ops = []   # Partial operations — likely splits
unclear = []

for idx in range(len(funcs) - 1):
    addr, name, start = funcs[idx]
    _, _, next_start = funcs[idx + 1]

    if addr not in extra:
        continue
    if addr in blacklist:
        continue

    body_lines = lines[start:next_start]
    instrs = [l.strip() for l in body_lines if '/* $' in l]
    code = ''.join(body_lines)

    if len(instrs) > 3:
        continue  # Not suspiciously short

    # Classify
    if re.search(r'func_[0-9A-Fa-f]+\(\);\s*return;', code) and len(instrs) <= 1:
        trampolines.append((addr, instrs))
    elif 'return;\n' in code and len(instrs) <= 1 and 'func_' not in code.split('{', 1)[-1].split('}', 1)[0]:
        bare_returns.append((addr, instrs))
    elif len(instrs) <= 2:
        # Partial operation — this is likely a split
        snippet = [l.strip() for l in body_lines if l.strip() and not l.strip().startswith('//') and not l.strip().startswith('/*') and 'g_cycle' not in l and 'glue_check' not in l]
        partial_ops.append((addr, len(instrs), snippet[1:4] if len(snippet) > 1 else []))
    else:
        unclear.append((addr, instrs))

print(f"=== Audit of short extra_func functions ===\n")
print(f"Trampolines (legitimate JMP table entries): {len(trampolines)}")
print(f"Bare returns (end-of-table/NOP): {len(bare_returns)}")
print(f"Partial operations (LIKELY SPLITS): {len(partial_ops)}")
print(f"Unclear: {len(unclear)}")

if partial_ops:
    print(f"\n=== LIKELY SPLITS — candidates for blacklist ===\n")
    for addr, ninstr, snippet in sorted(partial_ops):
        print(f"  0x{addr:06X} ({ninstr} instrs):")
        for s in snippet:
            print(f"    {s[:100]}")
        print()

if bare_returns:
    print(f"\n=== BARE RETURNS (review — might be legitimate) ===\n")
    for addr, instrs in sorted(bare_returns):
        print(f"  0x{addr:06X}")

# Write potential blacklist entries
if partial_ops:
    out_path = os.path.join(SONIC_DIR, "run", "audit_splits.txt")
    with open(out_path, "w") as f:
        for addr, _, _ in sorted(partial_ops):
            f.write(f"0x{addr:06X}\n")
    print(f"\nWrote {len(partial_ops)} candidates to {out_path}")
