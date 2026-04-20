#!/usr/bin/env python3
"""smps_state_at.py — diff SMPS RAM between native and oracle at a
specific (game_mode, internal_frame) tuple.

SMPS driver state lives in $FFF000-$FFF1FF (approx), inside 68K work
RAM. When the FM write stream diverges (ISSUE-002), the root cause
often shows up here as a track PlaybackControl / DurationTimeout /
DataPointer byte that differs between native and oracle at the same
internal_frame.

The tool walks both rings for (game_mode, internal_frame) markers,
finds the matching wall-frames, fetches the 64 KB wram from each via
the `get_frame` TCP command, and prints every differing byte inside
the requested offset window.

Usage:
    python tools/smps_state_at.py --game-mode 0 --internal-frame 258
"""
import argparse, json, socket, sys

def rpc(sock, cmd, **kw):
    sock.sendall((json.dumps({'cmd':cmd,'id':1,**kw})+'\n').encode())
    buf = b''
    while b'\n' not in buf:
        ch = sock.recv(1<<22)
        if not ch: raise RuntimeError(f"{cmd} closed")
        buf += ch
    return json.loads(buf.split(b'\n',1)[0].decode())

def connect(port):
    return socket.create_connection(('127.0.0.1', port), timeout=30)

def find_wall_frame(sock, game_mode, internal_frame):
    """Return a wall frame where (game_mode, internal_frame) matches."""
    info = rpc(sock, 'frame_info')
    if not info.get('ok'):
        raise RuntimeError(f"frame_info failed: {info}")
    oldest = info['oldest_frame']
    current = info['current_frame']
    # current_frame is 1-past-last-recorded (s_history_count); clamp to
    # oldest + capacity-1 as the last queryable frame.
    last = current - 1 if current > 0 else 0
    rsp_gm = rpc(sock, 'frame_timeseries',
                 **{'from': oldest, 'to': last, 'field': 'game_mode'})
    rsp_if = rpc(sock, 'frame_timeseries',
                 **{'from': oldest, 'to': last, 'field': 'internal_frame'})
    if not (rsp_gm.get('ok') and rsp_if.get('ok')):
        raise RuntimeError(f"frame_timeseries failed: gm={rsp_gm} if={rsp_if}")
    gms = rsp_gm['values']; ifs = rsp_if['values']
    for i in range(min(len(gms), len(ifs))):
        if gms[i] == game_mode and ifs[i] == internal_frame:
            return oldest + i
    return None

def fetch_wram(sock, frame):
    """Fetch the wram bytes from a specific wall frame."""
    rsp = rpc(sock, 'get_frame', frame=frame, include=['wram'])
    if not rsp.get('ok'):
        raise RuntimeError(f"get_frame({frame}) failed: {rsp}")
    wram_b64 = rsp['frame']['wram']
    import base64
    return base64.b64decode(wram_b64)

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--native-port', type=int, default=4378)
    ap.add_argument('--oracle-port', type=int, default=4379)
    ap.add_argument('--game-mode', type=int, required=True)
    ap.add_argument('--internal-frame', type=int, required=True)
    ap.add_argument('--range-start', type=lambda s: int(s, 0), default=0xF000,
                    help='start offset (into 64 KB wram) to diff [0xF000]')
    ap.add_argument('--range-end', type=lambda s: int(s, 0), default=0xFDA0,
                    help='end offset (exclusive) [0xFDA0 — avoids stack noise]')
    ap.add_argument('--max-diffs', type=int, default=64)
    args = ap.parse_args()

    n = connect(args.native_port)
    o = connect(args.oracle_port)
    rpc(n, 'pause'); rpc(o, 'pause')
    try:
        n_wf = find_wall_frame(n, args.game_mode, args.internal_frame)
        o_wf = find_wall_frame(o, args.game_mode, args.internal_frame)
        print(f"# native wall-frame for (gm={args.game_mode}, "
              f"if={args.internal_frame}) = {n_wf}")
        print(f"# oracle wall-frame for (gm={args.game_mode}, "
              f"if={args.internal_frame}) = {o_wf}")
        if n_wf is None or o_wf is None:
            print("# ring doesn't hold that state on at least one side; aborting")
            return 1
        n_wram = fetch_wram(n, n_wf)
        o_wram = fetch_wram(o, o_wf)
        if len(n_wram) != 0x10000 or len(o_wram) != 0x10000:
            print(f"# unexpected wram sizes native={len(n_wram)} oracle={len(o_wram)}")
            return 1
        diffs = []
        for off in range(args.range_start, args.range_end):
            if n_wram[off] != o_wram[off]:
                diffs.append((off, n_wram[off], o_wram[off]))
        print(f"# diffs in [0x{args.range_start:04X}..0x{args.range_end:04X}): "
              f"{len(diffs)}")
        for off, nv, ov in diffs[:args.max_diffs]:
            print(f"  0x{0xFF0000 + off:06X}: native=0x{nv:02X} oracle=0x{ov:02X}")
        return 0 if not diffs else 1
    finally:
        rpc(n, 'continue'); rpc(o, 'continue')
        n.close(); o.close()

if __name__ == '__main__':
    sys.exit(main())
