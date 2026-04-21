#!/usr/bin/env python3
"""Probe Tier-2 break mechanism: arm + check diagnostic counters."""
import socket, json, time, subprocess, sys, os

exe = os.path.abspath('build-rdb/Release/SonicTheHedgehogRecomp.exe')
rom = os.path.abspath('segagenesisrecomp/sonicthehedgehog/sonic.bin')
subprocess.call(['taskkill', '/F', '/IM', 'SonicTheHedgehogRecomp.exe'],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
p = subprocess.Popen([exe, rom, '--turbo'],
                     cwd=os.path.dirname(exe),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(20):
        try:
            s = socket.create_connection(('127.0.0.1', 4378), timeout=10)
            break
        except OSError: time.sleep(0.3)
    else:
        raise RuntimeError("no connect")

    def rpc(cmd, **kw):
        s.sendall((json.dumps({'id': 1, 'cmd': cmd, **kw}) + '\n').encode())
        buf = b''
        while b'\n' not in buf: buf += s.recv(65536)
        return json.loads(buf.split(b'\n', 1)[0].decode())

    # Baseline without break (nothing armed → slow_count should stay 0).
    r0 = rpc('rdb_break_list')
    print("before:", r0)
    time.sleep(0.5)
    r0b = rpc('rdb_break_list')
    print("before +0.5s:", r0b)

    # Arm a breakpoint at a very early boot block (SkipSecurity). By
    # the time we connect + arm, it's already past — so this shouldn't
    # park. But g_rdb_break_pending should now be 1 and slow_count
    # should grow.
    r1 = rpc('rdb_break', block='0x000B10')
    print("arm:", r1)
    time.sleep(0.5)
    r2 = rpc('rdb_break_list')
    print("after arm +0.5s:", r2)
    time.sleep(0.5)
    r3 = rpc('rdb_break_list')
    print("after arm +1.0s:", r3)
finally:
    if p.poll() is None:
        p.terminate()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill()
