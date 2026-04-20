#!/usr/bin/env python3
"""
compare_fm_trace.py — compare YM2612 register-write timelines.

The existing fm_trace TCP command captures every FM register write
(frame, master-cycle, address, value) to a log file. For audio-bug
debugging, this is the *cleanest* signal: if native and oracle issue
the same register writes in the same order, the codegen is correct;
any divergence is a codegen bug at that write site.

Workflow:
  1. Launch both binaries with debug.ini next to each exe
  2. This script connects, enables fm_trace=on on both, waits, then
     disables and fetches+diffs the logs
  3. Prints the first divergence: frame, cycle, side that wrote it

Assumes default ports (native=4378 oracle=4379).
"""
import argparse, json, socket, sys, time, os

def rpc(sock, cmd, **kw):
    sock.sendall((json.dumps({'cmd':cmd,'id':1,**kw})+'\n').encode())
    buf = b''
    while b'\n' not in buf:
        ch = sock.recv(1<<20)
        if not ch: raise RuntimeError(f"{cmd} closed connection")
        buf += ch
    return json.loads(buf.split(b'\n',1)[0].decode())

def connect(port, label, retries=10, delay=0.5):
    last = None
    for _ in range(retries):
        try: return socket.create_connection(('127.0.0.1', port), timeout=10)
        except (ConnectionRefusedError, OSError) as e:
            last = e; time.sleep(delay)
    raise RuntimeError(f"{label} port {port}: {last}")

def read_trace(path):
    """Returns a list of (frame, cycle, addr, value, a7, [ret0..ret3]) tuples.
    Older logs without the stack columns are padded with zeros for
    backward compatibility. Skips blank/comment lines."""
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'): continue
            parts = line.split()
            if len(parts) < 4: continue
            a7   = int(parts[4], 0) if len(parts) > 4 else 0
            rets = [int(parts[5+i], 0) if len(parts) > 5+i else 0
                    for i in range(4)]
            out.append((
                int(parts[0]),
                int(parts[1]),
                int(parts[2], 0),
                int(parts[3], 0),
                a7,
                rets,
            ))
    return out

def fetch_internal_frame_map(sock, start_wall, length):
    """Returns {wall_frame -> (game_mode, internal_frame)} tuple-keyed by
    wall frame. Bucketing on internal_frame alone misaligns when native
    and oracle are in different game_modes at the same internal_frame
    (the side that runs faster reaches e.g. pre-demo while the slower
    side is still on title). Including game_mode in the bucket key
    enforces same-song comparison."""
    rsp_if = rpc(sock, 'frame_timeseries',
                 **{'from': start_wall, 'to': start_wall + length - 1,
                    'field': 'internal_frame'})
    rsp_gm = rpc(sock, 'frame_timeseries',
                 **{'from': start_wall, 'to': start_wall + length - 1,
                    'field': 'game_mode'})
    if not (rsp_if.get('ok') and rsp_gm.get('ok')):
        raise RuntimeError(f"frame_timeseries failed: if={rsp_if} gm={rsp_gm}")
    ivals = rsp_if['values']; gvals = rsp_gm['values']
    return {start_wall + i: (gvals[i], ivals[i])
            for i in range(min(len(ivals), len(gvals)))
            if ivals[i] is not None and gvals[i] is not None}

def bucket_by_internal_frame(trace, start_wall, wall_to_int):
    """Returns {(game_mode, internal_frame) -> [(addr, value, a7, rets), ...]}.
    trace.frame is relative to capture start, so absolute_wall =
    trace.frame + start_wall."""
    buckets = {}
    for entry in trace:
        relf, _cyc, addr, val, a7, rets = entry
        wall = relf + start_wall
        key = wall_to_int.get(wall)
        if key is None: continue   # ring lost this frame
        buckets.setdefault(key, []).append((addr, val, a7, tuple(rets)))
    return buckets

def _entry_view(entry):
    """Unpack a bucket entry into a diff-report dict."""
    addr, val, a7, rets = entry
    return {'addr': addr, 'val': val, 'a7': a7, 'rets': list(rets)}

def diff_aligned(n_buckets, o_buckets, max_diffs):
    """Walk shared internal_frames in order; return list of divergences.
    Writes are equal iff (addr, value) match — a7 and return chain differ
    across runs even when semantics are identical, so they're reported but
    not gated on."""
    # Keys are (game_mode, internal_frame) tuples. Sort by internal_frame
    # then by game_mode.
    common = sorted(set(n_buckets) & set(o_buckets), key=lambda k: (k[1], k[0]))
    out = []
    for key in common:
        gm, ifr = key
        ns = n_buckets[key]
        os_ = o_buckets[key]
        # Key difference check: write semantics only.
        ns_key = [(e[0], e[1]) for e in ns]
        os_key = [(e[0], e[1]) for e in os_]
        if ns_key == os_key:
            continue
        # Find first per-write difference in this bucket.
        for k in range(min(len(ns), len(os_))):
            if ns_key[k] != os_key[k]:
                out.append({
                    'game_mode': gm,
                    'internal_frame': ifr,
                    'index_in_frame': k,
                    'native':  _entry_view(ns[k]),
                    'oracle':  _entry_view(os_[k]),
                    'native_count':  len(ns),
                    'oracle_count':  len(os_),
                })
                break
        else:
            shorter = min(len(ns), len(os_))
            out.append({
                'game_mode': gm,
                'internal_frame': ifr,
                'index_in_frame': shorter,
                'native':  None if len(ns) <= len(os_) else _entry_view(ns[shorter]),
                'oracle':  None if len(os_) <= len(ns) else _entry_view(os_[shorter]),
                'native_count':  len(ns),
                'oracle_count':  len(os_),
                'note': 'one stream ran out (different write count)',
            })
        if len(out) >= max_diffs:
            break
    return out, len(common)

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--native', type=int, default=4378)
    ap.add_argument('--oracle', type=int, default=4379)
    ap.add_argument('--frames', type=int, default=300,
                    help='how many wall frames to record (default 300 = 5 sec)')
    ap.add_argument('--build-dir', default=None,
                    help='directory containing fm_trace_*.log (default: '
                         'guess from CWD)')
    ap.add_argument('--native-log', default=None,
                    help='override native log path')
    ap.add_argument('--oracle-log', default=None,
                    help='override oracle log path')
    ap.add_argument('--skip-capture', action='store_true',
                    help='assume trace logs already exist; just compare them')
    ap.add_argument('--align', choices=['internal-frame', 'stream-index'],
                    default='internal-frame',
                    help='how to pair writes from the two sides. '
                         'internal-frame: bucket by $FFFE0C counter '
                         '(true game-state alignment). '
                         'stream-index: pair Nth native write with Nth '
                         'oracle write (only useful when both ran exactly '
                         'the same internal-frame range).')
    ap.add_argument('--max-diffs', type=int, default=10)
    args = ap.parse_args()

    # Resolve log paths.
    bdir = args.build_dir or os.getcwd()
    n_log = args.native_log or os.path.join(bdir, 'fm_trace_native.log')
    o_log = args.oracle_log or os.path.join(bdir, 'fm_trace_interp.log')

    n_start = o_start = 0
    if not args.skip_capture:
        n = connect(args.native, 'native')
        o = connect(args.oracle, 'oracle')
        try:
            print(f"# enabling fm_trace on both (frames={args.frames})", file=sys.stderr)
            rn = rpc(n, 'fm_trace', action='on', frames=args.frames)
            ro = rpc(o, 'fm_trace', action='on', frames=args.frames)
            if not rn.get('ok') or not ro.get('ok'):
                sys.exit(f"fm_trace enable failed: native={rn} oracle={ro}")
            n_start = rn.get('start_frame', 0)
            o_start = ro.get('start_frame', 0)
            print(f"#   native log: {rn.get('file')} (start_frame={n_start})", file=sys.stderr)
            print(f"#   oracle log: {ro.get('file')} (start_frame={o_start})", file=sys.stderr)
            wall = (args.frames / 60.0) + 1.5
            print(f"# waiting {wall:.1f}s for capture...", file=sys.stderr)
            time.sleep(wall)
            rpc(n, 'fm_trace', action='off')
            rpc(o, 'fm_trace', action='off')
            # Build wall->internal_frame maps from each side's ring (for
            # internal-frame alignment). Pause first so the ring stops
            # rotating mid-query.
            rpc(n, 'pause'); rpc(o, 'pause')
            try:
                if args.align == 'internal-frame':
                    n_w2i = fetch_internal_frame_map(n, n_start, args.frames)
                    o_w2i = fetch_internal_frame_map(o, o_start, args.frames)
                else:
                    n_w2i = o_w2i = None
            finally:
                rpc(n, 'continue'); rpc(o, 'continue')
        finally:
            n.close(); o.close()
    else:
        # No live binaries to consult; can only do stream-index align.
        if args.align == 'internal-frame':
            sys.exit("--skip-capture forces --align stream-index "
                     "(internal-frame map needs live ring access)")
        n_w2i = o_w2i = None

    if not os.path.exists(n_log): sys.exit(f"missing {n_log}")
    if not os.path.exists(o_log): sys.exit(f"missing {o_log}")

    nt = read_trace(n_log)
    ot = read_trace(o_log)
    print(f"# native writes: {len(nt)}   oracle writes: {len(ot)}")

    if args.align == 'internal-frame':
        nb = bucket_by_internal_frame(nt, n_start, n_w2i)
        ob = bucket_by_internal_frame(ot, o_start, o_w2i)
        def _kr(buckets):
            if not buckets: return '-..-'
            lo = min(k[1] for k in buckets)
            hi = max(k[1] for k in buckets)
            return f"{lo}..{hi}"
        print(f"# native covers internal_frames: [{_kr(nb)}]   ({len(nb)} distinct (gm,if))")
        print(f"# oracle covers internal_frames: [{_kr(ob)}]   ({len(ob)} distinct (gm,if))")
        diffs, n_common = diff_aligned(nb, ob, args.max_diffs)
        print(f"# {n_common} (game_mode, internal_frame) tuples present on both sides")
        if not diffs:
            print("identical FM register writes across all matched (gm, internal_frame) tuples "
                  "— no audio codegen divergence detected")
            return 0
        for i, d in enumerate(diffs):
            print(f"\nDIVERGENCE #{i+1} @ game_mode={d['game_mode']} internal_frame={d['internal_frame']} "
                  f"(write #{d['index_in_frame']} of "
                  f"native:{d['native_count']} / oracle:{d['oracle_count']})")
            if d.get('note'): print(f"  note: {d['note']}")
            for side in ('native', 'oracle'):
                if d[side]:
                    e = d[side]
                    stack = ' '.join(f"0x{r:06X}" for r in e['rets'])
                    print(f"  {side}: addr=0x{e['addr']:04X} "
                          f"val=0x{e['val']:02X}  a7=0x{e['a7']:06X}  "
                          f"ret_chain=[{stack}]")
        return 1

    # stream-index legacy mode
    i = 0; j = 0; diffs = 0
    while i < len(nt) and j < len(ot):
        ni = nt[i]; oj = ot[j]
        if ni[2] == oj[2] and ni[3] == oj[3]:
            i += 1; j += 1
            continue
        print(f"DIVERGENCE #{diffs+1}:")
        print(f"  native[{i}]: frame={ni[0]} cyc={ni[1]} addr=0x{ni[2]:04X} val=0x{ni[3]:02X}")
        print(f"  oracle[{j}]: frame={oj[0]} cyc={oj[1]} addr=0x{oj[2]:04X} val=0x{oj[3]:02X}")
        diffs += 1
        if diffs >= args.max_diffs: break
        i += 1; j += 1
    if diffs == 0 and i == len(nt) == len(ot):
        print("identical FM register write streams"); return 0
    return 1 if diffs else 0

if __name__ == '__main__':
    sys.exit(main())
