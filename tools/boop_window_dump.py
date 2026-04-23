#!/usr/bin/env python3
"""Dump sample-level waveform in a window around a given time, both WAVs.

Usage: boop_window_dump.py nat.wav ora.wav time_seconds [n_samples]
"""
import sys, wave, struct


def read_wav(path):
    with wave.open(path, 'rb') as f:
        n = f.getnframes()
        data = f.readframes(n)
    return struct.unpack('<' + 'h' * (n * 2), data), 223721


def main():
    nat_path, ora_path = sys.argv[1], sys.argv[2]
    t = float(sys.argv[3])
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 64
    nat, rate = read_wav(nat_path)
    ora, _ = read_wav(ora_path)
    # interleaved stereo; frame index = t*rate, sample index = frame*2
    start = int(t * rate) * 2
    print(f"t={t}s  frame_idx={int(t*rate)}  stereo_pair_idx={start}")
    print(f"{'#':>4} {'nat_L':>7} {'nat_R':>7} {'ora_L':>7} {'ora_R':>7}"
          f" {'diffL':>7} {'diffR':>7}")
    for k in range(n):
        i = start + k * 2
        if i + 1 >= len(nat) or i + 1 >= len(ora):
            break
        nL, nR = nat[i], nat[i+1]
        oL, oR = ora[i], ora[i+1]
        print(f"{k:>4} {nL:>7} {nR:>7} {oL:>7} {oR:>7} {nL-oL:>7} {nR-oR:>7}")


if __name__ == '__main__':
    main()
