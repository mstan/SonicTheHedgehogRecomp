#!/usr/bin/env python3
"""
Ingest dispatch_misses.log into game.cfg as extra_func entries.

Usage: python tools/ingest_misses.py [--misses PATH] [--cfg PATH]

Reads dispatch_misses.log (generated at runtime when call_by_address fails),
extracts unique addresses, and appends any NEW ones to game.cfg as extra_func
entries. Existing entries are not duplicated.

After ingesting, run the recompiler to regenerate code with the new functions:
  GenesisRecomp.exe sonic.bin --game game.cfg
"""

import argparse
import re
import os
import sys

def main():
    p = argparse.ArgumentParser(description="Merge dispatch misses into game.cfg")
    p.add_argument("--misses", default="dispatch_misses.log",
                   help="Path to dispatch_misses.log (default: dispatch_misses.log)")
    p.add_argument("--cfg", default=None,
                   help="Path to game.cfg (default: auto-detect)")
    args = p.parse_args()

    # Auto-detect game.cfg
    cfg_path = args.cfg
    if cfg_path is None:
        candidates = [
            os.path.join(os.path.dirname(__file__), "..", "..", "..",
                         "segagenesisrecomp", "sonicthehedgehog", "game.cfg"),
            "game.cfg",
        ]
        for c in candidates:
            if os.path.exists(c):
                cfg_path = os.path.abspath(c)
                break
    if cfg_path is None or not os.path.exists(cfg_path):
        print(f"Error: game.cfg not found. Use --cfg to specify path.")
        sys.exit(1)

    # Read existing extra_func entries from game.cfg
    existing = set()
    with open(cfg_path, "r") as f:
        for line in f:
            m = re.match(r'\s*extra_func\s+(?:0x)?([0-9A-Fa-f]+)', line)
            if m:
                existing.add(int(m.group(1), 16))

    # Read blacklist
    blacklist = set()
    bl_path = os.path.join(os.path.dirname(cfg_path), "blacklist.txt")
    if os.path.exists(bl_path):
        with open(bl_path) as f:
            for line in f:
                line = line.split("#")[0].strip()
                if line:
                    blacklist.add(int(line, 16))

    # Collect addresses from all log files: dispatch misses + interp fallbacks
    log_files = [args.misses]
    # Also check for interp_fallbacks.log in the same directory
    misses_dir = os.path.dirname(os.path.abspath(args.misses)) if os.path.exists(args.misses) else "."
    interp_log = os.path.join(misses_dir, "interp_fallbacks.log")
    if os.path.exists(interp_log) and interp_log not in log_files:
        log_files.append(interp_log)
    # Also check common locations
    for d in [".", os.path.dirname(__file__) + "/.."]:
        for name in ["dispatch_misses.log", "interp_fallbacks.log"]:
            p = os.path.abspath(os.path.join(d, name))
            if os.path.exists(p) and p not in [os.path.abspath(x) for x in log_files]:
                log_files.append(p)

    new_addrs = set()
    files_found = 0
    for log_path in log_files:
        if not os.path.exists(log_path):
            continue
        files_found += 1
        with open(log_path, "r") as f:
            for line in f:
                m = re.match(r'\s*extra_func\s+(?:0x)?([0-9A-Fa-f]+)', line)
                if m:
                    addr = int(m.group(1), 16)
                    if addr > 0x80000:
                        continue  # outside ROM
                    if addr in blacklist:
                        continue  # known bad
                    if addr not in existing:
                        new_addrs.add(addr)

    if files_found == 0:
        print(f"No log files found (checked: {', '.join(log_files)})")
        sys.exit(0)

    if not new_addrs:
        print(f"No new dispatch misses to add (all {len(existing)} already in {cfg_path})")
        sys.exit(0)

    # Append new entries
    with open(cfg_path, "a") as f:
        f.write(f"\n# Dispatch misses ingested from {args.misses}\n")
        for addr in sorted(new_addrs):
            f.write(f"extra_func 0x{addr:06X}\n")
            print(f"  Added: extra_func 0x{addr:06X}")

    print(f"\nAdded {len(new_addrs)} new extra_func entries to {cfg_path}")
    print(f"Next: rebuild recompiler output and game binary.")

    # Clear all processed log files
    for log_path in log_files:
        if os.path.exists(log_path):
            os.remove(log_path)
            print(f"Cleared {log_path}")


if __name__ == "__main__":
    main()
