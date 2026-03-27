#!/usr/bin/env python3
"""
Full convergence loop: resolve ALL interpreter fallbacks, dispatch misses,
and unresolved switch table targets until the build is fully native.

Phases per round:
1. Build game (may fail with unresolved externals)
2. Extract missing func_XXXXXX symbols from linker errors → add to game.cfg
3. Run game to collect dispatch misses + interp fallbacks → add to game.cfg
4. Regenerate code with recompiler
5. Repeat until clean build + zero runtime fallbacks

Usage: python tools/converge_all.py [frames_per_run]
"""
import subprocess, os, sys, re

SONIC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECOMP_DIR = os.path.abspath(os.path.join(SONIC_DIR, "..", "..", "segagenesisrecomp", "genesisrecomp"))
RECOMP = os.path.join(RECOMP_DIR, "build", "recompiler", "Release", "GenesisRecomp.exe")
ROM = os.path.join(SONIC_DIR, "sonic.bin")
GAME_CFG = os.path.join(SONIC_DIR, "game.cfg")
EXE = os.path.join(SONIC_DIR, "build", "Release", "SonicTheHedgehogRecomp.exe")
MSBUILD = r"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
SLN_V2 = os.path.join(SONIC_DIR, "build", "SonicTheHedgehogRecomp.sln")
GENERATED = os.path.join(SONIC_DIR, "generated", "sonic_full.c")
MISSES_LOG = os.path.join(SONIC_DIR, "dispatch_misses.log")
INTERP_LOG = os.path.join(SONIC_DIR, "interp_fallbacks.log")
FRAMES = int(sys.argv[1]) if len(sys.argv) > 1 else 5000

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600, **kw)

def read_existing_extra_funcs():
    addrs = set()
    with open(GAME_CFG, "r") as f:
        for line in f:
            m = re.match(r'\s*extra_func\s+(?:0x)?([0-9A-Fa-f]+)', line)
            if m:
                addrs.add(m.group(1).upper().zfill(6))
    return addrs

def add_extra_funcs(new_addrs, label):
    if not new_addrs:
        return
    existing = read_existing_extra_funcs()
    to_add = sorted(new_addrs - existing)
    if not to_add:
        print(f"    {label}: all {len(new_addrs)} already in game.cfg")
        return
    with open(GAME_CFG, "a") as f:
        f.write(f"\n# {label}\n")
        for addr in to_add:
            f.write(f"extra_func 0x{addr}\n")
    print(f"    {label}: added {len(to_add)} new entries")

def regenerate():
    r = run([RECOMP, ROM, "--game", GAME_CFG], cwd=SONIC_DIR)
    ok = "Done" in (r.stdout + r.stderr)
    if not ok:
        print(f"    REGEN FAILED")
        print(r.stderr[-300:] if r.stderr else r.stdout[-300:])
    return ok

def build_v2():
    r = run([MSBUILD, SLN_V2, "-p:Configuration=Release", "-p:Platform=x64",
             "-verbosity:minimal"])
    output = r.stdout + r.stderr
    # Extract unresolved func_ symbols
    missing = set()
    for m in re.finditer(r'unresolved external symbol func_([0-9A-Fa-f]+)', output):
        missing.add(m.group(1).upper().zfill(6))
    ok = r.returncode == 0
    return ok, missing, output

def run_game():
    """Run the game and collect dispatch misses + interp fallbacks."""
    for f in [MISSES_LOG, INTERP_LOG]:
        if os.path.exists(f):
            os.remove(f)
    r = run([EXE, ROM, "--turbo", "--max-frames", str(FRAMES)], cwd=SONIC_DIR)
    new_addrs = set()
    for f in [MISSES_LOG, INTERP_LOG]:
        if os.path.exists(f):
            with open(f) as fh:
                for line in fh:
                    m = re.match(r'\s*extra_func\s+(?:0x)?([0-9A-Fa-f]+)', line)
                    if m:
                        new_addrs.add(m.group(1).upper().zfill(6))
            os.remove(f)
    return new_addrs

# Main loop
print(f"=== Full convergence loop (frames={FRAMES}) ===\n")

for round_num in range(1, 50):
    print(f"Round {round_num}:")

    # Phase 1: Try to build
    print("  Building...")
    ok, missing_syms, _ = build_v2()

    if missing_syms:
        print(f"  Link errors: {len(missing_syms)} unresolved symbols")
        add_extra_funcs(missing_syms, f"round {round_num} link errors")
        print("  Regenerating...")
        if not regenerate():
            break
        continue  # Rebuild without running

    if not ok:
        print("  Build failed (non-link error)")
        break

    # Phase 2: Build succeeded — run game to find runtime fallbacks
    print(f"  Running {FRAMES} frames...")
    runtime_addrs = run_game()

    if not runtime_addrs:
        # Check for interp fallbacks from hybrid_jmp/call that aren't extra_func misses
        # These won't create new entries since they're already dispatched
        print(f"  CONVERGED! Clean build + zero runtime misses/fallbacks.")
        break

    print(f"  Runtime: {len(runtime_addrs)} new addresses")
    add_extra_funcs(runtime_addrs, f"round {round_num} runtime")
    print("  Regenerating...")
    if not regenerate():
        break

else:
    print(f"\nDid not converge after 50 rounds!")

# Summary
existing = read_existing_extra_funcs()
print(f"\nTotal extra_func entries: {len(existing)}")

# Check remaining interpreter indirection
if os.path.exists(GENERATED):
    with open(GENERATED) as f:
        content = f.read()
    jmp = content.count("hybrid_jmp_interpret")
    call = content.count("hybrid_call_interpret")
    print(f"Remaining hybrid_jmp_interpret: {jmp}")
    print(f"Remaining hybrid_call_interpret: {call}")
    if jmp == 0 and call == 0:
        print("*** FULLY NATIVE — no interpreter indirection! ***")
