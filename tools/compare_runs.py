#!/usr/bin/env python3
"""
compare_runs.py — live dual-TCP first-divergence finder.

The canonical entry point for any audio or visual bug per DEBUG.md.

Connects to a native build (default port 4378) and an oracle build
(default port 4379), walks both ring buffers in parallel, and reports
the first frame where the requested subsystem state differs.

Both servers are expected to be running already with debug.ini present
next to the exe. Drive both with the same input by either:
  (a) pressing the same buttons in both windows manually, or
  (b) using --script to send a sequence via the set_input TCP command.

Field tokens (comma-separated):
    cpu        — full M68KRegSnap (D, A, USP, SR, exploded flags)
    z80        — Z80 register file (no RAM)
    z80_ram    — full 8 KB Z80 RAM (byte diff)
    vdp        — VDP semantic regs (display, scroll, addresses)
    vram       — full 64 KB VRAM
    cram       — palette (64 × uint16)
    vsram      — vertical scroll (64 × uint16)
    fm         — YM2612 raw register/operator state (byte diff)
    psg        — PSG raw state (byte diff)
    wram       — full 64 KB 68K work RAM (byte diff)
    game_data  — Sonic-decoded view (sonic_x/y/vel/routine/...)
    all        — every field above

Examples:
    # Audio bug — find first FM/PSG/Z80 divergence
    python tools/compare_runs.py --first-divergence --field fm,psg,z80,wram

    # Visual bug
    python tools/compare_runs.py --first-divergence --field vdp,vram,cram

    # Gameplay (physics)
    python tools/compare_runs.py --first-divergence --field cpu,wram

    # Print byte-level CSV across a range
    python tools/compare_runs.py --range 0:300 --field game_data --csv out.csv
"""
import argparse, json, socket, sys, time

# ---------- TCP client ---------------------------------------------------

class Client:
    def __init__(self, port, name):
        self.port = port
        self.name = name
        self.sock = None
        self._next_id = 1
    def connect(self, retries=10, delay=0.5):
        last = None
        for _ in range(retries):
            try:
                self.sock = socket.create_connection(('127.0.0.1', self.port), timeout=10)
                return
            except (ConnectionRefusedError, OSError) as e:
                last = e
                time.sleep(delay)
        raise RuntimeError(f"could not connect to {self.name} on port {self.port}: {last}")
    def call(self, cmd, **kw):
        rid = self._next_id; self._next_id += 1
        msg = {'cmd': cmd, 'id': rid, **kw}
        self.sock.sendall((json.dumps(msg) + '\n').encode())
        buf = b''
        while b'\n' not in buf:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise RuntimeError(f"{self.name} closed connection mid-response")
            buf += chunk
        line, _, _ = buf.partition(b'\n')
        return json.loads(line.decode())
    def close(self):
        if self.sock:
            try: self.sock.close()
            except Exception: pass
            self.sock = None

# ---------- per-field diff ----------------------------------------------

def _hex_to_bytes(s):
    return bytes.fromhex(s) if isinstance(s, str) else b''

def _diff_bytes(label, a, b, max_diffs=8):
    diffs = []
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            diffs.append((i, a[i], b[i]))
            if len(diffs) >= max_diffs:
                break
    if len(a) != len(b):
        diffs.append(('LEN', len(a), len(b)))
    return diffs

def _diff_dict(label, a, b, max_diffs=8):
    out = []
    keys = sorted(set(a.keys()) | set(b.keys()))
    for k in keys:
        va, vb = a.get(k), b.get(k)
        if va != vb:
            out.append((k, va, vb))
            if len(out) >= max_diffs:
                break
    return out

# ---------- field-by-field comparators ----------------------------------

FIELD_INCLUDE = {
    # field-name -> get_frame "include" tokens that bring back its bytes
    'cpu':       '',
    'z80':       '',
    'z80_ram':   'z80_ram',
    'vdp':       '',
    'vram':      'vram',
    'cram':      'cram',
    'vsram':     'vsram',
    'fm':        '',
    'psg':       '',
    'wram':      'wram',
    'game_data': '',
}

ALL_FIELDS = list(FIELD_INCLUDE.keys())

def parse_fields(spec):
    if spec == 'all':
        return ALL_FIELDS[:]
    out = []
    for tok in spec.split(','):
        tok = tok.strip()
        if not tok: continue
        if tok == 'all':
            return ALL_FIELDS[:]
        if tok not in FIELD_INCLUDE:
            sys.exit(f"unknown field '{tok}'. Known: {','.join(ALL_FIELDS)},all")
        out.append(tok)
    return out

def field_diff(field, a_frame, b_frame):
    """Return a list of (loc, native, oracle) diffs for this field."""
    if field == 'cpu':
        return _diff_dict('m68k', a_frame.get('m68k', {}), b_frame.get('m68k', {}))
    if field == 'z80':
        a = dict(a_frame.get('z80', {})); b = dict(b_frame.get('z80', {}))
        a.pop('ram', None); b.pop('ram', None)
        return _diff_dict('z80', a, b)
    if field == 'z80_ram':
        return _diff_bytes('z80_ram',
                           _hex_to_bytes(a_frame.get('z80', {}).get('ram', '')),
                           _hex_to_bytes(b_frame.get('z80', {}).get('ram', '')))
    if field == 'vdp':
        a = {k: v for k, v in a_frame.get('vdp', {}).items() if k not in ('vram','cram','vsram')}
        b = {k: v for k, v in b_frame.get('vdp', {}).items() if k not in ('vram','cram','vsram')}
        return _diff_dict('vdp', a, b)
    if field == 'vram':
        return _diff_bytes('vram',
                           _hex_to_bytes(a_frame.get('vdp', {}).get('vram', '')),
                           _hex_to_bytes(b_frame.get('vdp', {}).get('vram', '')))
    if field == 'cram':
        a = a_frame.get('vdp', {}).get('cram', [])
        b = b_frame.get('vdp', {}).get('cram', [])
        diffs = []
        for i in range(min(len(a), len(b))):
            if a[i] != b[i]:
                diffs.append((i, a[i], b[i]))
        return diffs
    if field == 'vsram':
        a = a_frame.get('vdp', {}).get('vsram', [])
        b = b_frame.get('vdp', {}).get('vsram', [])
        diffs = []
        for i in range(min(len(a), len(b))):
            if a[i] != b[i]:
                diffs.append((i, a[i], b[i]))
        return diffs
    if field == 'fm':
        return _diff_bytes('fm',
                           _hex_to_bytes(a_frame.get('fm', {}).get('raw', '')),
                           _hex_to_bytes(b_frame.get('fm', {}).get('raw', '')))
    if field == 'psg':
        return _diff_bytes('psg',
                           _hex_to_bytes(a_frame.get('psg', {}).get('raw', '')),
                           _hex_to_bytes(b_frame.get('psg', {}).get('raw', '')))
    if field == 'wram':
        return _diff_bytes('wram',
                           _hex_to_bytes(a_frame.get('wram', '')),
                           _hex_to_bytes(b_frame.get('wram', '')))
    if field == 'game_data':
        return _diff_dict('game_data',
                          a_frame.get('game_data', {}).get('sonic', {}),
                          b_frame.get('game_data', {}).get('sonic', {}))
    return []

# ---------- main loop ---------------------------------------------------

def fetch_frame(client, frame, include_tokens):
    rsp = client.call('get_frame', frame=frame,
                      include=','.join(include_tokens) if include_tokens else '')
    if not rsp.get('ok'):
        raise RuntimeError(f"{client.name} get_frame({frame}): {rsp.get('error', rsp)}")
    return rsp

def common_frame_window(a, b):
    """Return (lo, hi) inclusive intersection of both ring windows."""
    af = a.call('frame_info'); bf = b.call('frame_info')
    if not af.get('ok') or not bf.get('ok'):
        raise RuntimeError(f"frame_info failed: native={af} oracle={bf}")
    lo = max(af['oldest_frame'], bf['oldest_frame'])
    hi = min(af['current_frame'] - 1, bf['current_frame'] - 1)
    return lo, hi

def run_first_divergence(args, native, oracle):
    fields = parse_fields(args.field)
    include_tokens = sorted({tok for f in fields if (tok := FIELD_INCLUDE[f])})

    lo, hi = common_frame_window(native, oracle)
    if args.start is not None: lo = max(lo, args.start)
    if args.end   is not None: hi = min(hi, args.end)
    if hi < lo:
        sys.exit(f"no overlapping frames: lo={lo} hi={hi}")

    print(f"# scanning frames {lo}..{hi}, fields={','.join(fields)}", file=sys.stderr)
    for f in range(lo, hi + 1):
        af = fetch_frame(native, f, include_tokens)
        of = fetch_frame(oracle, f, include_tokens)
        for fld in fields:
            d = field_diff(fld, af, of)
            if d:
                print(f"FIRST DIVERGENCE @ frame={f} field={fld}")
                for loc, na, ob in d[:args.max_diffs]:
                    if isinstance(loc, int):
                        print(f"  byte/index 0x{loc:X}: native=0x{na:X} oracle=0x{ob:X}")
                    else:
                        print(f"  {loc}: native={na} oracle={ob}")
                return 1
        if (f - lo) % 60 == 0:
            print(f"  ... frame {f}: no divergence in {','.join(fields)}", file=sys.stderr)
    print("no divergence found in scanned range")
    return 0

def run_csv(args, native, oracle):
    fields = parse_fields(args.field)
    include_tokens = sorted({tok for f in fields if (tok := FIELD_INCLUDE[f])})
    lo_str, hi_str = args.range.split(':')
    lo, hi = int(lo_str), int(hi_str)

    out = open(args.csv, 'w')
    out.write("frame,field,loc,native,oracle\n")
    for f in range(lo, hi + 1):
        af = fetch_frame(native, f, include_tokens)
        of = fetch_frame(oracle, f, include_tokens)
        for fld in fields:
            for loc, na, ob in field_diff(fld, af, of):
                out.write(f"{f},{fld},{loc},{na},{ob}\n")
    out.close()
    print(f"wrote {args.csv}")
    return 0

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--native', type=int, default=4378, help='native debug-server port')
    ap.add_argument('--oracle', type=int, default=4379, help='oracle debug-server port')
    ap.add_argument('--field', default='all',
                    help='comma-separated field tokens (see header docstring)')
    ap.add_argument('--start', type=int, default=None,
                    help='first frame to scan (default: oldest in both rings)')
    ap.add_argument('--end',   type=int, default=None,
                    help='last frame to scan')
    ap.add_argument('--max-diffs', type=int, default=16,
                    help='max diff entries to print per field')
    ap.add_argument('--first-divergence', action='store_true',
                    help='stop at the first frame where any field diverges')
    ap.add_argument('--range', help='for --csv mode: from:to inclusive')
    ap.add_argument('--csv',   help='write per-divergence CSV here')
    args = ap.parse_args()

    if args.csv and not args.range:
        sys.exit("--csv requires --range FROM:TO")

    native = Client(args.native, 'native')
    oracle = Client(args.oracle, 'oracle')
    native.connect(); oracle.connect()
    try:
        if args.csv:
            return run_csv(args, native, oracle)
        return run_first_divergence(args, native, oracle)
    finally:
        native.close(); oracle.close()

if __name__ == '__main__':
    sys.exit(main())
