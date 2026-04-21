#!/usr/bin/env python3
"""
rdb_litmus.py — Tier-1 reverse-debugger paired native/oracle run.

Starts SonicTheHedgehogRecomp.exe and SonicTheHedgehogRecomp_oracle.exe
from build-rdb/Release in interactive (non --max-frames) mode so the TCP
server stays up, arms an address-range filter for the FM ports
($A04000-$A04003), advances both by N frames via run_frames, dumps each
ring via rdb_dump, and reports the first divergent
(index, func, caller) pair.

Requires: the runner must be built with -DSONIC_REVERSE_DEBUG=ON and the
generated C must have been regenerated with `GenesisRecomp --reverse-debug`.
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
    payload = {'cmd': cmd, 'id': 1, **kw}
    sock.sendall((json.dumps(payload) + '\n').encode())
    buf = b''
    while b'\n' not in buf:
        chunk = sock.recv(1 << 22)
        if not chunk:
            raise RuntimeError(f"{cmd}: connection closed")
        buf += chunk
    return json.loads(buf.split(b'\n', 1)[0].decode())


def connect(port, label, retries=40, delay=0.5):
    last = None
    for _ in range(retries):
        try:
            return socket.create_connection(('127.0.0.1', port), timeout=30)
        except (ConnectionRefusedError, OSError) as e:
            last = e
            time.sleep(delay)
    raise RuntimeError(f"{label} port {port}: {last}")


def drain_dump(sock, label, chunk=50000, max_total=2_000_000):
    all_rows = []
    start = 0
    meta = None
    while start < max_total:
        r = rpc(sock, 'rdb_dump', start=start, count=chunk, timeout=120)
        if not r.get('ok'):
            raise RuntimeError(f"{label} rdb_dump failed: {r}")
        if meta is None:
            meta = {'total': r['total'], 'ranges': r['ranges']}
        all_rows.extend(r['log'])
        if r.get('done') or not r['log']:
            break
        start += len(r['log'])
    return meta, all_rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', default='build-rdb/Release')
    ap.add_argument('--rom',
                    default='segagenesisrecomp/sonicthehedgehog/sonic.bin')
    ap.add_argument('--frames', type=int, default=3600)
    ap.add_argument('--range', dest='ranges', action='append',
                    default=None,
                    help='Hex range lo-hi (e.g. A04000-A04003). Repeatable.')
    ap.add_argument('--out-prefix', default='rdb')
    ap.add_argument('--chunk-frames', type=int, default=600,
                    help='run_frames cap per call (server-side limit is '
                         'usually 36000; 600 is a safe chunk)')
    args = ap.parse_args()

    if args.ranges is None:
        args.ranges = ['A04000-A04003']

    build_dir = os.path.abspath(args.build_dir)
    rom = os.path.abspath(args.rom)
    native_exe = os.path.join(build_dir, 'SonicTheHedgehogRecomp.exe')
    oracle_exe = os.path.join(build_dir, 'SonicTheHedgehogRecomp_oracle.exe')
    for p in (native_exe, oracle_exe, rom):
        if not os.path.isfile(p):
            print(f"FATAL: missing {p}", file=sys.stderr)
            return 2

    subprocess.call(['taskkill', '/F', '/IM', 'SonicTheHedgehogRecomp.exe',
                     '/IM', 'SonicTheHedgehogRecomp_oracle.exe'],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    cmd_n = [native_exe, rom, '--turbo']
    cmd_o = [oracle_exe, rom, '--turbo']
    print(f"[rdb_litmus] launching native + oracle (interactive, turbo)...")
    p_n = subprocess.Popen(cmd_n, cwd=build_dir,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    p_o = subprocess.Popen(cmd_o, cwd=build_dir,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        s_n = connect(4378, 'native')
        s_o = connect(4379, 'oracle')

        # Pause both, arm ranges, then advance and dump.
        for sock, label in [(s_n, 'native'), (s_o, 'oracle')]:
            r = rpc(sock, 'pause')
            if not r.get('ok'):
                print(f"  WARN: {label} pause failed: {r}")

        for sock, label in [(s_n, 'native'), (s_o, 'oracle')]:
            rpc(sock, 'rdb_reset')
            for rng in args.ranges:
                lo, hi = rng.split('-')
                r = rpc(sock, 'rdb_range', lo='0x' + lo, hi='0x' + hi)
                if not r.get('ok'):
                    raise RuntimeError(f"{label} rdb_range {rng}: {r}")
                print(f"  [{label}] range 0x{lo}-0x{hi} armed "
                      f"(nranges={r['nranges']})")

        # Resume + advance. run_frames blocks until done and sends back a
        # result frame; we loop until we've covered `args.frames`.
        for sock, label in [(s_n, 'native'), (s_o, 'oracle')]:
            rpc(sock, 'continue')

        print(f"[rdb_litmus] advancing {args.frames} frames on each side "
              f"(chunks of {args.chunk_frames})...")
        for side_label, sock in [('native', s_n), ('oracle', s_o)]:
            remaining = args.frames
            while remaining > 0:
                n = min(remaining, args.chunk_frames)
                r = rpc(sock, 'run_frames', count=n, timeout=600)
                if not r.get('ok'):
                    raise RuntimeError(f"{side_label} run_frames {n}: {r}")
                remaining -= n

        print("[rdb_litmus] dumping rings...")
        meta_n, log_n = drain_dump(s_n, 'native')
        meta_o, log_o = drain_dump(s_o, 'oracle')

        out_n = f"{args.out_prefix}_native.json"
        out_o = f"{args.out_prefix}_oracle.json"
        with open(out_n, 'w') as f:
            json.dump({'meta': meta_n, 'log': log_n}, f)
        with open(out_o, 'w') as f:
            json.dump({'meta': meta_o, 'log': log_o}, f)
        print(f"  wrote {out_n} ({meta_n['total']} entries)")
        print(f"  wrote {out_o} ({meta_o['total']} entries)")

        # First divergence
        n = min(len(log_n), len(log_o))
        print(f"\n[rdb_litmus] scanning {n} paired entries for divergence...")
        first = None
        for i in range(n):
            a, b = log_n[i], log_o[i]
            if (a['adr'] != b['adr'] or a['val'] != b['val']
                    or a['func'] != b['func'] or a['caller'] != b['caller']):
                first = i
                break
        if first is None:
            if len(log_n) != len(log_o):
                print(f"  NO per-entry divergence found, but counts differ "
                      f"(native={len(log_n)}, oracle={len(log_o)})")
            else:
                print("  IDENTICAL across both rings.")
        else:
            print(f"  FIRST DIVERGENCE at index {first}")
            print(f"    native: {json.dumps(log_n[first], sort_keys=True)}")
            print(f"    oracle: {json.dumps(log_o[first], sort_keys=True)}")
            ctx = 2
            print(f"  context (indices {max(0,first-ctx)}..{first+ctx}):")
            for i in range(max(0, first - ctx), min(n, first + ctx + 1)):
                mark = ' <-' if i == first else '   '
                print(f"   {i:6d}{mark} "
                      f"n:{log_n[i]['adr']}={log_n[i]['val']} "
                      f"f={log_n[i]['func']} c={log_n[i]['caller']} | "
                      f"o:{log_o[i]['adr']}={log_o[i]['val']} "
                      f"f={log_o[i]['func']} c={log_o[i]['caller']}")

        return 0

    finally:
        for p in (p_n, p_o):
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()


if __name__ == '__main__':
    sys.exit(main() or 0)
