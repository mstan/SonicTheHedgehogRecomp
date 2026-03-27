#!/usr/bin/env python3
"""Scan frame history for jump events."""
import socket, json, time, sys

def send_cmd(cmd):
    s = socket.socket()
    s.settimeout(5)
    s.connect(("127.0.0.1", 4378))
    s.sendall((json.dumps(cmd) + "\n").encode())
    time.sleep(0.15)
    chunks = []
    while True:
        try:
            chunk = s.recv(65536)
            if not chunk: break
            chunks.append(chunk)
            if b"\n" in chunk: break
        except: break
    s.close()
    return json.loads(b"".join(chunks).decode().strip())

info = send_cmd({"cmd": "frame_info", "id": 1})
current = info["current_frame"]
print(f"Total frames: {current}")

jump_frames = []
for start in range(0, current, 600):
    end = min(start + 599, current - 1)
    data = send_cmd({"cmd": "sonic_history", "id": 2, "start": start, "end": end})
    if "frames" not in data:
        continue
    for f in data["frames"]:
        if f.get("available") == False:
            continue
        yv = f.get("yvel", 0)
        st = f.get("status", 0)
        if yv < -100 or (st & 2):
            jump_frames.append(f)

print(f"Found {len(jump_frames)} airborne frames")
if jump_frames:
    jumps = []
    current_jump = [jump_frames[0]]
    for i in range(1, len(jump_frames)):
        if jump_frames[i]["frame"] - jump_frames[i-1]["frame"] > 5:
            jumps.append(current_jump)
            current_jump = [jump_frames[i]]
        else:
            current_jump.append(jump_frames[i])
    jumps.append(current_jump)

    print(f"Detected {len(jumps)} distinct jumps:")
    for i, jump in enumerate(jumps):
        peak_yvel = min(f["yvel"] for f in jump)
        min_y = min(f["y"] for f in jump)
        start_y = jump[0]["y"]
        duration = len(jump)
        f0 = jump[0]["frame"]
        f1 = jump[-1]["frame"]
        print(f"  Jump {i+1}: frames {f0}-{f1} ({duration}f airborne), "
              f"peak yvel={peak_yvel}, y: {start_y}->{min_y} (height={start_y - min_y})")
        # Print first 3 and last 3 frames of each jump
        show = jump[:3] + (["..."] if len(jump) > 6 else []) + jump[-3:]
        for fr in show:
            if fr == "...":
                print(f"    ...")
            else:
                print(f"    F{fr['frame']}: y={fr['y']} yvel={fr['yvel']} "
                      f"status={fr['status']} joy={fr['joy_held']}/{fr['joy_press']}")
