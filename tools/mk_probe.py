#!/usr/bin/env python3
"""mk_probe.py — MonkeyDude divergence probe (Sonic 3 standalone).

Closed-loop driver + sampler that runs entirely inside one process so there
is no LLM-round-trip gap that would let Sonic overshoot. Drives Sonic to a
target world-x by holding Right on the free-running runner, brakes, then
dumps full object RAM for every live slot plus RNG_seed and the VDP sprite
table. Designed to be run against native and oracle and diffed.

Usage:
  mk_probe.py <port> drive <target_x>   # drive to target_x, stop, dump
  mk_probe.py <port> dump               # just dump current state
  mk_probe.py <port> sample <slot> <n>  # sample slot's full RAM over n polls

Object model (sonic3_spec.c): base $FFB000, size $4A, 110 slots.
"""
import socket, json, sys, time

OBJ_BASE = 0xFFB000
OBJ_SIZE = 0x4A
OBJ_COUNT = 110
RNG_SEED = 0xFFF636            # word
SPRITE_TBL = 0xF800            # VDP sprite attribute table (640 bytes = 80 sprites*8)

# MonkeyDude family code pointers (offset $00) — used to tag interesting slots.
MONKEY_CODES = {
    0x54F52: "Obj_MonkeyDude(init)",
    0x54F56: "MonkeyDude(active)",
    0x54FBE: "loc_54FBE(resume)",
    0x53FFC: "WaitOffscreen(wait)",
    0x550C8: "body",
    0x55218: "arm_55218",
    0x55248: "arm_55248",
    0x552B2: "coconut_552B2",
    0x552CE: "coconut_552CE",
}

class Conn:
    def __init__(self, port):
        self.port = port
    def cmd(self, name, **args):
        s = socket.socket(); s.settimeout(5); s.connect(('127.0.0.1', self.port))
        req = {'id': 1, 'cmd': name}; req.update(args)
        s.sendall((json.dumps(req) + '\n').encode())
        buf = b''
        while b'\n' not in buf:
            ch = s.recv(65536)
            if not ch: break
            buf += ch
        s.close()
        return json.loads(buf.decode('utf-8', 'replace').strip())
    def read_mem(self, addr, size):
        r = self.cmd('read_memory', addr=hex(addr), size=size)
        return bytes.fromhex(r['hex'])
    def read_vram(self, addr, size):
        r = self.cmd('read_vram', addr=hex(addr), size=size)
        return bytes.fromhex(r['hex'])

def w16(b, off):  # big-endian unsigned word
    return (b[off] << 8) | b[off+1]
def s16(b, off):
    v = w16(b, off); return v - 0x10000 if v >= 0x8000 else v
def u32(b, off):
    return (b[off]<<24)|(b[off+1]<<16)|(b[off+2]<<8)|b[off+3]

def slot_summary(raw):
    """Decode the fields that matter for the monkey state machine."""
    return {
        "code":   "$%06X" % u32(raw, 0x00),
        "routine": raw[0x05],
        "mapframe": raw[0x22],          # mapping_frame
        "anim_frame": raw[0x23],
        "anim_timer": raw[0x24],
        "render": "0x%02X" % raw[0x04],
        "x":      w16(raw, 0x10),
        "y":      w16(raw, 0x14),
        "xvel":   s16(raw, 0x18),
        "yvel":   s16(raw, 0x1A),
        "art_tile": "0x%04X" % w16(raw, 0x0A),
        "timer_2E": w16(raw, 0x2E),
        "subtype": raw[0x2C],
        "f30":    "$%06X" % u32(raw, 0x30),   # anim script ptr
        "f34":    "$%06X" % u32(raw, 0x34),   # WaitOffscreen resume ptr
        "f38":    "0x%02X" % raw[0x38],       # flag bits used by swing logic
        "f39":    raw[0x39],
        "f3A":    raw[0x3A],
        "f3B":    raw[0x3B],
        "f3C":    raw[0x3C],                  # swing angle
        "f3E":    "0x%04X" % w16(raw, 0x3E),  # parent/anchor ptr
        "f40":    s16(raw, 0x40),             # swing direction
        "f44":    "0x%04X" % w16(raw, 0x44),
        "f46_parent3": "0x%04X" % w16(raw, 0x46),
        "f48_respawn": "0x%04X" % w16(raw, 0x48),
    }

def dump(c):
    out = {"rng_seed": "0x%04X" % w16(c.read_mem(RNG_SEED, 2), 0)}
    st = c.cmd('sonic_state')
    out["sonic"] = {k: st[k] for k in ("x","y","xvel","game_mode","internal_frame","camera_x")}
    slots = {}
    for i in range(OBJ_COUNT):
        raw = c.read_mem(OBJ_BASE + i*OBJ_SIZE, OBJ_SIZE)
        if u32(raw, 0) == 0:
            continue
        s = slot_summary(raw)
        codeval = u32(raw, 0)
        s["tag"] = MONKEY_CODES.get(codeval, "")
        slots[i] = s
    out["slots"] = slots
    return out

def find_sprites_for_tile(c, tile_lo=0x548, tile_hi=0x54F):
    """Walk the VDP sprite table; return entries whose tile index is in
    the MonkeyDude art range ($548..). Each entry: Y, link, size/flip, tile, X."""
    tbl = c.read_vram(SPRITE_TBL, 640)
    found = []
    idx = 0
    seen = 0
    while seen < 80:
        off = idx*8
        y = w16(tbl, off)
        size_link = tbl[off+2]; link = tbl[off+3]
        tile_attr = w16(tbl, off+4)
        tile = tile_attr & 0x7FF
        x = w16(tbl, off+6)
        if tile_lo <= tile <= tile_hi or (0x548 <= tile <= 0x570):
            found.append({"sprite": idx, "y": y, "x": x, "tile": "0x%03X" % tile,
                          "link": link, "size": "0x%02X" % size_link})
        seen += 1
        if link == 0: break
        idx = link
    return found

def drive(c, target_x, timeout=12.0):
    c.cmd('set_input', keys="08")          # hold Right (bit 3)
    t0 = time.time(); last = None
    while time.time() - t0 < timeout:
        x = c.cmd('sonic_state')['x']
        last = x
        if x >= target_x:
            break
        time.sleep(0.008)
    c.cmd('set_input', keys="00")          # release
    # let friction settle
    t1 = time.time()
    while time.time() - t1 < 1.5:
        st = c.cmd('sonic_state')
        if st['xvel'] == 0:
            break
        time.sleep(0.008)
    return last

def main():
    port = int(sys.argv[1])
    action = sys.argv[2] if len(sys.argv) > 2 else "dump"
    c = Conn(port)
    if action == "drive":
        target = int(sys.argv[3])
        reached = drive(c, target)
        d = dump(c)
        d["drive_reached_x"] = reached
        d["monkey_sprites"] = find_sprites_for_tile(c)
        print(json.dumps(d, indent=1))
    elif action == "dump":
        d = dump(c)
        d["monkey_sprites"] = find_sprites_for_tile(c)
        print(json.dumps(d, indent=1))
    elif action == "sample":
        slot = int(sys.argv[3]); n = int(sys.argv[4])
        rows = []
        for _ in range(n):
            raw = c.read_mem(OBJ_BASE + slot*OBJ_SIZE, OBJ_SIZE)
            s = slot_summary(raw)
            sp = find_sprites_for_tile(c)
            rows.append({"y": s["y"], "f3C": s["f3C"], "routine": s["routine"],
                         "mapframe": s["mapframe"], "code": s["code"],
                         "nsprites": len(sp),
                         "sprite_ys": [e["y"] for e in sp]})
            time.sleep(0.016)
        print(json.dumps(rows, indent=1))

if __name__ == "__main__":
    main()
