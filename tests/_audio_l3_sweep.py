"""Run L3 on every codegen function whose address is in the sound driver
region (0x71000-0x72FFF). Reports the per-function result so we can spot
divergences likely linked to the audio glitches. Skips tier-10 functions
(dynamic-dispatch — their stubs short-circuit so any divergence is
harness artifact, not a real bug)."""
import re, pathlib, subprocess
SUB  = pathlib.Path(__file__).resolve().parents[1] / "segagenesisrecomp"
EXE  = pathlib.Path(__file__).resolve().parents[1] / "build" / "tests" / "Release" / "l3_oracle_test.exe"
FULL = (SUB / "sonicthehedgehog/generated/sonic_full.c").read_text(encoding="utf-8")
MAN  = (SUB / "tests/fixtures/sonic1/l3/manifest.txt").read_text(encoding="utf-8")

# Parse manifest into addr -> tier so we can skip tier 10.
addr_tier: dict[int, int] = {}
cur_tier = -1
for line in MAN.splitlines():
    m = re.match(r"^# tier (\d+)", line)
    if m: cur_tier = int(m.group(1)); continue
    if line.strip() and not line.startswith("#"):
        try: addr_tier[int(line.strip(), 16)] = cur_tier
        except ValueError: pass

DEF = re.compile(r"^void (func_([0-9A-Fa-f]+))\(void\)\s*\{", re.MULTILINE)
audio_all = sorted({int(m.group(2), 16) for m in DEF.finditer(FULL)
                    if 0x71000 <= int(m.group(2), 16) <= 0x72FFF})
audio = [a for a in audio_all if addr_tier.get(a, 99) != 10]
print(f"{len(audio_all)} functions in sound driver range, "
      f"{len(audio)} after excluding tier 10")

# Run each via --filter and count results.
pass_n = 0
fail_n = 0
fails: list[tuple[int, str]] = []
for addr in audio:
    res = subprocess.run(
        [str(EXE), "--filter", f"{addr:06X}", "-v"],
        cwd=str(EXE.parents[3]), capture_output=True, text=True,
    )
    out = res.stdout + res.stderr
    if "1/1 functions pass" in out:
        pass_n += 1
        continue
    fail_n += 1
    # Extract failure summary.
    m = re.search(rf"FAIL func_{addr:06X} seed=\S+ tier=\d+:\s*(.*)", out)
    summary = m.group(1).strip() if m else "(no FAIL line)"
    fails.append((addr, summary))

print(f"\nAudio range: {pass_n}/{len(audio)} pass, {fail_n} fail")
print()
for addr, kind in fails[:25]:
    print(f"  ${addr:06X}  {kind}")
if len(fails) > 25:
    print(f"  ... +{len(fails) - 25} more")
