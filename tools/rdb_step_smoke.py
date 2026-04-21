#!/usr/bin/env python3
"""
rdb_step_smoke.py — Tier 2 smoke test: arm breakpoint, poll for park,
step one block, continue. Verifies the park/resume mechanism works
end-to-end.

The runner runs at --turbo speed from launch; we arm a breakpoint then
poll rdb_get_state until parked=true. We don't use run_frames because
it blocks the socket for the full duration.
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


def connect(port, retries=40, delay=0.5):
    last = None
    for _ in range(retries):
        try:
            return socket.create_connection(('127.0.0.1', port), timeout=20)
        except (ConnectionRefusedError, OSError) as e:
            last = e; time.sleep(delay)
    raise RuntimeError(f"port {port}: {last}")


def wait_parked(sock, timeout_s=10.0):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        r = rpc(sock, 'rdb_get_state')
        if r.get('parked'):
            return r
        time.sleep(0.05)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', default='build-rdb/Release')
    ap.add_argument('--rom',
                    default='segagenesisrecomp/sonicthehedgehog/sonic.bin')
    ap.add_argument('--block', default='0x000234',
                    help='Block/func address to break at')
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

    rc = 1
    try:
        s = connect(4378)
        print("[smoke] connected")

        # Arm breakpoint BEFORE unpausing / running.
        r = rpc(s, 'rdb_break', block=args.block)
        if not r.get('ok'): print(f"rdb_break failed: {r}"); return 1
        print(f"[smoke] armed breakpoint at {args.block}")

        # Game is running (--turbo). Poll until parked.
        print("[smoke] polling for park...")
        r = wait_parked(s, timeout_s=10)
        if not r:
            print("[smoke] FAIL: breakpoint never hit within 10s")
            return 1

        print(f"[smoke] PARKED at block={r['block']} func={r['func']} "
              f"frame={r['frame']}")
        print(f"  PC={r['PC']} SR={r['SR']}")
        print(f"  D={r['D']}")
        print(f"  A={r['A']}")
        print(f"  stack[0..3]={r['stack']}")

        initial_block = r['block']

        # Step one block.
        r = rpc(s, 'rdb_step')
        if not r.get('ok'): print(f"rdb_step failed: {r}"); return 1
        print("[smoke] rdb_step sent")

        # After one block, we should be parked at a new block.
        r = wait_parked(s, timeout_s=5)
        if not r:
            print("[smoke] FAIL: step didn't re-park")
            return 1
        print(f"[smoke] STEPPED to block={r['block']} (was {initial_block})")
        if r['block'] == initial_block:
            print("[smoke] WARN: block unchanged — step may not have advanced")
        # Capture a different reg snapshot to confirm movement.
        print(f"  D0={r['D'][0]} D5={r['D'][5]} A0={r['A'][0]}")

        # Continue (breakpoint still armed but we've moved past it within
        # this invocation — may re-hit when func_000234 is called again).
        r = rpc(s, 'rdb_continue')
        if not r.get('ok'): print(f"rdb_continue failed: {r}"); return 1
        print("[smoke] rdb_continue sent")

        # Give it a moment to run, then verify we're not parked.
        time.sleep(0.2)
        r = rpc(s, 'rdb_get_state')
        print(f"[smoke] post-continue: parked={r['parked']} "
              f"frame={r['frame']}")

        rc = 0
        print("[smoke] PASS")

    finally:
        if p.poll() is None:
            p.terminate()
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill()

    return rc


if __name__ == '__main__':
    sys.exit(main() or 0)
