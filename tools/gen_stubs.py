#!/usr/bin/env python3
"""Generate sonic_stubs.c excluding functions already in chunk files."""
import glob, re, os

SONIC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN_DIR = os.path.join(SONIC_DIR, "generated")

# Collect all func_ defined in chunks
defined = set()
for chunk in glob.glob(os.path.join(GEN_DIR, "sonic_chunk_*.c")) + glob.glob(os.path.join(GEN_DIR, "sonic_full.c")):
    with open(chunk) as f:
        for m in re.finditer(r'^void func_([0-9A-Fa-f]+)', f.read(), re.MULTILINE):
            defined.add(m.group(1).upper())

# Collect all func_ REFERENCED in chunks
referenced = set()
for chunk in glob.glob(os.path.join(GEN_DIR, "sonic_chunk_*.c")) + glob.glob(os.path.join(GEN_DIR, "sonic_full.c")):
    with open(chunk) as f:
        for m in re.finditer(r'func_([0-9A-Fa-f]+)', f.read()):
            referenced.add(m.group(1).upper())

# Also check dispatch
disp = os.path.join(GEN_DIR, "sonic_dispatch.c")
if os.path.exists(disp):
    with open(disp) as f:
        for m in re.finditer(r'func_([0-9A-Fa-f]+)', f.read()):
            referenced.add(m.group(1).upper())

# Stubs needed: referenced but not defined
needed = sorted(referenced - defined)
print(f"Defined: {len(defined)}, Referenced: {len(referenced)}, Need stubs: {len(needed)}")

with open(os.path.join(GEN_DIR, "sonic_stubs.c"), "w") as f:
    f.write("/* sonic_stubs.c — AUTO-GENERATED. Stubs for unresolved references. */\n")
    f.write("#include <stdio.h>\n\n")
    f.write("static void bad_stub(unsigned a) {\n")
    f.write('    fprintf(stderr, "[STUB] func_%08X called!\\n", a);\n}\n\n')
    for addr in needed:
        f.write(f"void func_{addr}(void) {{ bad_stub(0x{addr}u); }}\n")

print(f"Wrote {len(needed)} stubs to generated/sonic_stubs.c")
