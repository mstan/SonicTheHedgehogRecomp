#!/usr/bin/env python3
"""Cross-correlate two WAV files to find the boot-offset between native and
oracle, then report residual differences in the aligned region.

The register-level (gm, internal_frame) compare breaks when SMPS boot phase
differs by N internal-frames — aligned buckets aren't actually playing the
same SMPS state.  WAV output is the ground truth of what the YM2612/PSG/DAC
emulation produced; if the samples are identical after lag-correction then
there's no recompiler audio bug (whatever the user hears is elsewhere —
SDL queue, clownmdemu, realtime pacing).  If they diverge, this pins down
the exact sample (= 68K cycle range) where native starts differing.

Usage:
    python compare_wav.py nat.wav ora.wav [--max-lag-sec 5]
"""
import argparse, struct, sys, wave
import numpy as np


def load_wav(path):
    with wave.open(path, 'rb') as w:
        ch = w.getnchannels()
        sw = w.getsampwidth()
        sr = w.getframerate()
        nframes = w.getnframes()
        raw = w.readframes(nframes)
    if sw != 2:
        sys.exit(f"{path}: expected 16-bit PCM, got {sw*8}-bit")
    a = np.frombuffer(raw, dtype=np.int16)
    if ch == 2:
        a = a.reshape(-1, 2)
    else:
        a = a.reshape(-1, 1)
    return a, sr


def to_mono(a):
    # int32 to avoid overflow in the average
    return (a.astype(np.int32).sum(axis=1) // a.shape[1]).astype(np.int32)


def find_lag(nat_mono, ora_mono, max_lag):
    """Return (lag, peak_value) such that nat[lag:] aligns with ora[0:].
    Positive lag = native starts sooner (samples before lag are native-only).
    Negative lag = oracle starts sooner.
    Uses FFT-based cross-correlation for speed over long signals."""
    # Trim to common 2^k + max_lag window to keep FFT cheap
    n = min(len(nat_mono), len(ora_mono))
    n = min(n, 2 << 22)  # ~8M samples ≈ 36 sec at 223721 Hz
    a = nat_mono[:n].astype(np.float64)
    b = ora_mono[:n].astype(np.float64)
    # Remove DC so correlation peak isn't dominated by offset
    a -= a.mean()
    b -= b.mean()
    # Normalise to make the peak value interpretable (1.0 = identical shifted)
    na = np.sqrt((a * a).sum())
    nb = np.sqrt((b * b).sum())
    if na < 1e-9 or nb < 1e-9:
        return 0, 0.0
    size = 1 << (int(np.log2(2 * n - 1)) + 1)
    fa = np.fft.rfft(a, size)
    fb = np.fft.rfft(b, size)
    corr = np.fft.irfft(fa * np.conj(fb), size)
    # corr[k] = sum(a[i] * b[i-k]) — valid lags: [-max_lag, +max_lag]
    # numpy stores positive lags at 0..size/2 and negative at size/2..size
    pos = corr[:max_lag + 1]
    neg = corr[size - max_lag:]
    if len(pos) == 0 and len(neg) == 0:
        return 0, 0.0
    best_pos_lag = int(np.argmax(pos)) if len(pos) else 0
    best_neg_lag = int(np.argmax(neg)) - max_lag if len(neg) else 0
    best_pos_val = pos[best_pos_lag] if len(pos) else -1e18
    best_neg_val = neg[best_neg_lag + max_lag] if len(neg) else -1e18
    if best_pos_val >= best_neg_val:
        return best_pos_lag, float(best_pos_val / (na * nb))
    return best_neg_lag, float(best_neg_val / (na * nb))


def summarise_diff(nat, ora, lag, sr):
    """After lag-aligning, report residual stats + first divergence region."""
    if lag > 0:
        a = nat[lag:]
        b = ora
    elif lag < 0:
        a = nat
        b = ora[-lag:]
    else:
        a = nat
        b = ora
    n = min(len(a), len(b))
    a = a[:n]
    b = b[:n]
    diff = (a.astype(np.int32) - b.astype(np.int32))
    abs_diff = np.abs(diff)
    # Per-sample mono diff (L + R combined)
    if diff.ndim == 2:
        ch_abs = np.abs(diff).sum(axis=1)
    else:
        ch_abs = abs_diff
    total_energy_a = np.sqrt(np.mean(a.astype(np.float64) ** 2))
    total_energy_b = np.sqrt(np.mean(b.astype(np.float64) ** 2))
    total_diff_energy = np.sqrt(np.mean(diff.astype(np.float64) ** 2))
    print(f"# post-alignment: {n} samples ({n/sr:.2f} sec), lag={lag} samples ({lag/sr*1000:.1f} ms)")
    print(f"# RMS native={total_energy_a:.1f}  oracle={total_energy_b:.1f}  residual={total_diff_energy:.1f}  "
          f"(residual/oracle = {total_diff_energy/max(total_energy_b,1e-9):.2%})")
    # Find first 256-sample window where sum(abs_diff) > threshold
    window = 256
    threshold = 64 * window  # ~1% of int16 peak per sample
    cumsum = np.cumsum(np.concatenate(([0], ch_abs)))
    slide = cumsum[window:] - cumsum[:-window]
    first_divergent = int(np.argmax(slide > threshold)) if (slide > threshold).any() else -1
    if first_divergent < 0:
        print("# No divergent window found — WAVs are effectively identical after lag correction.")
        return
    print(f"\n# First significantly-divergent window at sample {first_divergent} "
          f"(t={first_divergent/sr:.3f}s post-alignment)")
    # Show a few samples from that region
    lo = max(0, first_divergent - 8)
    hi = min(n, first_divergent + 24)
    print(f"# native[{lo}:{hi}] vs oracle[{lo}:{hi}]:")
    for i in range(lo, hi):
        va = a[i] if a.ndim == 1 else a[i, 0]
        vb = b[i] if b.ndim == 1 else b[i, 0]
        marker = " <-- first-divergent" if i == first_divergent else ""
        print(f"  [{i:7d}] nat={int(va):+6d}  ora={int(vb):+6d}  diff={int(va)-int(vb):+6d}{marker}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('native')
    ap.add_argument('oracle')
    ap.add_argument('--max-lag-sec', type=float, default=5.0,
                    help='max expected lag in seconds (default 5)')
    args = ap.parse_args()

    nat, sr_n = load_wav(args.native)
    ora, sr_o = load_wav(args.oracle)
    if sr_n != sr_o:
        sys.exit(f"sample rate mismatch: native={sr_n} oracle={sr_o}")
    print(f"# native: {len(nat)} samples ({len(nat)/sr_n:.2f} sec, {nat.shape[1]}ch)")
    print(f"# oracle: {len(ora)} samples ({len(ora)/sr_o:.2f} sec, {ora.shape[1]}ch)")

    nat_mono = to_mono(nat)
    ora_mono = to_mono(ora)
    max_lag = int(args.max_lag_sec * sr_n)
    lag, peak = find_lag(nat_mono, ora_mono, max_lag)
    print(f"# cross-correlation peak lag: {lag} samples ({lag/sr_n*1000:+.1f} ms), "
          f"normalised correlation = {peak:.4f}")

    summarise_diff(nat, ora, lag, sr_n)


if __name__ == '__main__':
    main()
