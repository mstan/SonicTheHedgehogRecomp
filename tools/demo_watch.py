#!/usr/bin/env python3
"""demo_watch.py — watch the Sonic 3 boot -> title -> attract-demo flow on
one or both runner instances, logging whenever a monitored value changes.

Monitors (per poll): game_mode ($FFF600), Demo_timer ($FFF614),
Demo_mode_flag ($FFFFF0), Next_demo_number ($FFFFF2), Demo_data_addr
($FFEF52), Current_zone_act ($FFFE10), plus sonic x/y and internal_frame.

Usage: demo_watch.py <seconds> [port ...]   (default ports 4384 4385)
"""
import socket, json, sys, time

def cmd(port, name, **args):
    s = socket.socket(); s.settimeout(4); s.connect(('127.0.0.1', port))
    req = {'id': 1, 'cmd': name}; req.update(args)
    s.sendall((json.dumps(req) + '\n').encode())
    buf = b''
    while b'\n' not in buf:
        ch = s.recv(65536)
        if not ch: break
        buf += ch
    s.close()
    return json.loads(buf.decode('utf-8', 'replace').strip())

def rd(port, addr, size):
    try:
        return cmd(port, 'read_memory', addr=hex(addr), size=size)['hex']
    except Exception:
        return None

def snap(port):
    try:
        st = cmd(port, 'sonic_state')
    except Exception as e:
        return None
    return {
        "gm":   st.get('game_mode'),
        "dtimer": rd(port, 0xFFF614, 2),
        "dflag":  rd(port, 0xFFFFF0, 2),
        "dnum":   rd(port, 0xFFFFF2, 2),
        "ddata":  rd(port, 0xFFEF52, 4),
        "zact":   rd(port, 0xFFFE10, 2),
        "x": st.get('x'), "y": st.get('y'),
        "ifr": st.get('internal_frame'),
    }

def key(s):  # change-detection key: ignore timer/frame churn
    if s is None: return None
    return (s["gm"], s["dflag"], s["dnum"], s["zact"])

def main():
    dur = float(sys.argv[1]) if len(sys.argv) > 1 else 35.0
    ports = [int(p) for p in sys.argv[2:]] or [4384, 4385]
    last = {p: object() for p in ports}
    t0 = time.time()
    print(f"# t  port  gm dflag dnum zact dtimer ddata     x    y    ifr")
    while time.time() - t0 < dur:
        for p in ports:
            s = snap(p)
            k = key(s)
            if s is not None and k != last[p]:
                last[p] = k
                t = time.time() - t0
                print(f"{t:6.1f} {p} gm={s['gm']} dflag={s['dflag']} "
                      f"dnum={s['dnum']} zact={s['zact']} dtimer={s['dtimer']} "
                      f"ddata={s['ddata']} x={s['x']} y={s['y']} ifr={s['ifr']}",
                      flush=True)
        time.sleep(0.25)
    # final snapshot regardless
    for p in ports:
        s = snap(p)
        if s:
            t = time.time() - t0
            print(f"{t:6.1f} {p} [final] gm={s['gm']} dflag={s['dflag']} "
                  f"dnum={s['dnum']} zact={s['zact']} dtimer={s['dtimer']} "
                  f"ddata={s['ddata']} x={s['x']} y={s['y']} ifr={s['ifr']}",
                  flush=True)

if __name__ == "__main__":
    main()
