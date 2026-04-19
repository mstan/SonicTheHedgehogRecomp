#!/usr/bin/env python3
"""
Unified test runner for SonicTheHedgehogRecomp.

Runs:
  L1 — 68K decoder correctness: compiled C harness (l1_decoder_test.exe)
       reading tests/fixtures/sonic1/l1/instructions.txt + sonic.bin,
       comparing against the m68k_decoder output per instruction.
  L2 — Structural invariants on generated C: Python assertions on
       sonic_full.c / sonic_dispatch.c (see test_*.py in this dir).

Exit 0 on full green, 1 if anything fails.
"""
from __future__ import annotations

import importlib
import pathlib
import subprocess
import sys
import traceback

TESTS_DIR  = pathlib.Path(__file__).resolve().parent
REPO_ROOT  = TESTS_DIR.parent
BUILD_DIR  = REPO_ROOT / "build"

L1_EXE     = BUILD_DIR / "tests" / "Release" / "l1_decoder_test.exe"
L1_FIXTURE = (REPO_ROOT / "segagenesisrecomp" / "tests" / "fixtures"
              / "sonic1" / "l1" / "instructions.txt")
ROM_PATH   = BUILD_DIR / "Release" / "sonic.bin"
L1_LOG_DIR = BUILD_DIR / "tests" / "last_run"

L2_MODULES = [
    "test_structural",
]


def run_l1() -> int:
    if not L1_EXE.exists():
        print("L1: harness not built — run cmake --build build --config Release "
              "--target l1_decoder_test")
        return 1
    if not L1_FIXTURE.exists():
        print(f"L1: fixture missing — run python "
              f"segagenesisrecomp/tests/tools/gen_l1_fixtures.py")
        return 1
    if not ROM_PATH.exists():
        print(f"L1: ROM missing at {ROM_PATH}")
        return 1
    L1_LOG_DIR.mkdir(parents=True, exist_ok=True)
    rc = subprocess.call(
        [str(L1_EXE),
         "--fixture", str(L1_FIXTURE),
         "--rom",     str(ROM_PATH),
         "--log-dir", str(L1_LOG_DIR)],
        cwd=str(REPO_ROOT),
    )
    return rc


def run_l2() -> int:
    sys.path.insert(0, str(TESTS_DIR))
    passed = 0
    failed = 0
    failures: list[tuple[str, str]] = []
    for modname in L2_MODULES:
        mod = importlib.import_module(modname)
        tests = [(n, getattr(mod, n)) for n in dir(mod) if n.startswith("test_")]
        for name, fn in tests:
            label = f"{modname}.{name}"
            try:
                fn()
                print(f"  PASS  {label}")
                passed += 1
            except AssertionError as e:
                print(f"  FAIL  {label}: {e}")
                failures.append((label, str(e)))
                failed += 1
            except Exception:
                print(f"  ERR   {label}")
                failures.append((label, traceback.format_exc()))
                failed += 1
    print()
    print(f"L2: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


def main() -> int:
    print("=== L1 (decoder correctness) ===")
    rc1 = run_l1()
    print()
    print("=== L2 (structural invariants) ===")
    rc2 = run_l2()
    print()
    if rc1 == 0 and rc2 == 0:
        print("ALL TESTS PASSED")
        return 0
    print(f"FAILED — L1 rc={rc1}, L2 rc={rc2}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
