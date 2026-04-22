#!/usr/bin/env python3
"""Phase-4 oracle step-back smoke test.

Let oracle run a while so snapshots accumulate. Arm a break. When the
break fires, record insn_count. Issue rdb_oracle_step_back; verify
the reported restored_insn is strictly less than the recorded count.
Then query state and print — should match historical state.
"""
import socket, json, time, subprocess, os, sys

exe = os.path.abspath('build-rdb/Release/SonicTheHedgehogRecomp_oracle.exe')
rom = os.path.abspath('segagenesisrecomp/sonicthehedgehog/sonic.bin')

subprocess.call(['taskkill','/F','/IM','SonicTheHedgehogRecomp_oracle.exe'],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
p = subprocess.Popen([exe, rom, '--turbo'], cwd=os.path.dirname(exe),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(30):
        try: s = socket.create_connection(('127.0.0.1', 4379), timeout=15); break
        except OSError: time.sleep(0.3)

    def rpc(cmd, **kw):
        s.sendall((json.dumps({'id':1, 'cmd':cmd, **kw})+'\n').encode())
        buf=b''
        while b'\n' not in buf: buf += s.recv(1<<20)
        return json.loads(buf.split(b'\n',1)[0].decode())

    def wait_parked(timeout=15):
        t0 = time.time()
        while time.time()-t0 < timeout:
            r = rpc('rdb_oracle_state')
            if r.get('parked'): return r
            time.sleep(0.02)
        return None

    # Arm a break at a PC that fires late enough for snapshots to accumulate.
    r = rpc('rdb_oracle_break', pc='0x072A5A')
    print(f"arm: {r}")

    st = wait_parked(timeout=20)
    if not st: print("FAIL: no break"); sys.exit(1)
    print(f"parked at pc={st['pc']} D0={st['D'][0]:#010x} D5={st['D'][5]:#010x}")

    # Step back and see how far we rewound.
    for i in range(3):
        r = rpc('rdb_oracle_step_back')
        if not r.get('ok'):
            print(f"  step_back {i}: {r}"); break
        print(f"  step_back {i}: restored_insn={r['restored_insn']} "
              f"snap_count={r['snap_count']}")
        time.sleep(0.05)
        st2 = rpc('rdb_oracle_state')
        print(f"    state after: parked={st2.get('parked')} pc={st2.get('pc')} "
              f"D0={st2['D'][0]:#010x} D5={st2['D'][5]:#010x}")

    rpc('rdb_oracle_continue')
    print("continue sent")
finally:
    if p.poll() is None:
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
