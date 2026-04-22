#!/usr/bin/env python3
"""
rdb_vbla_diff.py — measure native VBla-fire distribution per wall frame.

Launches both native and oracle. Lets each run 600 wall frames at turbo.
Dumps:
  - Native: rdb_vbla_dump returns each fire's (wall, acc, game, reason).
            iterate_count is the wall-frame count.
  - Oracle: rdb_vbla_dump ring is empty (native-only path).
            iterate_count == wall frames advanced (should be 600).

Reports per-wall-frame fire-count distribution on native, and oracle's
iterate count. If native shows ANY wall frame with >1 fire, the
"native multi-fires VBla" hypothesis is confirmed.
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time
from collections import Counter


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


def drain_vbla(sock, label):
    out = []; start = 0; iter_count = 0
    while True:
        r = rpc(sock, 'rdb_vbla_dump', start=start, count=20000)
        out.extend(r['log'])
        iter_count = r['iterate_count']
        if r.get('done') or not r['log']: break
        start += len(r['log'])
    return out, iter_count


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', default='build-rdb/Release')
    ap.add_argument('--rom',
                    default='segagenesisrecomp/sonicthehedgehog/sonic.bin')
    ap.add_argument('--frames', type=int, default=600)
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

    pn = subprocess.Popen([exe_n, rom, '--turbo'], cwd=build_dir,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    po = subprocess.Popen([exe_o, rom, '--turbo'], cwd=build_dir,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sn = connect(4378); so = connect(4379)

        # Advance both via run_frames, chunked.
        for label, sock in [('native', sn), ('oracle', so)]:
            remaining = args.frames
            while remaining > 0:
                n = min(remaining, 600)
                rpc(sock, 'run_frames', count=n, timeout=300)
                remaining -= n

        nat_log, nat_iter = drain_vbla(sn, 'native')
        ora_log, ora_iter = drain_vbla(so, 'oracle')

        print("\n=== native ===")
        print(f"  iterate_count     = {nat_iter}")
        print(f"  total VBla fires  = {len(nat_log)}")
        if nat_log:
            print(f"  ratio fires/iter  = {len(nat_log) / max(nat_iter,1):.3f}")
            walls = [e['wall'] for e in nat_log]
            per_wall = Counter(walls)
            dist = Counter(per_wall.values())
            n_walls_with_fires = len(per_wall)
            n_walls_empty = max(0, nat_iter - n_walls_with_fires)
            print(f"  wall frames with fires: {n_walls_with_fires}")
            print(f"  wall frames empty:      {n_walls_empty}")
            print(f"  fire-count-per-wall distribution:")
            if n_walls_empty:
                print(f"    0 fires/wall:  {n_walls_empty}")
            for k in sorted(dist):
                print(f"    {k} fires/wall:  {dist[k]}")
            # Reasons
            reasons = Counter(e['reason'] for e in nat_log)
            R = {0: 'THRESHOLD', 1: 'SUPPRESSED', 2: 'FORCED'}
            print(f"  reasons: " +
                  ", ".join(f"{R.get(r,r)}={c}" for r, c in sorted(reasons.items())))
            # Acc-at-fire stats: should always be small (< g_vblank_threshold)
            accs = [e['acc'] for e in nat_log]
            print(f"  acc-at-fire min/avg/max: "
                  f"{min(accs)}/{sum(accs)//len(accs)}/{max(accs)}")

        print("\n=== oracle ===")
        print(f"  iterate_count     = {ora_iter}")
        print(f"  total VBla fires  = {len(ora_log)} "
              "(expected 0; oracle's VBla goes through interpreter, not glue.c)")

        print("\n=== verdict ===")
        if nat_log and any(c > 1 for c in Counter(e['wall']
                                                  for e in nat_log).values()):
            print("  MULTI-FIRE CONFIRMED. At least one wall frame on native "
                  "fires VBla more than once.")
        elif nat_log:
            print("  No multi-fire detected. Native fires <=1 per wall frame.")
        else:
            print("  No fires recorded — instrumentation broken or game hung.")

        return 0

    finally:
        for p in (pn, po):
            if p.poll() is None:
                p.terminate()
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired: p.kill()


if __name__ == '__main__':
    sys.exit(main() or 0)
