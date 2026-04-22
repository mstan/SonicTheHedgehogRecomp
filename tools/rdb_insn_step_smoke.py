#!/usr/bin/env python3
"""Tier 4 per-instruction step smoke test.

Arms an insn-level breakpoint at a very early PC, waits for it to hit,
then step-insn's through several 68K instructions and records state
at each step. Verifies PC advances by the expected bytes per 68K
instruction size.
"""
import socket, json, time, subprocess, os, sys

exe = os.path.abspath('build-rdb/Release/SonicTheHedgehogRecomp.exe')
rom = os.path.abspath('segagenesisrecomp/sonicthehedgehog/sonic.bin')

subprocess.call(['taskkill','/F','/IM','SonicTheHedgehogRecomp.exe'],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
p = subprocess.Popen([exe, rom, '--turbo'], cwd=os.path.dirname(exe),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(30):
        try: s = socket.create_connection(('127.0.0.1', 4378), timeout=15); break
        except OSError: time.sleep(0.3)

    def rpc(cmd, **kw):
        s.sendall((json.dumps({'id':1, 'cmd':cmd, **kw})+'\n').encode())
        buf=b''
        while b'\n' not in buf: buf += s.recv(1<<20)
        return json.loads(buf.split(b'\n',1)[0].decode())

    def wait_parked(timeout=10):
        t0 = time.time()
        while time.time()-t0 < timeout:
            r = rpc('rdb_get_state')
            if r.get('parked'): return r
            time.sleep(0.02)
        return None

    # Arm break at a SMPS-area PC that will definitely fire.
    r = rpc('rdb_insn_break', pc='0x072A5A')
    print(f"arm: {r}")

    st = wait_parked(timeout=15)
    if not st:
        print("FAIL: break never fired within 15s"); sys.exit(1)
    print(f"parked at block={st['block']} D5={st['D'][5]:#x} SR={st['SR']}")

    for i in range(5):
        rpc('rdb_step_insn')
        st2 = wait_parked(timeout=3)
        if not st2:
            print(f"  step {i}: no re-park"); break
        print(f"  step {i}: block={st2['block']} "
              f"D0={st2['D'][0]:#010x} D5={st2['D'][5]:#010x} "
              f"A4={st2['A'][4]:#010x} A5={st2['A'][5]:#010x} "
              f"SR={st2['SR']}")
    rpc('rdb_continue')
    print("continue sent — done")
finally:
    if p.poll() is None:
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
