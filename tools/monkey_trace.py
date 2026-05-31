#!/usr/bin/env python3
"""monkey_trace.py — capture the MonkeyDude parent's activation + state
trajectory across an AIZ attract-demo pass on both runner instances, then
align native vs oracle by sonic_x (frame-perfect deterministic across both)
and report the first frame where the monkey state diverges.

Everything except the monkey is frame-identical at the same demo position,
so the monkey parent (fixed slot 7 = $FFB206) is the thing to watch:
when does code flip $53FFC(wait) -> $54F56(active), and how does routine
evolve. The body/arms live in slot-dependent children; we also tap slot 9
and slot 10 code to see which slot the body lands in.

Ring-consistent: waits until Sonic passes the monkey, then queries the
always-on frame_record ring backward. No pause/step.

Usage: monkey_trace.py <out_dir> [--pass-x N] [--timeout S]
"""
import socket, json, sys, os, time

PORTS = {4384: "native", 4385: "oracle"}
# frame_timeseries field strings (offsets within 64KB WRAM window)
TAPS = {
    "sx":   "wram16[B010]",   # sonic x
    "chi":  "wram16[B206]",   # monkey(slot7) code ptr HIGH word (expect 0x0005)
    "clo":  "wram16[B208]",   # monkey code ptr LOW word  ($4F52 spawn/$3FFC wait/$4F56 active/$4FBE resume)
    "rout": "wram[B20B]",     # monkey routine (offset 5)
    "t2e":  "wram16[B234]",   # monkey timer $2E
    "rend": "wram[B20A]",     # monkey render_flags (offset 4)
    "f34":  "wram16[B23C]",   # monkey $34 resume ptr LOW word (B206+$34+2)
    "my":   "wram16[B21A]",   # monkey y_pos (offset $14)
    "rng":  "wram16[F636]",   # RNG_seed
    "s9":   "wram16[B29C]",   # slot 9 code LOW word
    "s10":  "wram16[B2E6]",   # slot 10 code LOW word
    "cam":  "wram16[EE78]",   # camera_x
    "cback":"wram16[F7DA]",   # Camera_X_pos_coarse_back (drives WaitOffscreen on-screen test)
}

def cmd(port, name, **a):
    s = socket.socket(); s.settimeout(8); s.connect(('127.0.0.1', port))
    req={'id':1,'cmd':name}; req.update(a)
    s.sendall((json.dumps(req)+'\n').encode())
    buf=b''
    while b'\n' not in buf:
        ch=s.recv(1<<20)
        if not ch: break
        buf+=ch
    s.close()
    return json.loads(buf.decode('utf-8','replace').strip())

def zact(port):
    try: return cmd(port,'read_memory',addr="0xFFFE10",size=2)['hex']
    except: return None

def grab(port):
    fi=cmd(port,'frame_info'); F=fi['current_frame']-1; O=fi['oldest_frame']
    lo=max(O, F-560)
    series={}
    for k,f in TAPS.items():
        series[k]=cmd(port,'frame_timeseries',field=f,**{"from":lo,"to":F})['values']
    rows=[]
    n=F-lo+1
    for i in range(n):
        if series["sx"][i] is None: continue
        rows.append({k:series[k][i] for k in TAPS})
    return rows

def main():
    out=sys.argv[1]; os.makedirs(out,exist_ok=True)
    pass_x=6380; timeout=240.0
    a=sys.argv[2:]; i=0
    while i<len(a):
        if a[i]=="--pass-x": pass_x=int(a[i+1]); i+=2
        elif a[i]=="--timeout": timeout=float(a[i+1]); i+=2
        else: i+=1
    captured={}
    t0=time.time()
    print(f"# waiting for AIZ demo to pass x={pass_x}",flush=True)
    while time.time()-t0<timeout and len(captured)<len(PORTS):
        for p,lbl in PORTS.items():
            if p in captured: continue
            try: st=cmd(p,'sonic_state')
            except Exception: continue
            if st.get('game_mode')==8 and zact(p)=="0000" and st.get('x',0)>=pass_x:
                rows=grab(p)
                captured[p]=rows
                json.dump(rows, open(os.path.join(out,f"{lbl}_trace.json"),"w"))
                xs=[r["sx"] for r in rows]
                print(f"  >>> {lbl}: {len(rows)} frames, sonic_x {min(xs)}..{max(xs)}",flush=True)
        time.sleep(0.25)
    if len(captured)<len(PORTS):
        print("# timeout: did not capture both",flush=True); return
    # Align by sonic_x and diff the monkey parent state.
    nat={r["sx"]:r for r in captured[4384]}
    ora={r["sx"]:r for r in captured[4385]}
    common=sorted(set(nat)&set(ora))
    print(f"\n# aligned on {len(common)} common sonic_x values "
          f"({min(common)}..{max(common)})")
    print(f"# {'sx':>5} | {'code':^18} | {'rout':^7} | {'t2e':^7} | {'rend':^7} | {'rng':^11} | {'bodyslot':^9}")
    print(f"# {'':>5} | nat        ora    | nat ora | nat ora | nat ora | nat   ora   | nat ora")
    first_div=None
    def codestr(v): return "$%04X"%v if v is not None else "----"
    def bodyslot(r):
        if r["s9"]==0x50C8: return 9
        if r["s10"]==0x50C8: return 10
        return "-"
    prev=None
    for sx in common:
        n=nat[sx]; o=ora[sx]
        div = (n["clo"]!=o["clo"] or n["rout"]!=o["rout"])
        if div and first_div is None: first_div=sx
        sig=(n["clo"],o["clo"],n["rout"],o["rout"],n["rend"],o["rend"])
        if sig!=prev:
            mark = "  <-- DIVERGE" if div else ""
            print(f"  {sx:>5} | {codestr(n['clo'])} {codestr(o['clo'])} | "
                  f"{n['rout']!s:>3} {o['rout']!s:>3} | {n['t2e']!s:>5} {o['t2e']!s:>5} | "
                  f"0x{n['rend']:02X} 0x{o['rend']:02X} | "
                  f"{n['rng']:>5} {o['rng']:>5} | {bodyslot(n)!s:>3} {bodyslot(o)!s:>3}{mark}")
            prev=sig
    print(f"\n# FIRST monkey divergence at sonic_x = {first_div}")

if __name__=="__main__":
    main()
