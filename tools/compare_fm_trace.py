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

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--native', type=int, default=4378)
    ap.add_argument('--oracle', type=int, default=4379)
    ap.add_argument('--frames', type=int, default=300,
                    help='how many frames to record (default 300 = 5 sec)')
    ap.add_argument('--build-dir', default=None,
                    help='directory containing fm_trace_*.log (default: '
                         'guess from CWD)')
    ap.add_argument('--native-log', default=None,
                    help='override native log path')
    ap.add_argument('--oracle-log', default=None,
                    help='override oracle log path')
    ap.add_argument('--skip-capture', action='store_true',
                    help='assume trace logs already exist; just compare them')
    ap.add_argument('--max-diffs', type=int, default=10)
    args = ap.parse_args()

    # Resolve log paths.
    bdir = args.build_dir or os.getcwd()
    n_log = args.native_log or os.path.join(bdir, 'fm_trace_native.log')
    o_log = args.oracle_log or os.path.join(bdir, 'fm_trace_interp.log')

    if not args.skip_capture:
        n = connect(args.native, 'native')
        o = connect(args.oracle, 'oracle')
        try:
            print(f"# enabling fm_trace on both (frames={args.frames})", file=sys.stderr)
            rn = rpc(n, 'fm_trace', action='on', frames=args.frames)
            ro = rpc(o, 'fm_trace', action='on', frames=args.frames)
            if not rn.get('ok') or not ro.get('ok'):
                sys.exit(f"fm_trace enable failed: native={rn} oracle={ro}")
            print(f"#   native log: {rn.get('file')}", file=sys.stderr)
            print(f"#   oracle log: {ro.get('file')}", file=sys.stderr)
            # Wait for both to finish capturing (frames + small margin).
            wall = (args.frames / 60.0) + 1.5
            print(f"# waiting {wall:.1f}s for capture...", file=sys.stderr)
            time.sleep(wall)
            # Force-stop in case the frame counter didn't trip.
            rpc(n, 'fm_trace', action='off')
            rpc(o, 'fm_trace', action='off')
        finally:
            n.close(); o.close()

    if not os.path.exists(n_log): sys.exit(f"missing {n_log}")
    if not os.path.exists(o_log): sys.exit(f"missing {o_log}")

    nt = read_trace(n_log)
    ot = read_trace(o_log)
    print(f"# native writes: {len(nt)}   oracle writes: {len(ot)}")

    # Walk both streams in parallel and find first (addr,value) divergence.
    # Cycle numbers will differ between native and oracle because execution
    # speeds differ — that's not a bug, so we compare on (address, value)
    # ordering, not on cycle.
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
        # Advance both; when streams drift apart heuristically one-step both.
        i += 1; j += 1

    if diffs == 0 and i == len(nt) == len(ot):
        print("identical FM register write streams — no audio codegen divergence detected")
        return 0
    if diffs == 0:
        # Same prefix, but one stream is longer
        print(f"streams match for {min(len(nt),len(ot))} writes; "
              f"then one side has {abs(len(nt)-len(ot))} extra writes "
              f"({'native' if len(nt)>len(ot) else 'oracle'} longer)")
    return 1 if diffs else 0

if __name__ == '__main__':
    sys.exit(main())
