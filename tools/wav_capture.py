#!/usr/bin/env python3
"""Control WAV capture via TCP. Usage: wav_capture.py start <file> | stop"""
import socket, json, sys

def send(obj):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    s.connect(('127.0.0.1', 4378))
    s.sendall((json.dumps(obj) + '\n').encode())
    data = b''
    while True:
        try:
            chunk = s.recv(4096)
            if not chunk: break
            data += chunk
            if b'\n' in data: break
        except: break
    s.close()
    print(data.decode().strip())

if len(sys.argv) < 2:
    print("Usage: wav_capture.py start <file> | stop")
    sys.exit(1)

if sys.argv[1] == 'start':
    f = sys.argv[2] if len(sys.argv) > 2 else 'capture.wav'
    send({'id': 1, 'cmd': 'audio_wav', 'action': 'start', 'path': f})
elif sys.argv[1] == 'stop':
    send({'id': 1, 'cmd': 'audio_wav', 'action': 'stop'})
