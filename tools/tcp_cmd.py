#!/usr/bin/env python3
"""Send a single TCP command and print response. Usage: tcp_cmd.py <json>"""
import socket, json, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3)
s.connect(('127.0.0.1', 4378))
cmd = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {'id':1,'cmd':'ping'}
s.sendall((json.dumps(cmd) + '\n').encode())
data = b''
while True:
    try:
        chunk = s.recv(8192)
        if not chunk: break
        data += chunk
        if b'\n' in data: break
    except: break
s.close()
print(data.decode().strip())
