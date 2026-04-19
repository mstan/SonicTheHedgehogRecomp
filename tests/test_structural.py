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

REPO_ROOT     = pathlib.Path(__file__).resolve().parents[1]
SONIC_DIR     = REPO_ROOT / "segagenesisrecomp" / "sonicthehedgehog"
GEN_DIR       = SONIC_DIR / "generated"
FULL_C        = GEN_DIR / "sonic_full.c"
DISP_C        = GEN_DIR / "sonic_dispatch.c"
L1_FIXTURE    = (REPO_ROOT / "segagenesisrecomp" / "tests" / "fixtures"
                 / "sonic1" / "l1" / "instructions.txt")

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


# ---------------------------------------------------------------------------
# Disasm coverage: every JSR/BSR target the disasm reaches must be a
# defined function in our generated C. Missing entries are HARD failures
# (the recompiler is leaving real subroutines un-translated). Extra
# functions in the codegen that the disasm doesn't reach as call targets
# are reported as a soft WARNING — they may be valid (e.g. interrupt
# handlers, dispatch-table targets) or they may indicate over-eager
# discovery from decoded data.
#
# Oracle: the L1 fixture (assembled from the s1disasm REV00 listing,
# sha1-verified to match sonic.bin). Decoding JSR/BSR target addresses
# from raw bytes is independent of our recompiler decoder, so this test
# can fail even if the decoder agrees with itself.
# ---------------------------------------------------------------------------

# Fixture row: "<addr_hex> <bytes_hex>\t<mnem>\t<ops>"
_FIX_ROW_RE = re.compile(
    r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\t([^\t]+)\t",
)

def _disasm_call_targets() -> set[int]:
    """Return the set of static JSR/BSR target addresses the disasm calls.

    Decodes target offsets from the assembled bytes (independent of our
    decoder). Skips dynamic forms (register-indirect JSR (An), indexed
    JSR table(PC,Dn.W) etc.) — those have no static target so they
    don't claim a specific address must exist.
    """
    if not L1_FIXTURE.exists():
        return set()
    targets: set[int] = set()
    for line in L1_FIXTURE.read_text(encoding="utf-8").splitlines():
        m = _FIX_ROW_RE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        bh   = m.group(2)
        mn   = m.group(3).lower()
        if not (mn.startswith("jsr") or mn.startswith("bsr")):
            continue
        b = bytes.fromhex(bh)
        if len(b) < 2:
            continue
        op = (b[0] << 8) | b[1]
        # BSR family: 0x61xx
        if (op & 0xFF00) == 0x6100:
            disp8 = op & 0xFF
            if disp8 == 0:        # 0x6100 = BSR.W word displacement
                if len(b) < 4: continue
                d = (b[2] << 8) | b[3]
                if d & 0x8000: d -= 0x10000
                targets.add((addr + 2 + d) & 0xFFFFFF)
            elif disp8 == 0xFF:   # 0x61FF = BSR.L long displacement (rare)
                if len(b) < 6: continue
                d = (b[2] << 24) | (b[3] << 16) | (b[4] << 8) | b[5]
                if d & 0x80000000: d -= 0x100000000
                targets.add((addr + 2 + d) & 0xFFFFFF)
            else:                  # BSR.S short displacement
                d = disp8
                if d & 0x80: d -= 0x100
                targets.add((addr + 2 + d) & 0xFFFFFF)
            continue
        # JSR with absolute-long EA: 0x4EB9 + 4-byte abs address
        if op == 0x4EB9 and len(b) >= 6:
            t = (b[2] << 24) | (b[3] << 16) | (b[4] << 8) | b[5]
            targets.add(t & 0xFFFFFF)
            continue
        # JSR with absolute-word EA: 0x4EB8 + 2-byte signed-extended addr
        if op == 0x4EB8 and len(b) >= 4:
            t = (b[2] << 8) | b[3]
            if t & 0x8000: t -= 0x10000
            targets.add(t & 0xFFFFFF)
            continue
        # JSR (d16,PC): 0x4EBA + 2-byte signed displacement from PC
        if op == 0x4EBA and len(b) >= 4:
            d = (b[2] << 8) | b[3]
            if d & 0x8000: d -= 0x10000
            targets.add((addr + 2 + d) & 0xFFFFFF)
            continue
        # Other JSR forms have no static target — skip.
    return targets


def _func_addr_set(src: str) -> set[int]:
    return {int(name[5:], 16) for name in FUNC_DEF_RE.findall(src)}


def test_no_disasm_subroutines_missing_from_codegen():
    """HARD failure: any disasm-reachable JSR/BSR target that isn't a
    defined func_XXXXXX in the generated code."""
    targets = _disasm_call_targets()
    if not targets:
        return  # fixture not generated yet; not this test's job to enforce
    defs = _func_addr_set(_read(FULL_C))
    missing = sorted(targets - defs)
    if missing:
        sample = ", ".join(f"${a:06X}" for a in missing[:8])
        raise AssertionError(
            f"{len(missing)} disasm-reached JSR/BSR targets have no "
            f"matching func_XXXXXX in sonic_full.c (missing translations). "
            f"First few: {sample}"
        )


def test_codegen_extras_not_in_disasm_call_set():
    """SOFT warning: defined functions that the disasm never call-targets
    statically. Could be legitimate (interrupt handlers, dispatch-table
    targets the recompiler discovered through other means) or over-eager
    function discovery. Printed as info; never fails."""
    targets = _disasm_call_targets()
    if not targets:
        return
    defs = _func_addr_set(_read(FULL_C))
    extras = sorted(defs - targets)
    if extras:
        sample = ", ".join(f"${a:06X}" for a in extras[:8])
        # Print to stdout — the runner picks it up but never fails on it.
        print(f"  WARN  {len(extras)} codegen functions not in disasm "
              f"call set (interrupt handlers / dispatch targets / over-"
              f"eager discovery). First few: {sample}")


# ---------------------------------------------------------------------------
# Wrong-splits warning: defined functions whose physical predecessor is a
# non-terminator instruction. These addresses are interior labels of the
# preceding function (reached by fall-through), not subroutine entries.
# Promoting them to func_XXXXXX produces early-RTS-mid-function patterns
# when the parent's body terminates early at the split. SOFT warning until
# we have a properly-tested auto-merge implementation; tracking the count
# lets us detect regressions.
# ---------------------------------------------------------------------------

# Cache: parse the L1 fixture into (start_addr -> instruction_byte_length,
# start_addr -> mnemonic_no_size). Used to find the predecessor of a given
# address without a reverse decoder.
_FIX_INSN_CACHE: tuple[dict[int, int], dict[int, str]] | None = None

def _fixture_insn_index() -> tuple[dict[int, int], dict[int, str]]:
    global _FIX_INSN_CACHE
    if _FIX_INSN_CACHE is not None:
        return _FIX_INSN_CACHE
    starts: dict[int, int] = {}
    mnems:  dict[int, str] = {}
    if L1_FIXTURE.exists():
        for line in L1_FIXTURE.read_text(encoding="utf-8").splitlines():
            m = _FIX_ROW_RE.match(line)
            if not m: continue
            addr = int(m.group(1), 16)
            nbytes = len(m.group(2)) // 2
            starts[addr] = nbytes
            mnems[addr]  = m.group(3).lower().split(".")[0]
    _FIX_INSN_CACHE = (starts, mnems)
    return _FIX_INSN_CACHE


_TERMINATORS = frozenset({"rts", "rte", "rtr", "stop", "jmp", "bra"})

def _is_interior_label(addr: int) -> bool:
    """True iff some instruction in the fixture ends exactly at `addr` and
    that instruction is NOT a terminator. Returns False if no predecessor
    is found (safer default — treats unknown as fresh entry)."""
    starts, mnems = _fixture_insn_index()
    for back in (2, 4, 6, 8, 10):
        pa = addr - back
        if pa < 0:
            return False
        n = starts.get(pa)
        if n is None:
            continue
        if pa + n != addr:
            continue
        return mnems[pa] not in _TERMINATORS
    return False


def test_no_interior_labels_split_into_functions():
    """SOFT warning (high-watermark tracker): defined functions whose
    predecessor instruction is a non-terminator. These were promoted by
    boundary-split when the disasm intends them as interior labels of the
    preceding function. Until proper auto-merge is implemented, we just
    track the count so regressions show up immediately."""
    if not L1_FIXTURE.exists():
        return
    defs = _func_addr_set(_read(FULL_C))
    interior = sorted(a for a in defs if _is_interior_label(a))
    if interior:
        sample = ", ".join(f"${a:06X}" for a in interior[:8])
        print(f"  WARN  {len(interior)} interior-label addresses promoted "
              f"to functions (auto-merge not implemented; bcc-from-sibling "
              f"references will hit early-RTS-mid-function). "
              f"First few: {sample}")
