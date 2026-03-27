#!/usr/bin/env python3
"""
Test jump in native Step 2 build.
Uses a persistent connection to avoid timing issues between commands.
"""
import socket, json, time, sys

class GameConn:
    def __init__(self, host="127.0.0.1", port=4378):
        self.sock = socket.create_connection((host, port), timeout=30)
        self.buf = b""

    def cmd(self, d):
        d.setdefault("id", 1)
        self.sock.sendall((json.dumps(d) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("disconnected")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line.decode())

    def close(self):
        self.sock.close()

g = GameConn()

print("Connected:", g.cmd({"cmd": "ping"}))

# Step 1: Get to gameplay - press Start
print("\n--- Pressing Start ---")
g.cmd({"cmd": "set_input", "keys": "0x80"})
g.cmd({"cmd": "run_frames", "count": 5})
g.cmd({"cmd": "set_input", "keys": "0x00"})
g.cmd({"cmd": "run_frames", "count": 700})

state = g.cmd({"cmd": "sonic_state"})
print(f"After 700 frames: mode={state['game_mode']} routine={state['routine']} obj_id={state['obj_id']}")

if state["game_mode"] not in (8, 12):
    print("Not in gameplay yet, running more frames...")
    g.cmd({"cmd": "run_frames", "count": 300})
    state = g.cmd({"cmd": "sonic_state"})
    print(f"After 1000 frames: mode={state['game_mode']}")

# Step 2: Enable IO logging
g.cmd({"cmd": "io_log", "enable": 1})

# Step 3: Ensure clean joypad state, then press A+Right
print("\n--- Clearing joypad, then pressing A+Right (0x48) ---")
g.cmd({"cmd": "set_input", "keys": "0x00"})
g.cmd({"cmd": "run_frames", "count": 3})  # Let ReadJoypads see "no buttons" for a few frames

g.cmd({"cmd": "set_input", "keys": "0x48"})  # NOW press A+Right

# Run 1 frame at a time to observe each frame
for i in range(5):
    g.cmd({"cmd": "run_frames", "count": 1})
    s = g.cmd({"cmd": "sonic_state"})
    jp = g.cmd({"cmd": "read_joypad_port"})
    ram = g.cmd({"cmd": "read_ram", "addr": "0xF602", "size": 4})
    # F602=duplicate held, F603=duplicate press, F604=primary held, F605=primary press
    hex_bytes = ram.get("hex", "00000000")
    f602 = int(hex_bytes[0:2], 16)
    f603 = int(hex_bytes[2:4], 16)
    f604 = int(hex_bytes[4:6], 16)
    f605 = int(hex_bytes[6:8], 16)
    print(f"  Frame +{i+1}: y={s['y']} yvel={s['yvel']} status={s['status']} "
          f"F602={f602:02X} F603={f603:02X} F604={f604:02X} F605={f605:02X}")

# Step 4: Release and run more frames to see full arc
g.cmd({"cmd": "set_input", "keys": "0x00"})
g.cmd({"cmd": "run_frames", "count": 60})

# Step 5: Pull sonic_history over the jump region
info = g.cmd({"cmd": "frame_info"})
current = info["current_frame"]
# Look at the last ~80 frames to find the jump
start = max(0, current - 80)
hist = g.cmd({"cmd": "sonic_history", "start": start, "end": current - 1})

print(f"\n--- Sonic history (frames {start}-{current-1}) ---")
if "frames" in hist:
    peak_yvel = 0
    for f in hist["frames"]:
        if f.get("available") == False:
            continue
        yv = f.get("yvel", 0)
        st = f.get("status", 0)
        if yv < -100 or yv > 100 or (st & 2):
            print(f"  F{f['frame']}: y={f['y']} yvel={yv} status={st} "
                  f"joy={f.get('joy_held',0)}/{f.get('joy_press',0)}")
            if yv < peak_yvel:
                peak_yvel = yv
    print(f"\n  Peak yvel: {peak_yvel}")
    if peak_yvel == 0:
        print("  NO JUMP DETECTED - yvel never went significantly negative")
    elif peak_yvel > -500:
        print("  PARTIAL - yvel went negative but not jump velocity (-1651)")
    else:
        print("  JUMP DETECTED!")

# Disable logging
g.cmd({"cmd": "io_log", "enable": 0})
g.cmd({"cmd": "quit"})
g.close()
print("\nDone.")
