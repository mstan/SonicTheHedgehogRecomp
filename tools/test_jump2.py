#!/usr/bin/env python3
"""Detailed frame-by-frame jump observation."""
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

# Get to level
g.cmd({"cmd": "set_input", "keys": "0x80"})
g.cmd({"cmd": "run_frames", "count": 5})
g.cmd({"cmd": "set_input", "keys": "0x00"})
g.cmd({"cmd": "run_frames", "count": 700})
s = g.cmd({"cmd": "sonic_state"})
print(f"Mode={s['game_mode']} obj={s['obj_id']}")

# Note frame before pressing A
info = g.cmd({"cmd": "frame_info"})
pre_frame = info["current_frame"]
print(f"Pre-jump frame: {pre_frame}")

# Clear then press A+Right
g.cmd({"cmd": "set_input", "keys": "0x00"})
g.cmd({"cmd": "run_frames", "count": 3})
g.cmd({"cmd": "set_input", "keys": "0x48"})
g.cmd({"cmd": "run_frames", "count": 10})
g.cmd({"cmd": "set_input", "keys": "0x00"})
g.cmd({"cmd": "run_frames", "count": 50})

info2 = g.cmd({"cmd": "frame_info"})
post_frame = info2["current_frame"]
print(f"Post-jump frame: {post_frame}")

# Query full history
end = min(post_frame - 1, pre_frame + 70)
hist = g.cmd({"cmd": "sonic_history", "start": pre_frame - 2, "end": end})
print(f"\nFrame-by-frame ({pre_frame-2} to {end}):")
for f in hist.get("frames", []):
    yv = f.get("yvel", 0)
    st = f.get("status", 0)
    mark = " <<< JUMP!" if yv < -100 else (" *air*" if (st & 2) else "")
    if yv != 0 or (st & 2) or f["frame"] <= pre_frame + 15:
        print(f"  F{f['frame']}: y={f['y']} yvel={yv} st={st} "
              f"joy={f.get('joy_held',0)}/{f.get('joy_press',0)}{mark}")

g.cmd({"cmd": "quit"})
g.sock.close()
