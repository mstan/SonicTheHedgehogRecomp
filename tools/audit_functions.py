#!/usr/bin/env python
"""
Audit: compare disassembly-named functions (sonic1.syms.toml) against
what the recompiler actually generated (sonic_full.c / sonic_dispatch.c).

Emits:
  - missing-from-dispatch.txt: syms entries NOT in the dispatch table
  - missing-from-generated.txt: syms entries NOT defined in sonic_full.c
  - present-and-matched.txt: syms entries fully present (for sanity)
  - audio-related-missing.txt: subset of missing that look SMPS/audio-related

Heuristic for "audio-related": name or comment matches
  SMPS | Sound | Music | FM | PSG | DAC | Voice | Sample | Tempo | Fade | Note | Tick
  Play | Update | Coord | Driver | SFX | Queue
"""
import re
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent / "segagenesisrecomp" / "sonicthehedgehog"
SYMS = ROOT / "sonic1.syms.toml"
GEN_FULL = ROOT / "generated" / "sonic_full.c"
GEN_DISPATCH = ROOT / "generated" / "sonic_dispatch.c"

AUDIO_PAT = re.compile(
    r"SMPS|Sound|Music|FM|PSG|DAC|Voice|Sample|Tempo|Fade|Note|Tick|"
    r"Play|Update[A-Z]|Coord|Driver|SFX|Queue",
    re.IGNORECASE,
)

def parse_syms():
    """Returns list of (addr, name, section, blacklisted)."""
    out = []
    section = None
    blacklist_re = re.compile(r"^\s*# BLACKLISTED:\s*\{\s*name\s*=\s*\"([^\"]+)\"\s*,\s*addr\s*=\s*(0x[0-9A-Fa-f]+)\s*\}")
    entry_re    = re.compile(r"^\s*\{\s*name\s*=\s*\"([^\"]+)\"\s*,\s*addr\s*=\s*(0x[0-9A-Fa-f]+)\s*\}")
    name_re     = re.compile(r"^\s*name\s*=\s*\"([^\"]+)\"")
    for line in SYMS.read_text(encoding="utf-8").splitlines():
        if line.startswith("[[section]]"):
            section = None
            continue
        m_sec = name_re.match(line)
        if m_sec and section is None:
            section = m_sec.group(1)
            continue
        m_bl = blacklist_re.match(line)
        if m_bl:
            out.append((int(m_bl.group(2), 16), m_bl.group(1), section, True))
            continue
        m_e = entry_re.match(line)
        if m_e:
            out.append((int(m_e.group(2), 16), m_e.group(1), section, False))
    return out

def scan_generated():
    """Returns set of addrs that have a `void func_XXXXXX(void) {` body."""
    defined = set()
    # Skip forward decls (void func_XXXXXX(void);) — only count definitions.
    defn_re = re.compile(r"^void func_([0-9A-F]{6})\(void\)\s*\{")
    for line in GEN_FULL.read_text(encoding="utf-8").splitlines():
        m = defn_re.match(line)
        if m:
            defined.add(int(m.group(1), 16))
    return defined

def scan_dispatch():
    """Returns set of addrs in the dispatch table."""
    dispatched = set()
    addr_re = re.compile(r"0x([0-9A-Fa-f]{6,8})")
    in_table = False
    for line in GEN_DISPATCH.read_text(encoding="utf-8").splitlines():
        # Crude: anything that looks like a hex addr in this file.
        for m in addr_re.finditer(line):
            v = int(m.group(1), 16)
            if 0x200 <= v <= 0x400000:  # plausible code range
                dispatched.add(v)
    return dispatched

def main():
    syms = parse_syms()
    defined = scan_generated()
    dispatched = scan_dispatch()

    print(f"Syms entries:        {len(syms)}")
    print(f"  blacklisted:       {sum(1 for s in syms if s[3])}")
    print(f"  active:            {sum(1 for s in syms if not s[3])}")
    print(f"Generated funcs:     {len(defined)}")
    print(f"Dispatch entries:    {len(dispatched)} (includes call sites)")

    active = [s for s in syms if not s[3]]
    missing_gen = [s for s in active if s[0] not in defined]
    missing_dis = [s for s in active if s[0] not in dispatched]

    print(f"\nActive syms missing from generated: {len(missing_gen)}")
    print(f"Active syms missing from dispatch:  {len(missing_dis)}")

    # Write outputs
    out_dir = pathlib.Path(__file__).resolve().parent.parent / "build" / "Release"
    out_dir.mkdir(parents=True, exist_ok=True)

    with (out_dir / "audit-missing-from-generated.txt").open("w", encoding="utf-8") as f:
        f.write("# addr,name,section\n")
        for a, n, sec, _ in missing_gen:
            f.write(f"{a:06X},{n},{sec}\n")

    with (out_dir / "audit-missing-from-dispatch.txt").open("w", encoding="utf-8") as f:
        f.write("# addr,name,section\n")
        for a, n, sec, _ in missing_dis:
            f.write(f"{a:06X},{n},{sec}\n")

    audio_missing = [s for s in missing_gen if AUDIO_PAT.search(s[1])]
    with (out_dir / "audit-audio-missing.txt").open("w", encoding="utf-8") as f:
        f.write("# Audio-pattern functions missing from generated output\n")
        f.write("# addr,name,section\n")
        for a, n, sec, _ in audio_missing:
            f.write(f"{a:06X},{n},{sec}\n")

    print(f"\nAudio-pattern entries missing from generated: {len(audio_missing)}")
    if audio_missing:
        print("First 20:")
        for a, n, sec, _ in audio_missing[:20]:
            print(f"  {a:06X}  {n}  [{sec}]")

if __name__ == "__main__":
    main()
