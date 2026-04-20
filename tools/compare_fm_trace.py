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
    """Returns a list of (frame, cycle, addr, value) tuples, skipping blank/comment."""
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'): continue
            parts = line.split()
            if len(parts) < 4: continue
            out.append((
                int(parts[0]),
                int(parts[1]),
                int(parts[2], 0),
                int(parts[3], 0),
            ))
    return out

def fetch_internal_frame_map(sock, start_wall, length):
    """Returns {wall_frame -> internal_frame} for [start_wall, start_wall+length-1].
    Skips entries the ring no longer holds; consumer handles missing keys."""
    rsp = rpc(sock, 'frame_timeseries',
              **{'from': start_wall, 'to': start_wall + length - 1,
                 'field': 'internal_frame'})
    if not rsp.get('ok'):
        raise RuntimeError(f"frame_timeseries(internal_frame) failed: {rsp}")
    vals = rsp['values']
    return {start_wall + i: v for i, v in enumerate(vals) if v is not None}

def bucket_by_internal_frame(trace, start_wall, wall_to_int):
    """Returns {internal_frame -> [(addr, value), ...]}. Trace.frame is
    relative to capture start, so absolute_wall = trace.frame + start_wall."""
    buckets = {}
    for relf, _cyc, addr, val in trace:
        wall = relf + start_wall
        ifr = wall_to_int.get(wall)
        if ifr is None: continue   # ring lost this frame
        buckets.setdefault(ifr, []).append((addr, val))
    return buckets

def diff_aligned(n_buckets, o_buckets, max_diffs):
    """Walk shared internal_frames in order; return list of divergences."""
    common = sorted(set(n_buckets) & set(o_buckets))
    out = []
    for ifr in common:
        ns = n_buckets[ifr]
        os_ = o_buckets[ifr]
        if ns == os_:
            continue
        # Find first per-write difference in this bucket.
        for k in range(min(len(ns), len(os_))):
            if ns[k] != os_[k]:
                out.append({
                    'internal_frame': ifr,
                    'index_in_frame': k,
                    'native':  {'addr': ns[k][0],  'val': ns[k][1]},
                    'oracle':  {'addr': os_[k][0], 'val': os_[k][1]},
                    'native_count':  len(ns),
                    'oracle_count':  len(os_),
                })
                break
        else:
            out.append({
                'internal_frame': ifr,
                'index_in_frame': min(len(ns), len(os_)),
                'native':  None if len(ns) <= len(os_) else
                           {'addr': ns[len(os_)][0], 'val': ns[len(os_)][1]},
                'oracle':  None if len(os_) <= len(ns) else
                           {'addr': os_[len(ns)][0], 'val': os_[len(ns)][1]},
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
        print(f"# native covers internal_frames: "
              f"[{min(nb,default='-')}..{max(nb,default='-')}]   "
              f"({len(nb)} distinct)")
        print(f"# oracle covers internal_frames: "
              f"[{min(ob,default='-')}..{max(ob,default='-')}]   "
              f"({len(ob)} distinct)")
        diffs, n_common = diff_aligned(nb, ob, args.max_diffs)
        print(f"# {n_common} internal_frames present on both sides")
        if not diffs:
            print("identical FM register writes across all matched internal_frames "
                  "— no audio codegen divergence detected")
            return 0
        for i, d in enumerate(diffs):
            print(f"\nDIVERGENCE #{i+1} @ internal_frame={d['internal_frame']} "
                  f"(write #{d['index_in_frame']} of "
                  f"native:{d['native_count']} / oracle:{d['oracle_count']})")
            if d.get('note'): print(f"  note: {d['note']}")
            if d['native']:
                print(f"  native: addr=0x{d['native']['addr']:04X} "
                      f"val=0x{d['native']['val']:02X}")
            if d['oracle']:
                print(f"  oracle: addr=0x{d['oracle']['addr']:04X} "
                      f"val=0x{d['oracle']['val']:02X}")
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
