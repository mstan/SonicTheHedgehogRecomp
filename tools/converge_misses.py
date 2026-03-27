#!/usr/bin/env python3
"""
Iterate: ingest dispatch misses → regenerate → rebuild → run until no new misses.
"""
import subprocess, os, sys

SONIC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
V1_DIR = os.path.abspath(os.path.join(SONIC_DIR, "..", "..", "segagenesisrecomp", "sonicthehedgehog"))
RECOMP = os.path.join(SONIC_DIR, "..", "..", "segagenesisrecomp", "genesisrecomp",
                       "build", "recompiler", "Release", "GenesisRecomp.exe")
ROM = os.path.join(SONIC_DIR, "sonic.bin")
GAME_CFG = os.path.join(V1_DIR, "game.cfg")
EXE = os.path.join(SONIC_DIR, "build", "Release", "SonicTheHedgehogRecomp.exe")
MSBUILD = r"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
SLN = os.path.join(SONIC_DIR, "build", "SonicTheHedgehogRecomp.sln")
MISSES = os.path.join(SONIC_DIR, "dispatch_misses.log")
FRAMES = int(sys.argv[1]) if len(sys.argv) > 1 else 5000  # default ~83s at 60fps

def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    return r

for round_num in range(1, 20):
    print(f"\n{'='*60}")
    print(f"Round {round_num}: running {FRAMES} frames...")

    # Run game
    r = run([EXE, ROM, "--turbo", "--max-frames", str(FRAMES)], cwd=SONIC_DIR)

    # Check for misses AND interp fallbacks
    INTERP_LOG = os.path.join(SONIC_DIR, "interp_fallbacks.log")
    has_misses = os.path.exists(MISSES)
    has_interp = os.path.exists(INTERP_LOG)
    if not has_misses and not has_interp:
        print(f"  CONVERGED! No dispatch misses or interp fallbacks after {FRAMES} frames.")
        break

    total_new = 0
    for logf in [MISSES, INTERP_LOG]:
        if os.path.exists(logf):
            with open(logf) as f:
                n = len([l for l in f if l.strip()])
            total_new += n
            print(f"  {logf}: {n} entries")
    print(f"  {total_new} total new entries")

    # Ingest
    r = run([sys.executable, os.path.join(SONIC_DIR, "tools", "ingest_misses.py"),
             "--misses", MISSES, "--cfg", GAME_CFG])
    print(f"  ingest: {r.stdout.strip().split(chr(10))[-1] if r.stdout else r.stderr}")

    # Regenerate
    r = run([RECOMP, ROM, "--game", GAME_CFG], cwd=V1_DIR)
    if "Done" not in (r.stdout + r.stderr):
        print(f"  REGEN FAILED: {r.stderr[-200:]}")
        break
    print(f"  regenerated")

    # Build v2
    r = run([MSBUILD, SLN, "-p:Configuration=Release", "-p:Platform=x64",
             "-verbosity:minimal"])
    if r.returncode != 0:
        print(f"  BUILD FAILED: {r.stdout[-300:]}")
        break
    print(f"  built")

else:
    print(f"\nDid not converge after 20 rounds!")

# Summary
r = run(["grep", "-c", "extra_func", GAME_CFG])
print(f"\nTotal extra_func entries in game.cfg: {r.stdout.strip()}")
