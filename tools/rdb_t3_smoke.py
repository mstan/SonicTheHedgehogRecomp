#!/usr/bin/env python3
"""Tier 3 smoke: arm a narrow PC range on oracle, let run briefly,
dump. Verify entries have PC in range + non-zero CPU state."""
import socket, json, time, subprocess, os, sys

exe = os.path.abspath('build-rdb/Release/SonicTheHedgehogRecomp_oracle.exe')
rom = os.path.abspath('segagenesisrecomp/sonicthehedgehog/sonic.bin')
subprocess.call(['taskkill','/F','/IM','SonicTheHedgehogRecomp_oracle.exe'],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
p = subprocess.Popen([exe, rom, '--turbo'], cwd=os.path.dirname(exe),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(40):
        try: s = socket.create_connection(('127.0.0.1', 4379), timeout=20); break
        except OSError: time.sleep(0.3)
    else:
        print("no connect"); sys.exit(1)
    def rpc(c, **kw):
        s.sendall((json.dumps({'id':1,'cmd':c,**kw})+'\n').encode())
        b=b''
        while b'\n' not in b: b += s.recv(1<<22)
        return json.loads(b.split(b'\n',1)[0].decode())

    print('reset:', rpc('t3_reset'))
    # Narrow range: audio driver area. At 50 fps-ish oracle, SMPS runs
    # per vblank, so we get many hits even in a short window.
    print('range:', rpc('t3_range', lo='0x072A00', hi='0x072FFF'))
    time.sleep(0.8)
    d = rpc('t3_dump', start=0, count=50)
    print(f"total={d['total']}  returned={d['returned']}  done={d['done']}")
    print(f"ranges={d['ranges']}")
    for e in d['log'][:10]:
        print(f"  f={e['f']} pc={e['pc']} sr={e['sr']} "
              f"D0={e['D'][0]:#010x} D5={e['D'][5]:#010x} "
              f"A0={e['A'][0]:#010x} A5={e['A'][5]:#010x}")
finally:
    if p.poll() is None:
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
