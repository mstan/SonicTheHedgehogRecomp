#!/usr/bin/env python3
"""
dump_sound_state.py — Dump SMPS sound driver state from running instance.

Sonic 1 SMPS track state layout:
  $F000-$F07F: FM track 1 (DAC)
  $F080-$F0FF: FM track 2
  $F100-$F17F: FM track 3
  ...etc (6 FM + 3 PSG = 9 tracks, 0x30 bytes each)

Key offsets within each track:
  +$00: flags byte (bit 7 = playing, bit 1 = rest, bit 2 = overridden)
  +$04: data pointer (longword)
  +$08: transposition
  +$0A: volume
  +$10: frequency (word) — THE KEY VALUE
  +$1E: detune
"""
import socket, json, sys

HOST, PORT = '127.0.0.1', 4378

def send_cmd(obj):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    s.connect((HOST, PORT))
    s.sendall((json.dumps(obj) + "\n").encode())
    data = b""
    while True:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
            if b"\n" in data:
                break
        except:
            break
    s.close()
    return json.loads(data.decode().strip())

# Read sound driver state: $FFF000-$FFF200
result = send_cmd({"id": 1, "cmd": "read_ram", "addr": "0xF000", "size": 512})

if "error" in result:
    print(f"Error: {result['error']}")
    sys.exit(1)

hex_data = result.get("hex", "")
data = bytes.fromhex(hex_data)

track_names = ["DAC", "FM1", "FM2", "FM3", "FM4", "FM5", "PSG1", "PSG2", "PSG3"]
track_size = 0x30

print(f"SMPS Sound Driver State ({len(data)} bytes from $FFF000)")
print("=" * 70)

for i, name in enumerate(track_names):
    offset = i * track_size
    if offset + track_size > len(data):
        break
    track = data[offset:offset + track_size]

    flags = track[0]
    playing = bool(flags & 0x80)
    rest = bool(flags & 0x02)
    data_ptr = int.from_bytes(track[4:8], 'big')
    transpose = track[8]
    volume = track[0x0A]
    freq = int.from_bytes(track[0x10:0x12], 'big')
    detune = track[0x1E]

    status = "PLAYING" if playing else "stopped"
    if rest:
        status += "+rest"

    print(f"  {name:>4}: {status:<15} ptr=${data_ptr:08X} trans={transpose:3d} vol={volume:3d} freq=${freq:04X} det={detune:3d}")

    # Hex dump of full track
    hex_line = " ".join(f"{b:02X}" for b in track)
    print(f"        {hex_line}")
    print()
