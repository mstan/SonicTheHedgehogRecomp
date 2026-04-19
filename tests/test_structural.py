"""
L2 structural invariants on generated sonic_full.c / sonic_dispatch.c.

These tests catch codegen bugs that would otherwise only show up at link
time ("unresolved external symbol func_XXXXXX") or at runtime ("dispatch
miss at $XXXXXX"). Running them after each regenerate keeps regressions
from compiling.

Each test asserts on current generated output. No ROM, no execution.
"""
from __future__ import annotations

import pathlib
import re

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
GEN_DIR   = REPO_ROOT / "segagenesisrecomp" / "sonicthehedgehog" / "generated"
FULL_C    = GEN_DIR / "sonic_full.c"
DISP_C    = GEN_DIR / "sonic_dispatch.c"

FUNC_DEF_RE  = re.compile(r"^void (func_[0-9A-Fa-f]+)\(void\)\s*\{", re.MULTILINE)
FUNC_DECL_RE = re.compile(r"^void (func_[0-9A-Fa-f]+)\(void\)\s*;",   re.MULTILINE)
# Dispatch entry line: "    { 0x000200u, func_000200 },"
DISP_ROW_RE  = re.compile(
    r"\{\s*0x([0-9A-Fa-f]+)u\s*,\s*(func_[0-9A-Fa-f]+)\s*\}",
)
# Direct call inside a function body.
CALL_SITE_RE = re.compile(r"\b(func_[0-9A-Fa-f]+)\s*\(\s*\)")
# call_by_address(0xXXXXXX)
DYN_CALL_RE  = re.compile(r"call_by_address\s*\(\s*0x([0-9A-Fa-f]+)u?\s*\)")


def _read(path: pathlib.Path) -> str:
    assert path.exists(), f"missing generated file: {path}"
    return path.read_text(encoding="utf-8", errors="replace")


# ---------------------------------------------------------------------------
# Basic integrity: every decl has a definition and vice-versa.
# ---------------------------------------------------------------------------

def test_every_declaration_has_definition():
    src = _read(FULL_C)
    decls = set(FUNC_DECL_RE.findall(src))
    defs  = set(FUNC_DEF_RE.findall(src))
    missing = decls - defs
    assert not missing, (
        f"{len(missing)} functions declared without a definition in sonic_full.c — "
        f"first few: {sorted(missing)[:5]}"
    )


def test_every_definition_has_declaration():
    src = _read(FULL_C)
    decls = set(FUNC_DECL_RE.findall(src))
    defs  = set(FUNC_DEF_RE.findall(src))
    orphan = defs - decls
    assert not orphan, (
        f"{len(orphan)} function definitions without forward declarations — "
        f"first few: {sorted(orphan)[:5]}"
    )


def test_no_duplicate_function_definitions():
    src = _read(FULL_C)
    names = FUNC_DEF_RE.findall(src)
    dups = sorted({n for n in names if names.count(n) > 1})
    assert not dups, f"duplicate definitions: {dups[:10]}"


# ---------------------------------------------------------------------------
# Dispatch table invariants.
# ---------------------------------------------------------------------------

def test_dispatch_table_matches_func_set():
    full = _read(FULL_C)
    disp = _read(DISP_C)
    defs = set(FUNC_DEF_RE.findall(full))
    entries = DISP_ROW_RE.findall(disp)
    entry_funcs = {name for _, name in entries}
    missing_in_full = entry_funcs - defs
    assert not missing_in_full, (
        f"{len(missing_in_full)} dispatch entries reference functions not defined "
        f"in sonic_full.c (link would fail): {sorted(missing_in_full)[:5]}"
    )


def test_dispatch_table_sorted_by_address():
    disp = _read(DISP_C)
    entries = DISP_ROW_RE.findall(disp)
    addrs = [int(a, 16) for a, _ in entries]
    assert addrs == sorted(addrs), (
        "dispatch table is not sorted ascending by address — "
        "breaks future binary-search dispatch."
    )


def test_dispatch_table_no_duplicate_addresses():
    disp = _read(DISP_C)
    entries = DISP_ROW_RE.findall(disp)
    addrs = [a for a, _ in entries]
    seen: dict[str, int] = {}
    dups = []
    for a in addrs:
        seen[a] = seen.get(a, 0) + 1
        if seen[a] == 2:
            dups.append(a)
    assert not dups, f"{len(dups)} duplicate addresses in dispatch table: {dups[:5]}"


def test_dispatch_entry_names_match_addresses():
    disp = _read(DISP_C)
    mismatched = []
    for a, name in DISP_ROW_RE.findall(disp):
        # Expect name "func_" + uppercase(hex(addr).upper().zfill(6))
        want = f"func_{int(a, 16):06X}"
        if name != want:
            mismatched.append((a, name, want))
    assert not mismatched, (
        f"{len(mismatched)} dispatch entries whose function name doesn't match "
        f"their address (sign of stale regen): {mismatched[:3]}"
    )


def test_dispatch_table_size_accessor_matches_table():
    disp = _read(DISP_C)
    entries = DISP_ROW_RE.findall(disp)
    m = re.search(r"int game_dispatch_table_size\(void\)\s*\{\s*return\s+(\d+)\s*;\s*\}", disp)
    assert m, "game_dispatch_table_size() accessor not found in sonic_dispatch.c"
    claimed = int(m.group(1))
    assert claimed == len(entries), (
        f"game_dispatch_table_size() returns {claimed} but the table has "
        f"{len(entries)} entries — binary-search / interior-label check "
        f"will read past the end or short-circuit early."
    )


# ---------------------------------------------------------------------------
# Call-site sanity: every direct call resolves to a defined function.
# ---------------------------------------------------------------------------

def test_every_direct_call_resolves():
    full = _read(FULL_C)
    defs = set(FUNC_DEF_RE.findall(full))
    calls = set(CALL_SITE_RE.findall(full))
    unresolved = calls - defs
    assert not unresolved, (
        f"{len(unresolved)} direct func_XXXX() calls in sonic_full.c have no "
        f"definition (link would fail): {sorted(unresolved)[:5]}"
    )
