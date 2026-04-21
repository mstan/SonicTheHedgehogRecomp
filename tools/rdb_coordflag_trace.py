#!/usr/bin/env python3
"""
rdb_coordflag_trace.py — step-trace CoordFlag (func_072A5A) via Tier 2.

Sets a breakpoint at CoordFlag entry. On the first park, captures the
input D5 (coord-flag byte), then repeatedly issues rdb_step + rdb_get_state
until control returns out of the function (stack[0] changes from the
CoordFlag caller). Prints a compact per-block trace so we can diff
against s1disasm or a known-good oracle run.

Also captures the first N parks (different coord-flag inputs) so we
see a real-world distribution, not a synthetic seed.
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time


def rpc(sock, cmd, timeout=10, **kw):
    sock.settimeout(timeout)
    sock.sendall((json.dumps({'cmd': cmd, 'id': 1, **kw}) + '\n').encode())
    buf = b''
    while b'\n' not in buf:
        chunk = sock.recv(1 << 20)
        if not chunk:
            raise RuntimeError(f"{cmd}: connection closed")
        buf += chunk
    return json.loads(buf.split(b'\n', 1)[0].decode())


def connect(port, retries=40):
    for _ in range(retries):
        try:
            return socket.create_connection(('127.0.0.1', port), timeout=20)
        except OSError:
            time.sleep(0.3)
    raise RuntimeError(f"no connect on {port}")


def wait_parked(sock, timeout_s=15.0):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        r = rpc(sock, 'rdb_get_state')
        if r.get('parked'):
            return r
        time.sleep(0.02)
    return None


def compact(state):
    """One-line summary: block, D0-D5, SR."""
    d = state['D']
    a = state['A']
    return (f"  block={state['block']} "
            f"D0={d[0]:08x} D1={d[1]:08x} D5={d[5]:08x} D6={d[6]:08x} "
            f"A0={a[0]:06x} A1={a[1]:06x} A6={a[6]:06x} "
            f"SR={state['SR']} stack0={state['stack'][0]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', default='build-rdb/Release')
    ap.add_argument('--rom',
                    default='segagenesisrecomp/sonicthehedgehog/sonic.bin')
    ap.add_argument('--breaks', type=int, default=5,
                    help='Number of CoordFlag hits to trace')
    ap.add_argument('--max-blocks', type=int, default=40,
                    help='Per-hit: step cap before forced continue')
    args = ap.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    rom = os.path.abspath(args.rom)
    exe = os.path.join(build_dir, 'SonicTheHedgehogRecomp.exe')
    for p in (exe, rom):
        if not os.path.isfile(p):
            print(f"FATAL: missing {p}", file=sys.stderr); return 2

    subprocess.call(['taskkill', '/F', '/IM', 'SonicTheHedgehogRecomp.exe'],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    p = subprocess.Popen([exe, rom, '--turbo'], cwd=build_dir,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = connect(4378)
        r = rpc(s, 'rdb_break', block='0x072A5A')
        print(f"[arm] {r}")

        for hit in range(args.breaks):
            st = wait_parked(s, timeout_s=20)
            if not st:
                print(f"[hit {hit}] no park within timeout"); break
            d5_byte = st['D'][5] & 0xFF
            stack0  = st['stack'][0]
            print(f"\n=== hit {hit} @ frame {st['frame']} — D5={d5_byte:#04x} "
                  f"caller(stack0)={stack0} ===")
            print(compact(st))

            # Step until we return out of CoordFlag (stack0 changes once
            # the JSR pop happens) OR max-blocks reached.
            entry_stack0 = stack0
            for step_i in range(args.max_blocks):
                rpc(s, 'rdb_step')
                st2 = wait_parked(s, timeout_s=5)
                if not st2:
                    print(f"  step {step_i}: no re-park — continued past fn"); break
                print(f"  step {step_i} " + compact(st2))
                # Leaving CoordFlag: the return pop makes stack0 == entry's
                # caller-of-caller (stack[1]). We detect by comparing block
                # to the break addr range — if block is no longer in 0x72A..,
                # we've left CoordFlag's dispatch tree.
                if int(st2['block'], 16) < 0x072A00 or int(st2['block'], 16) > 0x072FFF:
                    print(f"  -> left CoordFlag dispatch tree at block={st2['block']}")
                    break

            # Continue until the next CoordFlag hit.
            rpc(s, 'rdb_continue')
            time.sleep(0.1)

        return 0

    finally:
        if p.poll() is None:
            p.terminate()
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill()


if __name__ == '__main__':
    sys.exit(main() or 0)
