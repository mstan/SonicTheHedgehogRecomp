#!/usr/bin/env python3
"""
Compare VRAM dumps between two builds to find tile differences.
Also captures screenshots for visual comparison.
"""
import socket, json, time, sys, os, struct

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
    def close(self):
        self.sock.close()

def capture_state(label, enter_gameplay=True):
    """Connect, optionally enter gameplay, dump VRAM + CRAM."""
    g = G()
    print(f"[{label}] Connected: frame {g.cmd({'cmd':'ping'})['frame']}")

    if enter_gameplay:
        # Get into GHZ gameplay
        for attempt in range(15):
            s = g.cmd({"cmd": "sonic_state"})
            if s["game_mode"] == 12:
                break
            g.cmd({"cmd": "set_input", "keys": "0x80"})
            g.cmd({"cmd": "run_frames", "count": 2})
            g.cmd({"cmd": "set_input", "keys": "0x00"})
            g.cmd({"cmd": "run_frames", "count": 120})

        s = g.cmd({"cmd": "sonic_state"})
        print(f"[{label}] mode={s['game_mode']} obj={s['obj_id']} x={s['x']} y={s['y']}")

        # Run a few frames to let level fully render (tiles loaded)
        g.cmd({"cmd": "set_input", "keys": "0x08"})  # hold Right
        g.cmd({"cmd": "run_frames", "count": 120})    # run 2 sec into level
        g.cmd({"cmd": "set_input", "keys": "0x00"})

    s = g.cmd({"cmd": "sonic_state"})
    print(f"[{label}] Final: mode={s['game_mode']} x={s['x']} y={s['y']}")

    # Dump VRAM
    vram_path = f"vram_{label}.bin"
    g.cmd({"cmd": "dump_vram", "path": vram_path})
    print(f"[{label}] VRAM dumped to {vram_path}")

    # Get CRAM
    cram = g.cmd({"cmd": "read_cram"})
    cram_hex = cram.get("hex", "")
    with open(f"cram_{label}.bin", "wb") as f:
        for i in range(0, len(cram_hex), 4):
            val = int(cram_hex[i:i+4], 16)
            f.write(struct.pack("<H", val))
    print(f"[{label}] CRAM dumped to cram_{label}.bin")

    g.cmd({"cmd": "quit"})
    g.close()
    return vram_path

def compare_vrams(path_a, path_b, label_a="A", label_b="B"):
    """Compare two VRAM dumps and report differences by tile."""
    with open(path_a, "rb") as f:
        a = f.read()
    with open(path_b, "rb") as f:
        b = f.read()

    if len(a) != len(b):
        print(f"Size mismatch: {label_a}={len(a)}, {label_b}={len(b)}")
        return

    # Genesis tiles: 8x8 pixels, 4bpp = 32 bytes per tile
    TILE_SIZE = 32
    num_tiles = len(a) // TILE_SIZE
    diff_tiles = []

    for t in range(num_tiles):
        off = t * TILE_SIZE
        tile_a = a[off:off+TILE_SIZE]
        tile_b = b[off:off+TILE_SIZE]
        if tile_a != tile_b:
            diff_bytes = sum(1 for x, y in zip(tile_a, tile_b) if x != y)
            diff_tiles.append((t, off, diff_bytes))

    print(f"\nVRAM comparison: {len(a)} bytes, {num_tiles} tiles")
    print(f"Different tiles: {len(diff_tiles)} / {num_tiles}")

    if diff_tiles:
        print(f"\nFirst 30 differing tiles:")
        for t, off, nbytes in diff_tiles[:30]:
            print(f"  Tile {t:4d} (VRAM 0x{off:04X}): {nbytes}/32 bytes differ")

        # Group by VRAM region to identify which art is affected
        print(f"\nDifferences by VRAM region:")
        regions = {}
        for t, off, nbytes in diff_tiles:
            region = (off // 0x2000) * 0x2000
            regions.setdefault(region, []).append(t)
        for region in sorted(regions):
            tiles = regions[region]
            print(f"  0x{region:04X}-0x{region+0x1FFF:04X}: {len(tiles)} tiles differ "
                  f"(tiles {tiles[0]}-{tiles[-1]})")

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "compare":
        compare_vrams("vram_native.bin", "vram_interp.bin", "native", "interp")
    else:
        print("Usage:")
        print("  1. Launch native build, then: python compare_vram.py capture native")
        print("  2. Launch interp build, then: python compare_vram.py capture interp")
        print("  3. Compare: python compare_vram.py compare")
        if len(sys.argv) > 2:
            capture_state(sys.argv[2])
