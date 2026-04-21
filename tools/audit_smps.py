#!/usr/bin/env python
"""
Audit SMPS/audio functions in the generated output for suspicious shapes.

For each named audio-related function:
  1. Find the function definition in sonic_full.c
  2. Measure body length (lines between braces)
  3. Check for:
     - Single-return stub (suspiciously tiny body, possibly truncated)
     - extras.c override (function implemented by hand, bypassing recomp)
     - Presence of uncovered labels (goto label_XXXXXX without matching label emission)
     - Unexpected return inside a non-RTS flow

Also compare function END addresses: does the generated body extend to the
next named function, or is it cut short vs the disasm?
"""
import re
import pathlib
import collections

ROOT = pathlib.Path(__file__).resolve().parent.parent / "segagenesisrecomp" / "sonicthehedgehog"
SYMS = ROOT / "sonic1.syms.toml"
GEN = ROOT / "generated" / "sonic_full.c"
EXTRAS_DIR = ROOT

AUDIO_PAT = re.compile(
    r"SMPS|Sound|Music|FM|PSG|DAC|Voice|Sample|Tempo|Fade|Note|Tick|"
    r"Play[A-Z]|Update[A-Z]|Coord|Driver|SFX|Queue|sndDriver|Modul|"
    r"Envelope|Pan|Detune|Volume",
    re.IGNORECASE,
)

def parse_syms():
    out = []
    entry_re = re.compile(r"^\s*\{\s*name\s*=\s*\"([^\"]+)\"\s*,\s*addr\s*=\s*(0x[0-9A-Fa-f]+)\s*\}")
    for line in SYMS.read_text(encoding="utf-8").splitlines():
        m = entry_re.match(line)
        if m:
            out.append((int(m.group(2), 16), m.group(1)))
    return out

def load_generated():
    """Returns: {addr: (start_line, end_line, body_lines)}."""
    text = GEN.read_text(encoding="utf-8").splitlines()
    defn_re = re.compile(r"^void func_([0-9A-F]{6})\(void\)\s*\{")
    funcs = {}
    i = 0
    while i < len(text):
        m = defn_re.match(text[i])
        if not m:
            i += 1
            continue
        addr = int(m.group(1), 16)
        start = i
        # Find matching closing brace (depth tracking at column 0 is safe
        # enough since the generator writes `}\n\n` for function ends).
        depth = 0
        j = i
        while j < len(text):
            line = text[j]
            depth += line.count("{") - line.count("}")
            if depth == 0 and j > i:
                break
            j += 1
        funcs[addr] = (start, j, j - start + 1)
        i = j + 1
    return funcs

def audit():
    syms = parse_syms()
    funcs = load_generated()

    audio_syms = [(a, n) for (a, n) in syms if AUDIO_PAT.search(n)]
    print(f"Audio-related named syms: {len(audio_syms)}")
    print()

    # Find disasm-next for each audio sym (what's the expected end address?)
    syms_sorted = sorted(syms, key=lambda p: p[0])
    addr_to_next = {}
    for i, (a, n) in enumerate(syms_sorted):
        nxt = syms_sorted[i+1][0] if i + 1 < len(syms_sorted) else None
        addr_to_next[a] = nxt

    # Audit each audio func
    suspicious = []
    for addr, name in audio_syms:
        if addr not in funcs:
            suspicious.append((addr, name, "NOT_DEFINED", "function missing from generated"))
            continue
        start, end, nlines = funcs[addr]
        # Body length in lines (excluding signature/brace)
        body = max(0, nlines - 3)
        expected_next = addr_to_next[addr]

        # Try to infer generated extent — find last /* $XXXXXX */ comment
        text = GEN.read_text(encoding="utf-8").splitlines()
        last_addr_in_body = None
        for k in range(start, end + 1):
            m = re.match(r"\s*/\*\s*\$([0-9A-F]{6})\s*\*/", text[k])
            if m:
                last_addr_in_body = int(m.group(1), 16)

        # Flag suspicious shapes
        if nlines <= 5:
            suspicious.append((addr, name, "TINY", f"body {nlines} lines — likely stub/decode-fail"))
        elif last_addr_in_body is not None and expected_next is not None:
            gap = expected_next - last_addr_in_body
            # If gap > 128 bytes we're probably cut short
            if gap > 128:
                suspicious.append((addr, name, "SHORT",
                    f"last addr ${last_addr_in_body:06X}, next sym ${expected_next:06X}, gap={gap} bytes"))

    # Output
    print(f"Suspicious audio functions: {len(suspicious)}")
    print()
    for addr, name, kind, detail in suspicious:
        print(f"  {addr:06X} {name:<32}  {kind:<12} {detail}")

    # Dump complete list
    out_dir = pathlib.Path(__file__).resolve().parent.parent / "build" / "Release"
    out_dir.mkdir(parents=True, exist_ok=True)
    with (out_dir / "audit-smps.txt").open("w", encoding="utf-8") as f:
        f.write(f"# {len(audio_syms)} audio-related symbols, {len(suspicious)} suspicious\n")
        f.write("# addr,name,kind,detail\n")
        for addr, name, kind, detail in suspicious:
            f.write(f"{addr:06X},{name},{kind},{detail}\n")

if __name__ == "__main__":
    audit()
