#!/usr/bin/env python3
"""Compare PLC and tile loading state between two running builds."""
import socket, json, time, sys

class G:
    def __init__(self, port=4378):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30)
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

# Get into GHZ
for attempt in range(15):
    s = g.cmd({"cmd": "sonic_state"})
    if s["game_mode"] == 12:
        break
    g.cmd({"cmd": "set_input", "keys": "0x80"})
    g.cmd({"cmd": "run_frames", "count": 2})
    g.cmd({"cmd": "set_input", "keys": "0x00"})
    g.cmd({"cmd": "run_frames", "count": 120})

s = g.cmd({"cmd": "sonic_state"})
print(f"Mode={s['game_mode']} x={s['x']}")

# Read PLC-related RAM
# v_plc_buffer starts after v_ngfx_buffer ($200 bytes) which is at...
# We need to find the actual RAM offset. v_ngfx_buffer is typically at $FC00-$FDFF
# v_plc_buffer at $FE00-$FE5F
# v_plc_patternsleft at $FE60
plc_ram = g.cmd({"cmd": "read_ram", "addr": "0xF680", "size": 16})
print(f"PLC queue ($F680): {plc_ram.get('hex','')}")

# Check the VRAM destination word in PLC state
# v_plc_buffer+4 is the VRAM dest
plc_buf = g.cmd({"cmd": "read_ram", "addr": "0xFE00", "size": 96})
print(f"PLC buffer ($FE00): {plc_buf.get('hex','')}")

plc_left = g.cmd({"cmd": "read_ram", "addr": "0xFE60", "size": 2})
print(f"PLC patterns left ($FE60): {plc_left.get('hex','')}")

# Run RIGHT for 2 seconds to load flower tiles
g.cmd({"cmd": "set_input", "keys": "0x08"})
g.cmd({"cmd": "run_frames", "count": 120})
g.cmd({"cmd": "set_input", "keys": "0x00"})

s2 = g.cmd({"cmd": "sonic_state"})
print(f"\nAfter running: x={s2['x']}")

plc_left2 = g.cmd({"cmd": "read_ram", "addr": "0xFE60", "size": 2})
print(f"PLC patterns left ($FE60): {plc_left2.get('hex','')}")

# Dump VRAM for the flower tile region
vram_data = g.cmd({"cmd": "read_vram", "addr": "0x6B80", "size": 1024})
print(f"\nFlower VRAM (0x6B80, 1024 bytes): {vram_data.get('hex','')[:80]}...")

# Check specific tile 876 (0x6D80) which was 32/32 bytes different
t876 = g.cmd({"cmd": "read_vram", "addr": "0x6D80", "size": 32})
print(f"Tile 876 (0x6D80): {t876.get('hex','')}")

g.cmd({"cmd": "quit"})
g.sock.close()
