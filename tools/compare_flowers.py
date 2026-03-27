#!/usr/bin/env python3
"""Compare flower tiles at exact same game state."""
import socket, json, time, subprocess, sys, os

SONIC_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(SONIC_DIR, "build", "Release", "SonicTheHedgehogRecomp.exe")
ROM = os.path.join(SONIC_DIR, "sonic.bin")

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

def capture(label):
    g = G()
    # Enter GHZ
    for _ in range(15):
        s = g.cmd({"cmd": "sonic_state"})
        if s["game_mode"] == 12: break
        g.cmd({"cmd": "set_input", "keys": "0x80"})
        g.cmd({"cmd": "run_frames", "count": 2})
        g.cmd({"cmd": "set_input", "keys": "0x00"})
        g.cmd({"cmd": "run_frames", "count": 120})

    # Run exactly 60 frames, then capture on multiple subsequent frames
    # to get all animation phases
    g.cmd({"cmd": "run_frames", "count": 60})

    print(f"\n[{label}] Flower tiles at 5 consecutive frames:")
    for frame in range(5):
        g.cmd({"cmd": "run_frames", "count": 1})
        s = g.cmd({"cmd": "sonic_state"})
        t861 = g.cmd({"cmd": "read_vram", "addr": "0x6BA0", "size": 32})
        t876 = g.cmd({"cmd": "read_vram", "addr": "0x6D80", "size": 32})
        print(f"  +{frame}: t861={t861['hex'][:32]}... t876={t876['hex'][:32]}...")

    # Dump full flower range
    g.cmd({"cmd": "dump_vram", "path": f"vram_flowers_{label}.bin", "offset": "0x6B80", "size": "0x500"})
    g.cmd({"cmd": "quit"})
    g.sock.close()

capture(sys.argv[1] if len(sys.argv) > 1 else "test")
