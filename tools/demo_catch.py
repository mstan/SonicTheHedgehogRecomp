#!/usr/bin/env python3
"""demo_catch.py — catch the AIZ attract-demo monkey window on both runner
instances and snapshot the SAME sonic_x ring frame from each for an
apples-to-apples offline diff. Ring-consistent (query backward), no pause.

Waits until each instance (game_mode 0x08, zact AIZ=0x0000) has carried
Sonic past `--pass-x`, then locates the ring frame whose sonic_x is closest
to `--target-x` and dumps get_frame(include=wram,vram) for it, plus a parsed
summary of the MonkeyDude object family + monkey sprite-table entries.

Usage: demo_catch.py <out_dir> [--target-x N] [--pass-x N] [--timeout S]
                      [--ports 4384 4385]
"""
import socket, json, sys, os, time

OBJ_BASE = 0xB000          # within 64KB WRAM window
OBJ_SIZE = 0x4A
OBJ_COUNT = 110
SPRITE_TBL = 0xF800
MONKEY_CODES = {
    0x54F52:"Obj_MonkeyDude(init)", 0x54F56:"MonkeyDude(active)",
    0x54FBE:"loc_54FBE(resume)", 0x53FFC:"WaitOffscreen(wait)",
    0x550C8:"body", 0x55218:"arm_55218", 0x55248:"arm_55248",
    0x552B2:"coconut_552B2", 0x552CE:"coconut_552CE",
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

def w16(b,o): return (b[o]<<8)|b[o+1]
def s16(b,o):
    v=w16(b,o); return v-0x10000 if v>=0x8000 else v
def u32(b,o): return (b[o]<<24)|(b[o+1]<<16)|(b[o+2]<<8)|b[o+3]

def parse_objects(wram):
    out={}
    for i in range(OBJ_COUNT):
        base=OBJ_BASE+i*OBJ_SIZE
        if u32(wram,base)==0: continue
        code=u32(wram,base)
        out[i]={
            "code":"$%06X"%code, "tag":MONKEY_CODES.get(code,""),
            "routine":wram[base+0x05], "render":"0x%02X"%wram[base+0x04],
            "mapframe":wram[base+0x22], "art_tile":"0x%04X"%w16(wram,base+0x0A),
            "x":w16(wram,base+0x10), "y":w16(wram,base+0x14),
            "xvel":s16(wram,base+0x18), "yvel":s16(wram,base+0x1A),
            "subtype":wram[base+0x2C], "timer_2E":w16(wram,base+0x2E),
            "f30":"$%06X"%u32(wram,base+0x30), "f34":"$%06X"%u32(wram,base+0x34),
            "f38":"0x%02X"%wram[base+0x38], "f39":wram[base+0x39],
            "f3A":wram[base+0x3A], "f3B":wram[base+0x3B], "f3C":wram[base+0x3C],
            "f3E":"0x%04X"%w16(wram,base+0x3E), "f40":s16(wram,base+0x40),
            "f44":"0x%04X"%w16(wram,base+0x44), "parent3":"0x%04X"%w16(wram,base+0x46),
        }
    return out

def parse_sprites(vram, lo=0x548, hi=0x570):
    found=[]; idx=0; seen=0
    while seen<80:
        o=SPRITE_TBL+idx*8
        y=w16(vram,o); link=vram[o+3]; tile=w16(vram,o+4)&0x7FF; x=w16(vram,o+6)
        if lo<=tile<=hi:
            found.append({"sprite":idx,"y":y,"x":x,"tile":"0x%03X"%tile,
                          "size":"0x%02X"%vram[o+2],"link":link})
        seen+=1
        if link==0: break
        idx=link
    return found

def zact(port):
    try: return cmd(port,'read_memory',addr="0xFFFE10",size=2)['hex']
    except: return None

def capture(port, target_x, out_dir, label):
    fi=cmd(port,'frame_info'); F=fi['current_frame']-1; O=fi['oldest_frame']
    lo=max(O, F-400)
    ts=cmd(port,'frame_timeseries',field="wram16[B010]",**{"from":lo,"to":F})['values']
    # find frame closest to target_x (ignore nulls)
    best=None;bestd=10**9;bestf=None
    for i,v in enumerate(ts):
        if v is None: continue
        d=abs(v-target_x)
        if d<bestd: bestd=d; best=v; bestf=lo+i
    if bestf is None: return None
    fr=cmd(port,'get_frame',frame=bestf,include="wram,vram,cram,vsram")
    # get_frame nests VRAM inside the "vdp" object; WRAM is top-level.
    wram=bytes.fromhex(fr['wram']); vram=bytes.fromhex(fr['vdp']['vram'])
    summary={
        "label":label,"port":port,"frame":bestf,"sonic_x_at_frame":best,
        "target_x":target_x,
        "rng_seed":"0x%04X"%w16(wram,0xF636),
        "camera_x":w16(wram,0xEE78),"camera_y":w16(wram,0xEE7C),
        "game_mode":wram[0xF600],"zact":"0x%04X"%w16(wram,0xFE10),
        "demo_data_addr":"0x%06X"%u32(wram,0xEF52),
        "objects":parse_objects(wram),
        "monkey_sprites":parse_sprites(vram),
    }
    with open(os.path.join(out_dir,f"{label}_raw.json"),"w") as f:
        json.dump(fr,f)   # full wram+vram for deeper offline analysis
    with open(os.path.join(out_dir,f"{label}_summary.json"),"w") as f:
        json.dump(summary,f,indent=1)
    return summary

def main():
    out_dir=sys.argv[1]; os.makedirs(out_dir,exist_ok=True)
    target_x=6200; pass_x=6380; timeout=120.0
    # ports specified as "port:label" tokens; default native/oracle pair.
    labels={}
    a=sys.argv[2:]
    i=0
    while i<len(a):
        if a[i]=="--target-x": target_x=int(a[i+1]); i+=2
        elif a[i]=="--pass-x": pass_x=int(a[i+1]); i+=2
        elif a[i]=="--timeout": timeout=float(a[i+1]); i+=2
        elif a[i]=="--port":               # --port 4386:fixed
            tok=a[i+1]; p,_,lbl=tok.partition(':')
            labels[int(p)]=lbl or ("native" if int(p)%2==0 else "oracle"); i+=2
        else: i+=1
    if not labels: labels={4384:"native",4385:"oracle"}
    ports=list(labels)
    done={}
    t0=time.time()
    print(f"# waiting for AIZ demo Sonic to pass x={pass_x} (target capture x={target_x})",flush=True)
    while time.time()-t0<timeout and len(done)<len(ports):
        for p in ports:
            if p in done: continue
            try:
                st=cmd(p,'sonic_state')
            except Exception:
                continue
            za=zact(p)
            in_aiz = st.get('game_mode')==8 and za=="0000"
            print(f"  {labels[p]} gm=0x{st.get('game_mode'):02X} zact={za} x={st.get('x')}",flush=True)
            if in_aiz and st.get('x') is not None and st['x']>=pass_x:
                s=capture(p,target_x,out_dir,labels[p])
                if s:
                    done[p]=s
                    print(f"  >>> captured {labels[p]} at frame {s['frame']} sonic_x={s['sonic_x_at_frame']}",flush=True)
        time.sleep(0.3)
    print(f"# captured {sorted(labels[p] for p in done)} in {time.time()-t0:.1f}s",flush=True)

if __name__=="__main__":
    main()
