#!/usr/bin/env python3
"""
rdb_insn_diff.py — compare 68K instructions-per-wall-frame between
native and oracle to locate the cap-mode tempo-slowdown bias.

Starts both targets, advances N wall frames, reads rdb_insn_counts on
each. Native's counter is ticked from generated C; oracle's from the
Tier-3 per-instruction hook. Ratio tells us whether the cap-mode
halving is from per-instruction overcount (native << oracle) or extra
path overhead (native ≈ oracle).

Use --pacing to drive native in fiber or accurate mode (oracle always
uses its interpreter's own pacing).
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time


def rpc(sock, cmd, timeout=120, **kw):
    sock.settimeout(timeout)
    sock.sendall((json.dumps({'cmd': cmd, 'id': 1, **kw}) + '\n').encode())
    buf = b''
    while b'\n' not in buf:
        chunk = sock.recv(1 << 22)
        if not chunk: raise RuntimeError(f"{cmd}: closed")
        buf += chunk
    return json.loads(buf.split(b'\n', 1)[0].decode())


def connect(port, retries=40):
    for _ in range(retries):
        try: return socket.create_connection(('127.0.0.1', port), timeout=20)
        except OSError: time.sleep(0.3)
    raise RuntimeError(f"no connect on {port}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', default='build-rdb/Release')
    ap.add_argument('--rom',
                    default='segagenesisrecomp/sonicthehedgehog/sonic.bin')
    ap.add_argument('--frames', type=int, default=600)
    ap.add_argument('--pacing', choices=['fiber', 'accurate'],
                    default='fiber',
                    help='Native pacing mode (oracle always uses interp)')
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

    cmd_n = [exe_n, rom, '--turbo', f'--pacing={args.pacing}']
    cmd_o = [exe_o, rom, '--turbo']
    print(f"[insn_diff] native pacing={args.pacing}, {args.frames} frames")

    pn = subprocess.Popen(cmd_n, cwd=build_dir,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    po = subprocess.Popen(cmd_o, cwd=build_dir,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sn = connect(4378); so = connect(4379)

        # Baseline sample (should both be ~small startup counts)
        r_n0 = rpc(sn, 'rdb_insn_counts')
        r_o0 = rpc(so, 'rdb_insn_counts')

        # Advance N frames on each.
        for label, sock in [('native', sn), ('oracle', so)]:
            remaining = args.frames
            while remaining > 0:
                n = min(remaining, 600)
                rpc(sock, 'run_frames', count=n, timeout=300)
                remaining -= n

        r_n1 = rpc(sn, 'rdb_insn_counts')
        r_o1 = rpc(so, 'rdb_insn_counts')

        nat_delta    = r_n1['native'] - r_n0['native']
        ora_delta    = r_o1['oracle'] - r_o0['oracle']
        nat_walls    = r_n1['wall_frame'] - r_n0['wall_frame']
        ora_walls    = r_o1['wall_frame'] - r_o0['wall_frame']

        print(f"\n=== NATIVE (pacing={args.pacing}) ===")
        print(f"  wall frames advanced: {nat_walls}")
        print(f"  68K instructions:     {nat_delta}")
        if nat_walls: print(f"  instructions / wall:  {nat_delta/nat_walls:.1f}")

        print(f"\n=== ORACLE ===")
        print(f"  wall frames advanced: {ora_walls}")
        print(f"  68K instructions:     {ora_delta}")
        if ora_walls: print(f"  instructions / wall:  {ora_delta/ora_walls:.1f}")

        if nat_walls and ora_walls and nat_delta and ora_delta:
            n_per = nat_delta / nat_walls
            o_per = ora_delta / ora_walls
            ratio = n_per / o_per
            print(f"\n=== RATIO ===")
            print(f"  native/oracle insns per wall frame = {ratio:.3f}")
            if ratio < 0.6:
                print("  INTERPRETATION: native executes FEWER instructions per")
                print("  wall frame than oracle → native's accumulator reaches")
                print("  cap/threshold too fast → per-instruction cycle costs")
                print("  are INFLATED (Option A). Next step: find which opcode(s).")
            elif ratio > 1.3:
                print("  INTERPRETATION: native executes MORE instructions per")
                print("  wall frame than oracle → extra dispatch/helper path")
                print("  overhead in the recompiled code (Option B). Next step:")
                print("  inspect call_by_address / hybrid_jmp_interpret paths.")
            else:
                print("  INTERPRETATION: instruction counts match. The bias is")
                print("  NOT in either per-instruction cost inflation or in")
                print("  extra instructions. Look elsewhere — e.g., the accumulator")
                print("  is being bumped in sites other than the emitted C.")

        return 0

    finally:
        for p in (pn, po):
            if p.poll() is None:
                p.terminate()
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired: p.kill()


if __name__ == '__main__':
    sys.exit(main() or 0)
