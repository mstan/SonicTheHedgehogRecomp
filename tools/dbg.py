#!/usr/bin/env python3
"""
dbg.py - CLI debug client for the Sonic 1 recomp TCP debug server.

Usage:
    python dbg.py ping
    python dbg.py get_registers
    python dbg.py read_memory 0xFF0000 64
    python dbg.py read_ram 0xD000 64
    python dbg.py sonic_state
    python dbg.py sonic_history 100 200
    python dbg.py object_table
    python dbg.py vblank_info
    python dbg.py frame_info
    python dbg.py frame_range 100 200
    python dbg.py watch 0xD012
    python dbg.py unwatch 0xD012
    python dbg.py run_frames 60
    python dbg.py set_input 0x40          # press A
    python dbg.py write_memory 0xFF0000 DEADBEEF
    python dbg.py quit
"""

import json
import socket
import sys
import time

PORT = 4378
HOST = "127.0.0.1"

_cmd_id = 0

def send_cmd(cmd_dict):
    global _cmd_id
    _cmd_id += 1
    cmd_dict["id"] = _cmd_id

    s = socket.socket()
    s.settimeout(5)
    try:
        s.connect((HOST, PORT))
    except ConnectionRefusedError:
        return '{"error": "connection refused -- is SonicTheHedgehogRecomp.exe running?"}'

    msg = json.dumps(cmd_dict) + "\n"
    s.sendall(msg.encode())

    # Wait a bit for response, then read
    time.sleep(0.1)
    chunks = []
    while True:
        try:
            chunk = s.recv(65536)
            if not chunk:
                break
            chunks.append(chunk)
            if b'\n' in chunk:
                break
        except socket.timeout:
            break

    s.close()
    data = b''.join(chunks).decode().strip()
    return data


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    cmd = sys.argv[1]

    if cmd == "ping":
        result = send_cmd({"cmd": "ping"})
    elif cmd == "get_registers":
        result = send_cmd({"cmd": "get_registers"})
    elif cmd == "read_memory":
        addr = sys.argv[2] if len(sys.argv) > 2 else "0xFF0000"
        size = int(sys.argv[3]) if len(sys.argv) > 3 else 16
        result = send_cmd({"cmd": "read_memory", "addr": addr, "size": size})
    elif cmd == "read_ram":
        addr = sys.argv[2] if len(sys.argv) > 2 else "0xD000"
        size = int(sys.argv[3]) if len(sys.argv) > 3 else 16
        result = send_cmd({"cmd": "read_ram", "addr": addr, "size": size})
    elif cmd == "write_memory":
        addr = sys.argv[2] if len(sys.argv) > 2 else "0xFF0000"
        hex_data = sys.argv[3] if len(sys.argv) > 3 else "00"
        result = send_cmd({"cmd": "write_memory", "addr": addr, "hex": hex_data})
    elif cmd == "sonic_state":
        result = send_cmd({"cmd": "sonic_state"})
    elif cmd == "sonic_history":
        start = int(sys.argv[2]) if len(sys.argv) > 2 else 0
        end = int(sys.argv[3]) if len(sys.argv) > 3 else start + 59
        result = send_cmd({"cmd": "sonic_history", "start": start, "end": end})
    elif cmd == "object_table":
        count = int(sys.argv[2]) if len(sys.argv) > 2 else 64
        result = send_cmd({"cmd": "object_table", "count": count})
    elif cmd == "vblank_info":
        result = send_cmd({"cmd": "vblank_info"})
    elif cmd == "frame_info":
        result = send_cmd({"cmd": "frame_info"})
    elif cmd == "frame_range":
        start = int(sys.argv[2]) if len(sys.argv) > 2 else 0
        end = int(sys.argv[3]) if len(sys.argv) > 3 else start + 59
        result = send_cmd({"cmd": "frame_range", "start": start, "end": end})
    elif cmd == "watch":
        addr = sys.argv[2] if len(sys.argv) > 2 else "0xD012"
        result = send_cmd({"cmd": "watch", "addr": addr})
    elif cmd == "unwatch":
        addr = sys.argv[2] if len(sys.argv) > 2 else "0xD012"
        result = send_cmd({"cmd": "unwatch", "addr": addr})
    elif cmd == "run_frames":
        count = int(sys.argv[2]) if len(sys.argv) > 2 else 60
        result = send_cmd({"cmd": "run_frames", "count": count})
    elif cmd == "set_input":
        keys = sys.argv[2] if len(sys.argv) > 2 else "0"
        result = send_cmd({"cmd": "set_input", "keys": keys})
    elif cmd == "audio_stats":
        result = send_cmd({"cmd": "audio_stats"})
    elif cmd == "audio_wav":
        action = sys.argv[2] if len(sys.argv) > 2 else "status"
        d = {"cmd": "audio_wav", "action": action}
        if action == "start" and len(sys.argv) > 3:
            d["path"] = sys.argv[3]
        result = send_cmd(d)
    elif cmd == "io_log":
        enable = int(sys.argv[2]) if len(sys.argv) > 2 else 1
        result = send_cmd({"cmd": "io_log", "enable": enable})
    elif cmd == "read_joypad_port":
        result = send_cmd({"cmd": "read_joypad_port"})
    elif cmd == "quit":
        result = send_cmd({"cmd": "quit"})
    else:
        print(f"Unknown command: {cmd}")
        print(__doc__)
        sys.exit(1)

    # Pretty-print JSON response
    try:
        parsed = json.loads(result)
        print(json.dumps(parsed, indent=2))
    except (json.JSONDecodeError, TypeError):
        print(result)


if __name__ == "__main__":
    main()
