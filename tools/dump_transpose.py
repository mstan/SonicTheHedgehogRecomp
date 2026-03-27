#!/usr/bin/env python3
"""Dump transpose + detune for all SMPS tracks via TCP."""
import socket, json, sys

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(3)
s.connect(('127.0.0.1', 4378))
s.sendall((json.dumps({'id':1,'cmd':'read_ram','addr':'0xF040','size':448}) + '\n').encode())
data = b''
while True:
    try:
        chunk = s.recv(8192)
        if not chunk: break
        data += chunk
        if b'\n' in data: break
    except: break
s.close()
resp = json.loads(data.decode().strip())
raw = bytes.fromhex(resp.get('hex',''))

tracks = ['DAC','FM1','FM2','FM3','FM4','FM5','PSG1','PSG2','PSG3']
for i, name in enumerate(tracks):
    off = i * 0x30
    if off + 0x30 > len(raw): break
    t = raw[off:off+0x30]
    flags = t[0]
    trans = t[8]
    ts = trans if trans < 128 else trans - 256
    vol = t[9]
    freq = (t[0x10]<<8)|t[0x11]
    det = t[0x1E]
    ds = det if det < 128 else det - 256
    playing = 'PLAY' if flags & 0x80 else 'stop'
    print(f'  {name:>4}: {playing} trans={trans:3d}({ts:+4d}) vol={vol:3d} freq=${freq:04X} det={det:3d}({ds:+4d})')
