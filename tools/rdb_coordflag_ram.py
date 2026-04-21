#!/usr/bin/env python3
"""One-hit CoordFlag snapshot with sound-shadow RAM window."""
import socket, json, time, subprocess, os, sys

exe = os.path.abspath('build-rdb/Release/SonicTheHedgehogRecomp.exe')
rom = os.path.abspath('segagenesisrecomp/sonicthehedgehog/sonic.bin')
subprocess.call(['taskkill','/F','/IM','SonicTheHedgehogRecomp.exe'],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
p = subprocess.Popen([exe, rom, '--turbo'], cwd=os.path.dirname(exe),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(30):
        try: s = socket.create_connection(('127.0.0.1',4378), timeout=15); break
        except OSError: time.sleep(0.3)
    def rpc(c, **kw):
        s.sendall((json.dumps({'id':1,'cmd':c,**kw})+'\n').encode())
        b=b''
        while b'\n' not in b: b += s.recv(65536)
        return json.loads(b.split(b'\n',1)[0].decode())
    print(rpc('rdb_break', block='0x072A5A'))
    # Wait park.
    for _ in range(100):
        st = rpc('rdb_get_state', ram_lo='0x1000', ram_hi='0x1040')
        if st.get('parked'): break
        time.sleep(0.05)
    else:
        print("no park"); sys.exit(1)
    print(f"frame={st['frame']} block={st['block']} D5={st['D'][5]:#x}")
    print(f"SR={st['SR']} stack={st['stack']}")
    ram = st.get('ram', '')
    if ram:
        print(f"ram $FF{int(st['ram_lo'],16)&0xFFFF:04X}-$FF{int(st['ram_hi'],16)&0xFFFF:04X}:")
        # Dump as space-separated bytes.
        for i in range(0, len(ram), 32):
            addr = 0xFF1000 + i//2
            print(f"  ${addr:06X}: {' '.join(ram[i:i+32][j:j+2] for j in range(0,32,2))}")
    else:
        print(f"ram not present in response (keys: {list(st.keys())})")
finally:
    if p.poll() is None:
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
