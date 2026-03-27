#!/usr/bin/env python3
"""
extract_symbols.py — Extract function symbols from s1disasm + annotations CSV.

Parses the s1disasm 68K assembly source to find subroutine labels
(marked with "; S U B R O U T I N E" banners), then cross-references
with annotations_from_disasm.csv to resolve ROM addresses.

Outputs a TOML symbols file compatible with the recompiler.
"""
import os
import re
import sys
from pathlib import Path
from collections import OrderedDict

SCRIPT_DIR = Path(__file__).parent.resolve()
PROJECT_ROOT = SCRIPT_DIR.parent  # SonicTheHedgehogRecomp/
RECOMP_ROOT = PROJECT_ROOT / "segagenesisrecomp"
DISASM_DIR = PROJECT_ROOT.parent / "_s1disasm" if (PROJECT_ROOT.parent / "_s1disasm").exists() else None

# Protected ranges: disasm-discovered labels in these ranges are excluded
# from subroutines/table_targets sections (only extra_seeds from game.cfg
# are kept). Aggressive boundary splitting in the sound driver breaks audio.
PROTECTED_RANGES = [
    (0x071000, 0x072FFF, "sound driver"),
]

SONIC_DIR_SUB = RECOMP_ROOT / "sonicthehedgehog"          # via submodule
SONIC_DIR_TOP = PROJECT_ROOT.parent / "segagenesisrecomp" / "sonicthehedgehog"  # top-level

# Prefer submodule paths, fall back to top-level
def _find(filename):
    if (SONIC_DIR_SUB / filename).exists():
        return SONIC_DIR_SUB / filename
    if (SONIC_DIR_TOP / filename).exists():
        return SONIC_DIR_TOP / filename
    return SONIC_DIR_SUB / filename  # default even if missing

ANNOTATIONS_CSV = _find("annotations_from_disasm.csv")
BLACKLIST_FILE = _find("blacklist.txt")
GAME_CFG = _find("game.cfg")
OUTPUT_TOML = SONIC_DIR_SUB / "sonic1.syms.toml"


def load_annotations(path):
    """Load annotations CSV → {label_name: hex_addr_int, ...}"""
    name_to_addr = {}
    addr_to_names = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",", 2)
            if len(parts) < 2:
                continue
            addr_str = parts[0].strip()
            # May have multiple names separated by " / "
            names_field = parts[1].strip()
            try:
                addr = int(addr_str, 16)
            except ValueError:
                continue
            # Collect names from ALL columns (col1=primary, col2=alias/notes)
            all_names_raw = names_field
            if len(parts) >= 3 and parts[2].strip():
                all_names_raw += " / " + parts[2].strip()
            if not all_names_raw.strip():
                continue
            # Split on " / " for aliases
            for name in all_names_raw.split(" / "):
                name = name.strip()
                if name:
                    name_to_addr[name] = addr
                    addr_to_names.setdefault(addr, []).append(name)
    return name_to_addr, addr_to_names


def load_blacklist(path):
    """Load blacklist addresses."""
    blacklist = set()
    if not path.exists():
        return blacklist
    with open(path) as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            line = line.upper().replace("0X", "")
            try:
                blacklist.add(int(line, 16))
            except ValueError:
                pass
    return blacklist


def load_existing_extra_funcs(path):
    """Load existing extra_func entries from game.cfg."""
    funcs = set()
    if not path.exists():
        return funcs
    with open(path) as f:
        for line in f:
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            if stripped.startswith("extra_func"):
                parts = stripped.split()
                if len(parts) >= 2:
                    addr_str = parts[1].split("#")[0].strip()
                    try:
                        addr = int(addr_str, 16)
                        if addr < 0x80000:  # Sonic 1 ROM is 512KB
                            funcs.add(addr)
                    except ValueError:
                        pass
    return funcs


def extract_subroutine_labels_from_disasm(disasm_dir):
    """
    Parse s1disasm ASM files to find labels marked with SUBROUTINE banners.
    Returns a set of label names that are confirmed subroutines.
    """
    subroutine_labels = set()
    banner_re = re.compile(r"S U B R O U T I N E")
    label_re = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*):")

    asm_files = []
    for ext in ("*.asm",):
        asm_files.extend(disasm_dir.rglob(ext))

    for asm_file in sorted(asm_files):
        lines = []
        try:
            with open(asm_file, encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
        except Exception:
            continue

        i = 0
        while i < len(lines):
            if banner_re.search(lines[i]):
                # Look ahead for the next label (skip blank/comment lines)
                for j in range(i + 1, min(i + 10, len(lines))):
                    m = label_re.match(lines[j])
                    if m:
                        subroutine_labels.add(m.group(1))
                        break
                    # If we hit actual code before a label, stop looking
                    stripped = lines[j].strip()
                    if stripped and not stripped.startswith(";") and not stripped.startswith("*"):
                        break
            i += 1

    return subroutine_labels


def extract_jump_table_targets_from_disasm(disasm_dir):
    """
    Find labels used as jump table entries — these are indirect call targets
    that won't be found by BSR/JSR static walk.

    Patterns like:
        dc.w LabelName-BaseLabel
        bra.w LabelName
    in table contexts.
    """
    # This is a lighter heuristic — we mainly rely on SUBROUTINE banners
    # but also catch common jump table patterns
    table_targets = set()

    # Look for ptr_* labels (used in game mode arrays, object dispatch)
    ptr_re = re.compile(r"^(ptr_[A-Za-z0-9_]+):")

    for asm_file in sorted(disasm_dir.rglob("*.asm")):
        try:
            with open(asm_file, encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = ptr_re.match(line)
                    if m:
                        table_targets.add(m.group(1))
        except Exception:
            continue

    return table_targets


def categorize_annotations(name_to_addr, subroutine_labels, table_targets):
    """
    Categorize annotation labels into sections based on address ranges
    and source classification.
    """
    sections = OrderedDict()
    sections["subroutines"] = {}       # SUBROUTINE-banner confirmed
    sections["table_targets"] = {}     # ptr_* / jump table dispatch
    sections["unlabeled"] = {}         # In annotations but not classified

    for name, addr in sorted(name_to_addr.items(), key=lambda x: x[1]):
        if addr >= 0x80000:  # Outside Sonic 1 ROM range
            continue
        if name in subroutine_labels:
            sections["subroutines"][addr] = name
        elif name in table_targets:
            sections["table_targets"][addr] = name

    return sections


def generate_toml(sections, existing_extra_funcs, blacklist, addr_to_names, output_path):
    """Generate the TOML symbols file."""
    all_addrs = set()
    lines = []
    lines.append("# sonic1.syms.toml — Function symbols for Sonic the Hedgehog (Genesis)")
    lines.append("# Auto-generated from s1disasm + annotations_from_disasm.csv")
    lines.append("# Source: https://github.com/sonicretro/s1disasm")
    lines.append("#")
    lines.append("# Sections:")
    lines.append("#   subroutines   — labels with S U B R O U T I N E banner in s1disasm")
    lines.append("#   table_targets — ptr_* labels used in jump/dispatch tables")
    lines.append("#   extra_seeds   — addresses from game.cfg not found in disasm labels")
    lines.append("")

    def in_protected_range(addr):
        for lo, hi, _desc in PROTECTED_RANGES:
            if lo <= addr <= hi:
                return True
        return False

    # Subroutines section
    lines.append("[[section]]")
    lines.append('name = "subroutines"')
    lines.append('source = "s1disasm SUBROUTINE banner"')
    lines.append("functions = [")
    sub_count = 0
    protected_count = 0
    for addr, name in sorted(sections["subroutines"].items()):
        if addr in blacklist:
            lines.append(f'    # BLACKLISTED: {{ name = "{name}", addr = 0x{addr:06X} }},')
            continue
        if in_protected_range(addr) and addr not in existing_extra_funcs:
            lines.append(f'    # PROTECTED (sound driver): {{ name = "{name}", addr = 0x{addr:06X} }},')
            protected_count += 1
            continue
        lines.append(f'    {{ name = "{name}", addr = 0x{addr:06X} }},')
        all_addrs.add(addr)
        sub_count += 1
    lines.append("]")
    lines.append("")

    # Table targets section
    lines.append("[[section]]")
    lines.append('name = "table_targets"')
    lines.append('source = "s1disasm ptr_* / dispatch table labels"')
    lines.append("functions = [")
    tbl_count = 0
    for addr, name in sorted(sections["table_targets"].items()):
        if addr in blacklist:
            lines.append(f'    # BLACKLISTED: {{ name = "{name}", addr = 0x{addr:06X} }},')
            continue
        if addr in all_addrs:
            continue  # already covered
        if in_protected_range(addr) and addr not in existing_extra_funcs:
            lines.append(f'    # PROTECTED (sound driver): {{ name = "{name}", addr = 0x{addr:06X} }},')
            protected_count += 1
            continue
        lines.append(f'    {{ name = "{name}", addr = 0x{addr:06X} }},')
        all_addrs.add(addr)
        tbl_count += 1
    lines.append("]")
    lines.append("")

    # Extra seeds — addresses from game.cfg that weren't in any disasm label
    extra_only = existing_extra_funcs - all_addrs - blacklist
    lines.append("[[section]]")
    lines.append('name = "extra_seeds"')
    lines.append('source = "game.cfg extra_func (runtime discovery / manual)"')
    lines.append("functions = [")
    extra_count = 0
    for addr in sorted(extra_only):
        # Try to find a name from annotations
        names = addr_to_names.get(addr, [])
        name = names[0] if names else f"func_{addr:06X}"
        lines.append(f'    {{ name = "{name}", addr = 0x{addr:06X} }},')
        extra_count += 1
    lines.append("]")
    lines.append("")

    with open(output_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
        f.write("\n")

    return sub_count, tbl_count, extra_count, len(all_addrs) + extra_count


def main():
    if not DISASM_DIR or not DISASM_DIR.exists():
        print(f"ERROR: s1disasm not found at {DISASM_DIR}")
        print("Clone it: git clone https://github.com/sonicretro/s1disasm _s1disasm")
        sys.exit(1)

    if not ANNOTATIONS_CSV.exists():
        print(f"ERROR: annotations not found at {ANNOTATIONS_CSV}")
        sys.exit(1)

    print(f"[1/5] Loading annotations from {ANNOTATIONS_CSV}...")
    name_to_addr, addr_to_names = load_annotations(ANNOTATIONS_CSV)
    print(f"       {len(name_to_addr)} labels loaded")

    print(f"[2/5] Extracting SUBROUTINE labels from {DISASM_DIR}...")
    subroutine_labels = extract_subroutine_labels_from_disasm(DISASM_DIR)
    print(f"       {len(subroutine_labels)} subroutine labels found")

    print(f"[3/5] Extracting jump table targets...")
    table_targets = extract_jump_table_targets_from_disasm(DISASM_DIR)
    print(f"       {len(table_targets)} table target labels found")

    print(f"[4/5] Loading existing game.cfg extra_funcs and blacklist...")
    existing = load_existing_extra_funcs(GAME_CFG)
    blacklist = load_blacklist(BLACKLIST_FILE)
    print(f"       {len(existing)} existing extra_funcs, {len(blacklist)} blacklisted")

    # Cross-reference: find subroutine labels that have known addresses
    resolved = 0
    unresolved = []
    for label in sorted(subroutine_labels):
        if label in name_to_addr:
            resolved += 1
        else:
            unresolved.append(label)

    print(f"       {resolved}/{len(subroutine_labels)} subroutine labels resolved to addresses")
    if unresolved:
        print(f"       {len(unresolved)} unresolved: {', '.join(unresolved[:20])}")
        if len(unresolved) > 20:
            print(f"       ... and {len(unresolved) - 20} more")

    sections = categorize_annotations(name_to_addr, subroutine_labels, table_targets)

    print(f"[5/5] Generating {OUTPUT_TOML}...")
    sub_c, tbl_c, ext_c, total = generate_toml(
        sections, existing, blacklist, addr_to_names, OUTPUT_TOML
    )
    print(f"\n=== Summary ===")
    print(f"  Subroutines (disasm):   {sub_c}")
    print(f"  Table targets (disasm): {tbl_c}")
    print(f"  Extra seeds (game.cfg): {ext_c}")
    print(f"  Total functions:        {total}")
    print(f"  Blacklisted (skipped):  {len(blacklist)}")
    print(f"\nOutput: {OUTPUT_TOML}")


if __name__ == "__main__":
    main()
