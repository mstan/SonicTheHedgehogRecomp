#!/usr/bin/env python3
"""
rdb_t3_diff.py — paired native Tier-2 / oracle Tier-3 divergence probe.

Runs both targets simultaneously. Arms a native break at a specific PC
and an oracle range filter that contains it. Polls until native parks;
captures native's D/A/SR via rdb_get_state. Issues rdb_step on native
in a loop, capturing each block boundary. Meanwhile oracle has been
recording per-instruction; we dump its ring and filter to the same
PCs native visited. Finally diffs the two lists, reporting the first
(pc, nth-hit) where D/A/SR disagree.

Use this to answer: "at runtime with the same inputs, do native and
oracle agree on register state at every block boundary inside the
suspected function?"
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time


def rpc(sock, cmd, timeout=30, **kw):
    sock.settimeout(timeout)
    sock.sendall((json.dumps({'id': 1, 'cmd': cmd, **kw}) + '\n').encode())
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


def wait_parked(sock, timeout_s=30.0):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        r = rpc(sock, 'rdb_get_state')
        if r.get('parked'): return r
        time.sleep(0.02)
    return None


def drain_t3(sock):
    out = []; start = 0
    while True:
        r = rpc(sock, 't3_dump', start=start, count=50000)
        out.extend(r['log'])
        if r.get('done') or not r['log']: break
        start += len(r['log'])
    return out


def cmp_state(nat, ora):
    """Return list of (field, nat, ora) that differ. Both dicts have
    'D' and 'A' lists and 'SR' hex string."""
    diffs = []
    for i in range(8):
        if nat['D'][i] != ora['D'][i]:
            diffs.append((f'D{i}', nat['D'][i], ora['D'][i]))
    for i in range(8):
        if nat['A'][i] != ora['A'][i]:
            diffs.append((f'A{i}', nat['A'][i], ora['A'][i]))
    ns = int(nat['SR'], 16) & 0x1F  # compare CCR bits only — I/S bits
    os_ = int(ora['sr'], 16) & 0x1F  # may legitimately differ
    if ns != os_:
        diffs.append(('SR_ccr', f'0x{ns:02x}', f'0x{os_:02x}'))
    return diffs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build-dir', default='build-rdb/Release')
    ap.add_argument('--rom',
                    default='segagenesisrecomp/sonicthehedgehog/sonic.bin')
    ap.add_argument('--break-at', default='0x072A5A',
                    help='Native break PC (and bottom of oracle range)')
    ap.add_argument('--range-hi', default='0x072FFF',
                    help='Top of oracle PC range')
    ap.add_argument('--steps', type=int, default=30,
                    help='Max native block-steps per hit before giving up')
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

        # Arm both.
        print(f"[native] rdb_break {args.break_at}")
        rpc(sn, 'rdb_break', block=args.break_at)
        print(f"[oracle] t3_range {args.break_at}-{args.range_hi}")
        rpc(so, 't3_reset')
        rpc(so, 't3_range', lo=args.break_at, hi=args.range_hi)

        # Wait for native to park at break_at.
        st = wait_parked(sn, timeout_s=20)
        if not st:
            print("[native] never parked — break address likely not hit"); return 1
        print(f"[native] parked @ block={st['block']} frame={st['frame']} "
              f"D5={st['D'][5]:#x} stack0={st['stack'][0]}")
        nat_snaps = [st]

        # Wait for oracle to accumulate hits on the break PC — oracle runs
        # at a different pace than native. We poll the ring's total until
        # it's stopped growing for a few checks (steady state).
        prev_total = 0
        stable_checks = 0
        for _ in range(200):
            r = rpc(so, 't3_dump', start=0, count=1)
            total = r['total']
            if total > 0 and total == prev_total:
                stable_checks += 1
                if stable_checks >= 3: break
            else:
                stable_checks = 0
            prev_total = total
            time.sleep(0.1)
        print(f"[oracle] ring has {prev_total} entries before stepping native")

        # Step native, collecting block-entry snapshots until we leave
        # the PC range.
        for i in range(args.steps):
            rpc(sn, 'rdb_step')
            st2 = wait_parked(sn, timeout_s=5)
            if not st2:
                print(f"[native] step {i}: never re-parked"); break
            nat_snaps.append(st2)
            b = int(st2['block'], 16)
            lo = int(args.break_at, 16); hi = int(args.range_hi, 16)
            if b < lo or b > hi:
                print(f"[native] step {i}: left range at block={st2['block']}")
                break

        # Drain oracle.
        ora_log = drain_t3(so)
        print(f"[oracle] captured {len(ora_log)} instruction entries")

        # Pair by matching the ENTRY call to a specific coord-flag input:
        # find the first oracle CoordFlag entry where D5 (low byte) matches
        # native's entry D5. Then walk forward through oracle's log
        # contiguously — every subsequent native block snapshot matches
        # the next oracle entry at that same PC. This gives like-for-like
        # comparison for a single CoordFlag call chain.
        entry_block = nat_snaps[0]['block']
        entry_d5_low = nat_snaps[0]['D'][5] & 0xFF
        oracle_start = None
        for i, ora in enumerate(ora_log):
            if ora['pc'] == entry_block and (ora['D'][5] & 0xFF) == entry_d5_low:
                oracle_start = i; break
        if oracle_start is None:
            print(f"\n[fail] no oracle entry at {entry_block} with D5={entry_d5_low:#x}")
            return 1
        print(f"\n[pair] native entry D5={entry_d5_low:#x} matches oracle "
              f"entry at log index {oracle_start} (frame {ora_log[oracle_start]['f']})")

        print("\n=== paired comparison (CCR bits of SR; I/S ignored) ===")
        matched_oracle = []
        cursor = oracle_start
        for nat in nat_snaps:
            # Advance oracle cursor until its pc matches this native block.
            found = None
            while cursor < len(ora_log):
                if ora_log[cursor]['pc'] == nat['block']:
                    found = ora_log[cursor]
                    cursor += 1
                    break
                cursor += 1
            matched_oracle.append(found)

        # Diff each pair — show all, not just first.
        # Classify: "entry-context" diffs are registers that differ AT the
        # entry block (0x072A5A for CoordFlag) already — i.e. the call was
        # made from different contexts, not a recompiler bug. Track those
        # and subtract them from later blocks to isolate any NEW divergence
        # introduced by recompiled code semantics.
        entry_context = set()
        if nat_snaps and matched_oracle[0]:
            for f, _, _ in cmp_state(nat_snaps[0], matched_oracle[0]):
                entry_context.add(f)
        if entry_context:
            print(f"[note] entry-context diffs (ignored downstream): "
                  f"{sorted(entry_context)}")

        any_new_div = False
        for nat, ora in zip(nat_snaps, matched_oracle):
            if ora is None:
                print(f"  block={nat['block']}  (no matching oracle entry)")
                continue
            diffs = cmp_state(nat, ora)
            new_diffs = [(f, n, o) for (f, n, o) in diffs
                         if f not in entry_context]
            if not diffs:
                print(f"  block={nat['block']}  [ok]  "
                      f"D0={nat['D'][0]:#x} D5={nat['D'][5]:#x}")
            elif not new_diffs:
                print(f"  block={nat['block']}  [carried] "
                      f"{len(diffs)} entry-context diffs still present, "
                      "no new divergence")
            else:
                any_new_div = True
                print(f"  block={nat['block']}  NEW DIVERGENCE:")
                for f, n, o in new_diffs:
                    print(f"    {f}: native={n}  oracle={o}")

        if not any_new_div:
            print("\n[result] no NEW divergence beyond entry-context. "
                  "Recompiler produces identical state evolution to "
                  "interpreter for this call path. Bug is upstream of "
                  f"the traced range {args.break_at}-{args.range_hi}.")

        return 0

    finally:
        for p in (pn, po):
            if p.poll() is None:
                p.terminate()
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired: p.kill()


if __name__ == '__main__':
    sys.exit(main() or 0)
