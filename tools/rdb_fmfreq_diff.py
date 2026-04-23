#!/usr/bin/env python3
"""Step-diff FMUpdateFreq ($071E24) between native and oracle.

Arms a break at $071E24 on each. Each time the break fires, captures the
68K state + memory at (A5) and (A5+30). After N fires on each build,
aligns the snapshot sequences and reports the first call index at which
the two builds see divergent entry state.

The theory: $071E24 is inside FMUpdateFreq which writes freq MSB+LSB
for the current channel via sub_072722. The `btst #2, (a5)` at $071E2C
and `beq locret_71E48` at $071E30 gate whether the frequency writes
happen. If the two builds see different (A5) byte or different D6 at
entry, the write stream diverges.
"""
import socket, json, time, subprocess, os, sys


def rpc(sock, cmd, timeout=30, **kw):
    sock.settimeout(timeout)
    sock.sendall((json.dumps({'id': 1, 'cmd': cmd, **kw}) + '\n').encode())
    buf = b''
    while b'\n' not in buf:
        chunk = sock.recv(1 << 20)
        if not chunk: raise RuntimeError(f"{cmd}: closed")
        buf += chunk
    return json.loads(buf.split(b'\n', 1)[0].decode())


def connect(port, retries=40):
    for _ in range(retries):
        try: return socket.create_connection(('127.0.0.1', port), timeout=15)
        except OSError: time.sleep(0.3)
    raise RuntimeError(f"no connect on {port}")


def read_byte(sock, native, addr):
    if native:
        r = rpc(sock, 'read_memory', addr=f'0x{addr:06X}', count=1)
    else:
        r = rpc(sock, 'read_memory', addr=f'0x{addr:06X}', count=1)
    # response has `data` as hex string
    h = r.get('data') or r.get('bytes') or ''
    return int(h[:2], 16) if h else None


def get_state(sock, native):
    if native:
        r = rpc(sock, 'rdb_get_state')
    else:
        r = rpc(sock, 'rdb_oracle_state')
    return r


def wait_parked(sock, native, timeout=20):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st = get_state(sock, native)
        if st.get('parked'): return st
        time.sleep(0.02)
    return None


def arm(sock, native, pc):
    if native:
        return rpc(sock, 'rdb_break', block=f'0x{pc:06X}')
    else:
        return rpc(sock, 'rdb_oracle_break', pc=f'0x{pc:06X}')


def continue_(sock, native):
    if native:
        rpc(sock, 'rdb_continue')
    else:
        rpc(sock, 'rdb_oracle_continue')


def snapshot(sock, native):
    st = get_state(sock, native)
    A5 = st['A'][5]
    D6 = st['D'][6] & 0xFFFF
    D0 = st['D'][0] & 0xFFFF
    # Read bytes at A5 (flags) and A5+30 (freq-transpose offset)
    try: b_a5  = read_byte(sock, native, A5)
    except Exception: b_a5 = None
    try: b_a5_30 = read_byte(sock, native, (A5 + 30) & 0xFFFFFF)
    except Exception: b_a5_30 = None
    pc_val = st.get('pc') or st.get('block') or st.get('func')
    return {
        'pc': pc_val, 'A5': A5, 'D0': D0, 'D1': st['D'][1] & 0xFFFF,
        'D6': D6, 'byte_A5': b_a5, 'byte_A5_30': b_a5_30,
    }


def collect(sock, native, pc, n):
    arm(sock, native, pc)
    snaps = []
    for i in range(n):
        st = wait_parked(sock, native, timeout=25)
        if st is None:
            print(f"  {'N' if native else 'O'} snap {i}: no park"); break
        snap = snapshot(sock, native)
        snaps.append(snap)
        continue_(sock, native)
    return snaps


def main():
    build = os.path.abspath('build-rdb/Release')
    rom = os.path.abspath('segagenesisrecomp/sonicthehedgehog/sonic.bin')
    exe_n = os.path.join(build, 'SonicTheHedgehogRecomp.exe')
    exe_o = os.path.join(build, 'SonicTheHedgehogRecomp_oracle.exe')

    subprocess.call(['taskkill','/F','/IM','SonicTheHedgehogRecomp.exe',
                     '/IM','SonicTheHedgehogRecomp_oracle.exe'],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    pn = subprocess.Popen([exe_n, rom, '--turbo'], cwd=build,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    po = subprocess.Popen([exe_o, rom, '--turbo'], cwd=build,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        sn = connect(4378); so = connect(4379)

        N = 40
        print(f"collecting {N} FMUpdateFreq snapshots on each...")
        sn_snaps = collect(sn, True,  0x071E24, N)
        print(f"native got {len(sn_snaps)}")
        so_snaps = collect(so, False, 0x071E24, N)
        print(f"oracle got {len(so_snaps)}")

        print("\nidx  N(A5       byte_A5 byte_A5+30 D6     D0)   | O(A5       byte_A5 byte_A5+30 D6     D0)   match")
        for i in range(min(len(sn_snaps), len(so_snaps))):
            n = sn_snaps[i]; o = so_snaps[i]
            key = ('A5', 'byte_A5', 'byte_A5_30', 'D6')
            same = all(n[k] == o[k] for k in key)
            print(f" {i:3}  {n['A5']:#08x} {n['byte_A5']} {n['byte_A5_30']} {n['D6']:04x} {n['D0']:04x} | "
                  f"{o['A5']:#08x} {o['byte_A5']} {o['byte_A5_30']} {o['D6']:04x} {o['D0']:04x} "
                  f" {'OK' if same else 'DIFF <<'}")
            if not same and i > 0:
                print("  -> first divergence found")
                break
    finally:
        for p in (pn, po):
            if p.poll() is None:
                p.terminate()
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired: p.kill()


if __name__ == '__main__':
    main()
