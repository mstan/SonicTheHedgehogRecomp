#!/usr/bin/env python3
"""Test jump: brute-force enter level, then press A."""
import socket, json, time

class G:
    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", 4378), timeout=30)
        self.buf = b""
    def cmd(self, d):
        d.setdefault("id", 1)
        self.sock.sendall((json.dumps(d) + "\n").encode())
        while b"\n" not in self.buf:
            self.buf += self.sock.recv(65536)
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

g = G()
print("Connected:", g.cmd({"cmd": "ping"}))

# Repeatedly press Start until we're in level mode (0x0C = 12)
for attempt in range(20):
    s = g.cmd({"cmd": "sonic_state"})
    mode = s["game_mode"]
    if mode == 12:
        print(f"IN LEVEL! (attempt {attempt})")
        break
    # Pulse Start
    g.cmd({"cmd": "set_input", "keys": "0x80"})
    g.cmd({"cmd": "run_frames", "count": 2})
    g.cmd({"cmd": "set_input", "keys": "0x00"})
    g.cmd({"cmd": "run_frames", "count": 120})  # Wait ~2 seconds of game time
    s2 = g.cmd({"cmd": "sonic_state"})
    print(f"  attempt {attempt}: mode={s2['game_mode']} obj={s2['obj_id']}")
else:
    print("Failed to enter level mode!")
    g.cmd({"cmd": "quit"})
    g.sock.close()
    exit(1)

# Now in level. Wait a moment for Sonic to be stable.
g.cmd({"cmd": "run_frames", "count": 30})
s = g.cmd({"cmd": "sonic_state"})
print(f"Sonic: y={s['y']} yvel={s['yvel']} routine={s['routine']} status={s['status']}")

# Note frame before jump
info = g.cmd({"cmd": "frame_info"})
pre = info["current_frame"]
print(f"\nPre-jump frame: {pre}")

# Clear joypad, press A+Right
g.cmd({"cmd": "set_input", "keys": "0x00"})
g.cmd({"cmd": "run_frames", "count": 3})
g.cmd({"cmd": "set_input", "keys": "0x48"})  # A+Right
g.cmd({"cmd": "run_frames", "count": 15})
g.cmd({"cmd": "set_input", "keys": "0x08"})  # Release A, keep Right
g.cmd({"cmd": "run_frames", "count": 50})

info2 = g.cmd({"cmd": "frame_info"})
post = info2["current_frame"]
print(f"Post-jump frame: {post}")

# Pull sonic_history
hist = g.cmd({"cmd": "sonic_history", "start": pre - 2, "end": min(post - 1, pre + 75)})
print(f"\nFrame-by-frame:")
found_jump = False
for f in hist.get("frames", []):
    yv = f.get("yvel", 0)
    st = f.get("status", 0)
    if yv < -100:
        found_jump = True
    mark = " <<< JUMP!" if yv < -100 else (" *air*" if (st & 2) else "")
    if yv != 0 or (st & 2) or f["frame"] <= pre + 20:
        print(f"  F{f['frame']}: y={f['y']} yvel={yv} st={st} "
              f"joy={f.get('joy_held',0)}/{f.get('joy_press',0)}{mark}")

if found_jump:
    print("\n*** JUMP WORKS! ***")
else:
    print("\n*** NO JUMP - yvel never went significantly negative ***")

g.cmd({"cmd": "quit"})
g.sock.close()
