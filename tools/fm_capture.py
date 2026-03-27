#!/usr/bin/env python3
"""
fm_capture.py — Control FM trace via TCP debug server.

Usage:
    python fm_capture.py on [frames]   # start capture (default 300 frames)
    python fm_capture.py off           # stop capture
    python fm_capture.py               # check status
"""
import socket, sys, json

HOST, PORT = '127.0.0.1', 4378

def send_cmd(obj):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    try:
        s.connect((HOST, PORT))
        s.sendall((json.dumps(obj) + "\n").encode())
        data = s.recv(4096).decode().strip()
        print(data)
    except Exception as e:
        print(f"Error: {e}")
    finally:
        s.close()

args = sys.argv[1:]
if not args:
    send_cmd({"id": 1, "cmd": "fm_trace"})
elif args[0] == "on":
    frames = int(args[1]) if len(args) > 1 else 300
    send_cmd({"id": 1, "cmd": "fm_trace", "action": "on", "frames": frames})
elif args[0] == "off":
    send_cmd({"id": 1, "cmd": "fm_trace", "action": "off"})
