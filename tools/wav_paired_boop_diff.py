#!/usr/bin/env python3
"""Paired native-vs-oracle WAV diff to isolate audible boops.

Both WAVs captured from the same ROM + same demo start should be near
sample-identical (demo is deterministic). Any per-sample divergence
beyond the ~0.07% rate drift is a real audible anomaly — PSG volume
steps, FM envelope transitions, etc. that are identical on both builds
drop out of the diff.

Usage:
    python tools/wav_paired_boop_diff.py nat.wav ora.wav

Outputs:
  - Rolling RMS of sample-delta over 100ms windows
  - Timestamps of divergence spikes (windows above N-sigma of the mean)
  - Peak per-sample difference with its timestamp
"""
import sys, wave, struct
from collections import Counter


def read_wav(path):
    with wave.open(path, 'rb') as w:
        n_chan = w.getnchannels()
        samp_w = w.getsampwidth()
        rate   = w.getframerate()
        n_fr   = w.getnframes()
        raw    = w.readframes(n_fr)
    assert samp_w == 2, f"{path}: expected 16-bit, got {samp_w*8}-bit"
    fmt = '<' + ('h' * (len(raw) // 2))
    samples = struct.unpack(fmt, raw)
    return samples, n_chan, rate, n_fr


def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    nat_path, ora_path = sys.argv[1], sys.argv[2]

    nat, nat_ch, nat_rate, nat_fr = read_wav(nat_path)
    ora, ora_ch, ora_rate, ora_fr = read_wav(ora_path)

    print(f"native: {nat_path}  rate={nat_rate} ch={nat_ch} frames={nat_fr} samples={len(nat)}")
    print(f"oracle: {ora_path}  rate={ora_rate} ch={ora_ch} frames={ora_fr} samples={len(ora)}")
    assert nat_rate == ora_rate and nat_ch == ora_ch
    sample_rate = nat_rate
    n_ch        = nat_ch

    # Align by trimming to shortest
    n = min(len(nat), len(ora))
    print(f"comparing {n} interleaved samples ({n // n_ch / sample_rate:.2f}s)")

    # Drift-invariant metric: compute ENVELOPES of each (|sample|
    # low-passed via 10ms box-filter), then diff. Envelopes track
    # loudness and move slowly — phase/rate drift barely moves them,
    # but a genuine click/boop jumps the envelope briefly.
    window_ms      = 10
    env_window     = (sample_rate * n_ch) * window_ms // 1000
    # Compute rolling-abs-average (envelope) in chunks
    def envelope(sig):
        out = []
        acc = 0; head = 0
        buf = [0] * env_window
        for i, s in enumerate(sig):
            acc += abs(s) - buf[head]
            buf[head] = abs(s)
            head = (head + 1) % env_window
            if i >= env_window:
                out.append(acc / env_window)
            else:
                out.append(acc / (i + 1))
        return out
    print("computing envelopes (this is slow in pure Python)...")
    nat_env = envelope(nat[:n])
    ora_env = envelope(ora[:n])

    # Rolling RMS of envelope difference over 100ms windows
    report_ms   = 100
    window_samp = (sample_rate * n_ch) * report_ms // 1000
    per_win = []
    for start in range(0, n - window_samp, window_samp):
        w_nat = nat_env[start:start + window_samp]
        w_ora = ora_env[start:start + window_samp]
        # Per-sample envelope diff
        diffs = [abs(a - b) for a, b in zip(w_nat, w_ora)]
        mx = max(diffs)
        sq = sum(d * d for d in diffs)
        rms = (sq / len(diffs)) ** 0.5
        per_win.append((start, mx, rms))

    # Basic distribution
    rmses = sorted(r for _, _, r in per_win)
    if not rmses:
        print("no windows"); return 1
    median_rms = rmses[len(rmses) // 2]
    p95_rms    = rmses[len(rmses) * 95 // 100]
    p99_rms    = rmses[len(rmses) * 99 // 100]
    max_rms    = rmses[-1]
    print(f"\n|nat - ora|  rolling-RMS over {window_ms}ms windows:")
    print(f"  median : {median_rms:.1f}")
    print(f"  p95    : {p95_rms:.1f}")
    print(f"  p99    : {p99_rms:.1f}")
    print(f"  max    : {max_rms:.1f}")

    # Flag windows above 2x median — envelope diff is quite stable so
    # even 2x is a meaningful outlier
    threshold = median_rms * 2
    print(f"\nwindows with envelope-diff RMS > {threshold:.0f} ({threshold/median_rms:.1f}x median):")
    hits = sorted([(s, mx, r) for s, mx, r in per_win if r > threshold],
                  key=lambda x: -x[2])
    for s, mx, r in hits[:30]:
        t_sec = s / (sample_rate * n_ch)
        print(f"  t={t_sec:7.3f}s  RMS={r:8.1f}  peak_diff={mx:.1f}")
    if len(hits) > 30:
        print(f"  ... and {len(hits) - 30} more")
    print(f"\n  total flagged windows: {len(hits)} / {len(per_win)} "
          f"({100 * len(hits) / len(per_win):.1f}%)")

    # Peak single-sample envelope diff
    sample_diffs = [abs(nat_env[i] - ora_env[i]) for i in range(n)]
    pk_idx = max(range(n), key=lambda i: sample_diffs[i])
    pk_t   = pk_idx / (sample_rate * n_ch)
    print(f"\npeak envelope |diff|={sample_diffs[pk_idx]:.1f} at t={pk_t:.3f}s  "
          f"(nat_env={nat_env[pk_idx]:.1f}, ora_env={ora_env[pk_idx]:.1f})")


if __name__ == '__main__':
    sys.exit(main() or 0)
