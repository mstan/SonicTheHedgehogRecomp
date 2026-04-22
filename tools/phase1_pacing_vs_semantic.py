#!/usr/bin/env python3
"""
phase1_pacing_vs_semantic.py — at FM write #1506 (the known divergence),
is SMPS RAM state identical on both sides?

Runs both targets with mem-write-log covering FM ports AND SMPS RAM
($FFF000-$FFF100). Parses each side's log to find:
  - FM writes in order (watch on 0xA04000-0xA04003)
  - Last RAM write to each SMPS byte before FM-write N

Reconstructs RAM state byte-map at the instant of FM write 1506 on
each side. Diffs the maps.

Outcome:
  identical → pure pacing/timing — same state, different trigger time
  different → semantic state cascade — one side's SMPS internal
              counters already diverged upstream of 1506
"""
import argparse
import os
import subprocess
import sys
import time


def parse_mem_log(path):
    """Parse the runner's --mem-write-log output.
    Columns: wall_frame internal_frame game_mode address value a7 ret0 ret1 ret2 ret3 cycle
    Returns list of dicts in order."""
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split()
            if len(parts) < 5:
                continue
            try:
                addr = int(parts[3], 0)
                val  = int(parts[4], 0)
            except ValueError:
                continue
            out.append({
                'wall':  int(parts[0]),
                'addr':  addr,
                'val':   val,
                'ret0':  int(parts[6], 0) if len(parts) > 6 else 0,
            })
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', default='build/Release')
    ap.add_argument('--rom',
                    default='segagenesisrecomp/sonicthehedgehog/sonic.bin')
    ap.add_argument('--frames', type=int, default=3600)
    ap.add_argument('--target-index', type=int, default=1506,
                    help='FM write index where divergence occurs')
    args = ap.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    rom = os.path.abspath(args.rom)
    exe_n = os.path.join(build_dir, 'SonicTheHedgehogRecomp.exe')
    exe_o = os.path.join(build_dir, 'SonicTheHedgehogRecomp_oracle.exe')
    for p in (exe_n, exe_o, rom):
        if not os.path.isfile(p):
            print(f"FATAL: missing {p}", file=sys.stderr); return 2

    subprocess.call(['taskkill', '/F', '/IM', 'SonicTheHedgehogRecomp.exe',
                     '/IM', 'SonicTheHedgehogRecomp_oracle.exe'],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # Watchlist covers FM ports + full SMPS state area.
    # $FFF000-$FFF040 = SMPS globals. Per-track structs are further out.
    # $FFF040-$FFFB80 = track structures for 10 tracks (0x30 each).
    # We watch 0xFFF000-0xFFF0FF (globals + first few tracks) as a fingerprint.
    watch_addrs = []
    for a in (0xA04000, 0xA04001, 0xA04002, 0xA04003):
        watch_addrs.append(a)
    # MEM_WRITE_LOG_MAX_WATCH = 32 per cmd_server.c. We have 4 FM + 28 left.
    # Sample a strided subset of the SMPS area to stay within the limit.
    strides = [0xFFF000, 0xFFF001, 0xFFF002, 0xFFF004,       # globals
               0xFFF010, 0xFFF020, 0xFFF030,                  # DAC track key bytes
               0xFFF040, 0xFFF048, 0xFFF04E, 0xFFF050, 0xFFF052, 0xFFF054,   # FM1
               0xFFF070, 0xFFF078, 0xFFF07E, 0xFFF080, 0xFFF082, 0xFFF084,   # FM2
               0xFFF0A0, 0xFFF0A8, 0xFFF0AE,                  # FM3
               0xFFF0D0, 0xFFF0D8, 0xFFF0DE,                  # FM4
               0xFFF100, 0xFFF130,                            # PSG
               0xFFFE0C, 0xFFF600]                            # v_vblank_count, v_gamemode
    watch_addrs.extend(strides)
    spec = ','.join(f"0x{a:06X}" for a in watch_addrs) + f"@{args.frames}"

    print(f"[phase1] watching {len(watch_addrs)} addresses, {args.frames} frames")
    print("[phase1] launching...")
    # Both exes write their logs to the current dir (cwd=build_dir).
    for f in ('mem_write_log_native.log', 'mem_write_log_oracle.log'):
        p = os.path.join(build_dir, f)
        if os.path.exists(p): os.remove(p)

    p_n = subprocess.Popen([exe_n, rom, '--max-frames', str(args.frames),
                            '--turbo', '--mem-write-log', spec],
                           cwd=build_dir,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    p_o = subprocess.Popen([exe_o, rom, '--max-frames', str(args.frames),
                            '--turbo', '--mem-write-log', spec],
                           cwd=build_dir,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        p_n.wait(timeout=600); p_o.wait(timeout=600)
    finally:
        for p in (p_n, p_o):
            if p.poll() is None: p.terminate()

    nat = parse_mem_log(os.path.join(build_dir, 'mem_write_log_native.log'))
    ora = parse_mem_log(os.path.join(build_dir, 'mem_write_log_oracle.log'))
    print(f"[phase1] native entries: {len(nat)},  oracle entries: {len(ora)}")

    FM_PORTS = {0xA04000, 0xA04001, 0xA04002, 0xA04003}

    def find_fm_write_index(log, n):
        """Return log index of the nth FM write (0-based index → nth)."""
        count = 0
        for i, e in enumerate(log):
            if e['addr'] in FM_PORTS:
                if count == n: return i
                count += 1
        return -1

    def ram_map_at(log, upto_idx):
        """Reconstruct {addr: last_value} for all SMPS-area writes up to
        (but not including) log index `upto_idx`."""
        m = {}
        for e in log[:upto_idx]:
            if e['addr'] not in FM_PORTS:
                m[e['addr']] = e['val']
        return m

    n_idx = find_fm_write_index(nat, args.target_index)
    o_idx = find_fm_write_index(ora, args.target_index)
    print(f"[phase1] FM write #{args.target_index} located:")
    print(f"  native log index: {n_idx}  wall_frame: {nat[n_idx]['wall'] if n_idx>=0 else '-'}  ret0: 0x{nat[n_idx]['ret0']:06X}")
    print(f"  oracle log index: {o_idx}  wall_frame: {ora[o_idx]['wall'] if o_idx>=0 else '-'}  ret0: 0x{ora[o_idx]['ret0']:06X}")

    if n_idx < 0 or o_idx < 0:
        print("[phase1] FAIL: target FM-write index not reached on one side")
        return 1

    nat_ram = ram_map_at(nat, n_idx)
    ora_ram = ram_map_at(ora, o_idx)
    all_addrs = sorted(set(nat_ram) | set(ora_ram))

    identical = []
    diffs     = []
    for a in all_addrs:
        nv = nat_ram.get(a, None)
        ov = ora_ram.get(a, None)
        if nv == ov: identical.append(a)
        else: diffs.append((a, nv, ov))

    print(f"\n[phase1] SMPS/RAM state at moment of FM write #{args.target_index}:")
    print(f"  addresses sampled: {len(all_addrs)}")
    print(f"  identical values:  {len(identical)}")
    print(f"  different values:  {len(diffs)}")
    if diffs:
        print("  per-address diffs:")
        for a, nv, ov in diffs:
            print(f"    $%06X  native=%s  oracle=%s" %
                  (a, f"0x{nv:02X}" if nv is not None else '--',
                   f"0x{ov:02X}" if ov is not None else '--'))

    print("\n[phase1] VERDICT:")
    if not diffs:
        print("  SMPS RAM state IDENTICAL at moment of divergent FM write.")
        print("  -> divergence is PURE PACING: both sides produce the same")
        print("    SMPS state but the next event is triggered at different")
        print("    real-time moments, causing the FM write stream to shift.")
        print("  Next step: fix pacing (cap, sync VBla to game-state, etc.)")
    else:
        print(f"  SMPS RAM state DIFFERS ({len(diffs)} bytes) at moment of")
        print("  divergent FM write. -> SEMANTIC state has already diverged")
        print("  upstream. The recompiler emitted code that produced a")
        print("  different internal SMPS state than the interpreter.")
        print("  Next step: trace backward from these bytes to the first")
        print("  byte that diverged.")
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
