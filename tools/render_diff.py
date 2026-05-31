#!/usr/bin/env python3
"""render_diff.py — diff the full VDP render state of two demo_catch raw
captures (native vs oracle) taken at the same demo anchor. Surfaces where a
rendering divergence lives: VDP registers, CRAM, VSRAM, or which VRAM region
(plane A / plane B / sprite table / hscroll / tiles) differs.

Usage: render_diff.py <dirA> <dirB>   (each dir holds {label}_raw.json)
       e.g. render_diff.py _mk_native _mk_oracle
"""
import json, sys, glob, os

def load_raw(d):
    fs = glob.glob(os.path.join(d, "*_raw.json"))
    if not fs: raise SystemExit(f"no *_raw.json in {d}")
    return json.load(open(fs[0])), os.path.basename(fs[0])

def region_of(addr, v):
    """Classify a VRAM byte address by VDP base registers."""
    pa = v.get("plane_a", 0); pb = v.get("plane_b", 0)
    spr = v.get("sprite_table", 0); hs = v.get("hscroll", 0)
    win = v.get("window", 0)
    # plane size: width tiles from plane_w_shift (5->32,6->64,7->128), height from mask
    wsh = v.get("plane_w_shift", 6)
    wt = {5:32,6:64,7:128}.get(wsh,64)
    hmask = v.get("plane_h_mask", 0)
    ht = {0x00:32,0xFF:32,0x1F:32,0x3F:64,0x7F:128}.get(hmask,32)
    plane_bytes = wt*ht*2
    def inr(a,base,size): return base<=a<base+size
    if inr(addr,pa,plane_bytes): return "planeA"
    if inr(addr,pb,plane_bytes): return "planeB"
    if inr(addr,spr,640):        return "sprite_tbl"
    if inr(addr,hs,0x400):       return "hscroll"
    if win and inr(addr,win,plane_bytes): return "window"
    return "tiles/other"

def load_file(p):
    return json.load(open(p)), os.path.basename(p)

def main():
    if len(sys.argv) == 2:
        # single dir holding native_raw.json + oracle_raw.json
        d = sys.argv[1]
        A,fa = load_file(os.path.join(d,"native_raw.json"))
        B,fb = load_file(os.path.join(d,"oracle_raw.json"))
        dA=dB=d
    else:
        dA, dB = sys.argv[1], sys.argv[2]
        A,fa = load_raw(dA); B,fb = load_raw(dB)
    print(f"A(native) = {dA}/{fa}   frame={A.get('frame')}")
    print(f"B(oracle) = {dB}/{fb}   frame={B.get('frame')}")
    va, vb = A["vdp"], B["vdp"]

    # --- VDP scalar registers ---
    print("\n[VDP registers]")
    skip={"vram","cram","vsram"}
    nreg=0
    for k in va:
        if k in skip: continue
        if va.get(k)!=vb.get(k):
            print(f"  {k}: A={va.get(k)}  B={vb.get(k)}"); nreg+=1
    if not nreg: print("  (identical)")

    # --- CRAM ---
    ca, cb = va.get("cram",[]), vb.get("cram",[])
    cdiff=[(i,ca[i],cb[i]) for i in range(min(len(ca),len(cb))) if ca[i]!=cb[i]]
    print(f"\n[CRAM] {len(cdiff)} of {len(ca)} entries differ")
    for i,x,y in cdiff[:20]:
        print(f"  pal[{i:2d}] (line {i//16} idx {i%16}): A=0x{x:03X} B=0x{y:03X}")

    # --- VSRAM ---
    sa, sb = va.get("vsram",[]), vb.get("vsram",[])
    sdiff=[(i,sa[i],sb[i]) for i in range(min(len(sa),len(sb))) if sa[i]!=sb[i]]
    print(f"\n[VSRAM] {len(sdiff)} of {len(sa)} entries differ")
    for i,x,y in sdiff[:20]:
        print(f"  vsram[{i:2d}] ({'A' if i%2==0 else 'B'} col {i//2}): A={x} B={y}")

    # --- VRAM by region ---
    ra = bytes.fromhex(va["vram"]); rb = bytes.fromhex(vb["vram"])
    n = min(len(ra),len(rb))
    from collections import Counter
    byreg=Counter(); first={}
    total=0
    for a in range(n):
        if ra[a]!=rb[a]:
            total+=1
            r=region_of(a,va)
            byreg[r]+=1
            if r not in first: first[r]=a
    print(f"\n[VRAM] {total} of {n} bytes differ")
    print(f"  bases: planeA=0x{va.get('plane_a',0):04X} planeB=0x{va.get('plane_b',0):04X} "
          f"sprite=0x{va.get('sprite_table',0):04X} hscroll=0x{va.get('hscroll',0):04X} "
          f"w_shift={va.get('plane_w_shift')} h_mask=0x{va.get('plane_h_mask',0):02X}")
    for r,c in byreg.most_common():
        print(f"  {r:12s}: {c:6d} bytes  (first @ 0x{first[r]:04X})")

    # --- key WRAM render vars ---
    print("\n[WRAM render vars]")
    wa=bytes.fromhex(A["wram"]); wb=bytes.fromhex(B["wram"])
    def w16(b,o): return (b[o]<<8)|b[o+1]
    for name,off in [("Camera_X $EE78",0xEE78),("Camera_Y $EE7C",0xEE7C),
                     ("Camera_X_back $F70A",0xF70A),("Camera_Y_back $F70E",0xF70E),
                     ("Bg1_X $EE0C",0xEE0C),("Bg1_Y $EE10",0xEE10)]:
        x,y=w16(wa,off),w16(wb,off)
        mark="" if x==y else "   <-- DIFF"
        print(f"  {name}: A={x} B={y}{mark}")

if __name__=="__main__":
    main()
