#!/usr/bin/env python3
"""
wav_spectral_diff.py

Spectral / temporal A/B comparison of two WAV captures that should be
bit-identical but aren't. Used to hunt the ring-pickup squelch between
the native (recompiled) build and the oracle (interpreter) build.

Usage:
    python tools/wav_spectral_diff.py <nat.wav> <ora.wav>

Output:
    - Per-file basic info
    - Coarse alignment offset (via cross-correlation on a downmixed,
      decimated, early-onset window)
    - Top RMS-difference 100 ms windows
    - Top spectrogram-magnitude-difference time/freq cells
    - Ring-SFX band focus (2-6 kHz, fast-transient envelope diffs)
"""

import sys
import os
import wave
import numpy as np
from scipy import signal


# ---------------------------------------------------------------------------
# WAV loader (PCM16, mono/stereo). We intentionally use stdlib wave first so
# the script works even on hostile Python installs.
# ---------------------------------------------------------------------------
def load_wav(path):
    with wave.open(path, 'rb') as w:
        n_channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        framerate = w.getframerate()
        n_frames = w.getnframes()
        raw = w.readframes(n_frames)
    if sampwidth == 2:
        dtype = np.int16
    elif sampwidth == 1:
        dtype = np.uint8
    elif sampwidth == 4:
        dtype = np.int32
    else:
        raise RuntimeError(f"unsupported sampwidth {sampwidth}")
    data = np.frombuffer(raw, dtype=dtype)
    if n_channels > 1:
        data = data.reshape(-1, n_channels)
    # Normalize to float32 in [-1, 1]
    if dtype == np.int16:
        dataf = data.astype(np.float32) / 32768.0
    elif dtype == np.int32:
        dataf = data.astype(np.float32) / 2147483648.0
    else:
        dataf = (data.astype(np.float32) - 128.0) / 128.0
    return {
        'path': path,
        'rate': framerate,
        'channels': n_channels,
        'sampwidth': sampwidth,
        'n_frames': n_frames,
        'duration_s': n_frames / framerate,
        'data': dataf,  # (N,) mono or (N, C) stereo
    }


def to_mono(x):
    if x.ndim == 1:
        return x
    return x.mean(axis=1)


# ---------------------------------------------------------------------------
# Alignment: cross-correlate a bandlimited, decimated early window.
# We look only in the first ~10 seconds because the title-screen "SEGA"
# voice sits right there and is the biggest broadband transient.
# ---------------------------------------------------------------------------
def find_alignment_offset(a, b, rate, search_seconds=20.0, decim=16):
    """
    Returns offset_in_samples such that a[offset:] ~ b[:].
    Positive means 'a is delayed vs b by offset samples'.

    Uses envelope cross-correlation (not raw signal) so that local
    phase drift between the two emulation rates doesn't destroy
    the peak. Raw-signal correlation is useless when the two clocks
    advance at slightly different rates per frame.
    """
    amax = int(min(len(a), search_seconds * rate))
    bmax = int(min(len(b), search_seconds * rate))
    A = a[:amax]
    B = b[:bmax]

    # Envelope via rectify + low-pass + decimate. Envelope is robust to
    # the sub-sample-rate clock drift between the two captures.
    envA = np.abs(A)
    envB = np.abs(B)
    envA = signal.decimate(envA, decim, ftype='fir', zero_phase=True)
    envB = signal.decimate(envB, decim, ftype='fir', zero_phase=True)

    # Normalize.
    envA = (envA - envA.mean()) / (envA.std() + 1e-12)
    envB = (envB - envB.mean()) / (envB.std() + 1e-12)

    xc = signal.correlate(envA, envB, mode='full', method='fft')
    lags = signal.correlation_lags(len(envA), len(envB), mode='full')
    # Allow up to 10 s of lag in either direction.
    max_lag = int(10.0 * rate / decim)
    mask = (lags >= -max_lag) & (lags <= max_lag)
    xc_m = xc[mask]
    lags_m = lags[mask]
    peak = np.argmax(xc_m)
    best_lag_decim = int(lags_m[peak])
    best_lag = best_lag_decim * decim
    peak_val = float(xc_m[peak] / len(envA))
    return best_lag, peak_val


def find_alignment_onset(a, b, rate, search_seconds=20.0, thresh_frac=0.25):
    """
    Fallback alignment: find first sample in each stream whose short-term
    envelope crosses thresh_frac of that stream's peak envelope within the
    search window. Align their onsets. Useful when envelope XC peak is weak.
    """
    amax = int(min(len(a), search_seconds * rate))
    bmax = int(min(len(b), search_seconds * rate))

    def onset_idx(x, rate, thresh_frac):
        env = np.abs(x)
        win = max(1, int(rate * 0.005))
        kern = np.ones(win) / win
        env = np.convolve(env, kern, mode='same')
        peak = env.max()
        if peak <= 0:
            return 0
        thresh = thresh_frac * peak
        where = np.where(env > thresh)[0]
        return int(where[0]) if len(where) else 0

    oA = onset_idx(a[:amax], rate, thresh_frac)
    oB = onset_idx(b[:bmax], rate, thresh_frac)
    # We want a[offset:] ~ b[:], i.e. align onset of a to onset of b.
    # If onset_A is at sA and onset_B at sB, drop (sA - sB) of a.
    return oA - oB


# ---------------------------------------------------------------------------
# Per-100ms RMS difference ranking.
# ---------------------------------------------------------------------------
def rms_window_diff(a, b, rate, win_ms=100.0, topn=10):
    win = int(rate * win_ms / 1000.0)
    n = min(len(a), len(b))
    n = (n // win) * win
    A = a[:n].reshape(-1, win)
    B = b[:n].reshape(-1, win)
    rA = np.sqrt((A ** 2).mean(axis=1) + 1e-20)
    rB = np.sqrt((B ** 2).mean(axis=1) + 1e-20)
    diff = rA - rB  # signed: + = native louder, - = oracle louder
    absdiff = np.abs(diff)
    order = np.argsort(absdiff)[::-1][:topn]
    results = []
    for idx in order:
        t = idx * win / rate
        results.append({
            't_s': float(t),
            'rms_nat': float(rA[idx]),
            'rms_ora': float(rB[idx]),
            'diff': float(diff[idx]),
            'rel': float(diff[idx] / (rB[idx] + 1e-9)),
        })
    return results, rA, rB


# ---------------------------------------------------------------------------
# Spectrogram-difference ranking.
# ---------------------------------------------------------------------------
def spectro_diff(a, b, rate, nperseg=2048, noverlap=1024, topn=10):
    n = min(len(a), len(b))
    fA, tA, SA = signal.spectrogram(a[:n], fs=rate, nperseg=nperseg,
                                    noverlap=noverlap, scaling='spectrum',
                                    mode='magnitude')
    fB, tB, SB = signal.spectrogram(b[:n], fs=rate, nperseg=nperseg,
                                    noverlap=noverlap, scaling='spectrum',
                                    mode='magnitude')
    # Log-magnitude difference is perceptually closer than linear.
    LA = 20.0 * np.log10(SA + 1e-10)
    LB = 20.0 * np.log10(SB + 1e-10)
    D = LA - LB  # dB diff
    # Rank cells by |D|.
    flat = np.abs(D).ravel()
    idx_sorted = np.argsort(flat)[::-1]
    results = []
    seen_bins = {}
    for fi in idx_sorted:
        fbin, tbin = np.unravel_index(fi, D.shape)
        # Deduplicate close-by peaks so we get spread not one hotspot.
        key = (fbin // 4, tbin // 8)
        if key in seen_bins:
            continue
        seen_bins[key] = True
        results.append({
            't_s': float(tA[tbin]),
            'f_hz': float(fA[fbin]),
            'mag_nat_db': float(LA[fbin, tbin]),
            'mag_ora_db': float(LB[fbin, tbin]),
            'diff_db': float(D[fbin, tbin]),
        })
        if len(results) >= topn:
            break
    # Also compute mean |diff| per frequency band.
    band_edges = [0, 200, 500, 1000, 2000, 4000, 6000, 8000, 12000, 16000, 22050]
    bands = []
    for lo, hi in zip(band_edges[:-1], band_edges[1:]):
        mask = (fA >= lo) & (fA < hi)
        if not mask.any():
            continue
        m = np.abs(D[mask, :]).mean()
        bands.append({'lo_hz': lo, 'hi_hz': hi, 'mean_abs_diff_db': float(m)})
    return results, bands, (fA, tA, D)


# ---------------------------------------------------------------------------
# Ring-SFX focus: fast envelope diff in 2-6 kHz band (where the ring chime
# sits). We bandpass, take the envelope, and look for short-duration
# envelope discrepancies (~5-30 ms).
# ---------------------------------------------------------------------------
def ring_band_envelope_diff(a, b, rate, lo=2000.0, hi=6000.0, topn=10):
    sos = signal.butter(6, [lo, hi], btype='bandpass', fs=rate, output='sos')
    Af = signal.sosfiltfilt(sos, a)
    Bf = signal.sosfiltfilt(sos, b)
    # Envelope via magnitude of analytic signal.
    envA = np.abs(signal.hilbert(Af))
    envB = np.abs(signal.hilbert(Bf))
    # Smooth at ~5 ms.
    win = max(1, int(rate * 0.005))
    kern = np.ones(win) / win
    envA_s = np.convolve(envA, kern, mode='same')
    envB_s = np.convolve(envB, kern, mode='same')
    diff = envA_s - envB_s
    # Find top isolated transient divergences via ranked-peak-with-gap scan.
    absdiff = np.abs(diff)
    # Rough: find top peaks separated by >= 50 ms.
    gap = int(rate * 0.050)
    results = []
    scratch = absdiff.copy()
    for _ in range(topn):
        idx = int(np.argmax(scratch))
        if scratch[idx] <= 0:
            break
        results.append({
            't_s': float(idx / rate),
            'env_nat': float(envA_s[idx]),
            'env_ora': float(envB_s[idx]),
            'diff': float(diff[idx]),
        })
        lo_i = max(0, idx - gap)
        hi_i = min(len(scratch), idx + gap)
        scratch[lo_i:hi_i] = 0
    return results


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    nat_path = sys.argv[1]
    ora_path = sys.argv[2]

    print(f"[load] {nat_path}")
    nat = load_wav(nat_path)
    print(f"[load] {ora_path}")
    ora = load_wav(ora_path)

    print()
    print("=== (a) WAV basic info ===")
    for label, w in [("nat", nat), ("ora", ora)]:
        print(f"  {label}: rate={w['rate']} Hz  channels={w['channels']}  "
              f"sampwidth={w['sampwidth']}B  frames={w['n_frames']}  "
              f"duration={w['duration_s']:.3f}s  file={os.path.basename(w['path'])}")

    if nat['rate'] != ora['rate']:
        print(f"[warn] sample rate mismatch: {nat['rate']} vs {ora['rate']}")
    rate = nat['rate']

    natm = to_mono(nat['data'])
    oram = to_mono(ora['data'])

    print()
    print("=== (b) Alignment ===")
    offset_xc, peakv = find_alignment_offset(natm, oram, rate)
    offset_on = find_alignment_onset(natm, oram, rate)
    print(f"  envelope-XC lag:       {offset_xc} samples = "
          f"{offset_xc / rate * 1000.0:.2f} ms (peak corr {peakv:.4f})")
    print(f"  onset-match lag:       {offset_on} samples = "
          f"{offset_on / rate * 1000.0:.2f} ms")
    # Prefer envelope-XC if its peak correlation is meaningful; otherwise
    # fall back to onset match.
    if peakv > 0.3:
        offset = offset_xc
        print(f"  using envelope-XC alignment.")
    else:
        offset = offset_on
        print(f"  using onset-match alignment (XC peak too weak).")

    # If the two captures have non-trivially different total lengths
    # (>0.5%), the emulation rates diverged across the capture -- a
    # single offset will mis-align later portions. Time-stretch nat to
    # ora's length so downstream spectral diffs are meaningful.
    len_ratio = len(natm) / len(oram)
    if abs(len_ratio - 1.0) > 0.005:
        print(f"  length ratio nat/ora = {len_ratio:.4f}  "
              f"-> resampling nat to match ora")
        # Use polyphase resample for speed. Target length = len(ora).
        # scipy.signal.resample_poly needs integer up/down. Approximate.
        from fractions import Fraction
        frac = Fraction(len(oram), len(natm)).limit_denominator(2000)
        up, down = frac.numerator, frac.denominator
        print(f"  resample_poly up={up} down={down}")
        natm = signal.resample_poly(natm, up, down).astype(np.float32)
        # Re-run onset alignment after resample.
        offset = find_alignment_onset(natm, oram, rate)
        print(f"  post-resample onset lag: {offset} samples = "
              f"{offset / rate * 1000.0:.2f} ms")

    # Apply alignment so a[i] corresponds to b[i].
    if offset > 0:
        # nat lags by offset -> drop leading offset of nat.
        A = natm[offset:]
        B = oram
    elif offset < 0:
        A = natm
        B = oram[-offset:]
    else:
        A = natm
        B = oram
    n = min(len(A), len(B))
    A = A[:n]
    B = B[:n]
    print(f"  aligned length:        {n} samples = {n / rate:.3f} s")

    print()
    print("=== (c.1) Top 10 x 100ms RMS divergence windows ===")
    top_rms, rA, rB = rms_window_diff(A, B, rate, win_ms=100.0, topn=10)
    for i, r in enumerate(top_rms):
        sign = "NAT>ora" if r['diff'] > 0 else "ora>NAT"
        print(f"  #{i+1:02d} t={r['t_s']:7.3f}s  "
              f"rms_nat={r['rms_nat']:.5f}  rms_ora={r['rms_ora']:.5f}  "
              f"diff={r['diff']:+.5f}  rel={r['rel']*100:+6.1f}%  {sign}")

    print()
    print("=== (c.2) Top 10 spectrogram cells by |dB diff| (2048-pt STFT) ===")
    top_spec, bands, _ = spectro_diff(A, B, rate, nperseg=2048, noverlap=1024,
                                      topn=10)
    for i, r in enumerate(top_spec):
        print(f"  #{i+1:02d} t={r['t_s']:7.3f}s  f={r['f_hz']:7.1f}Hz  "
              f"nat={r['mag_nat_db']:+7.1f}dB  ora={r['mag_ora_db']:+7.1f}dB  "
              f"diff={r['diff_db']:+7.1f}dB")

    print()
    print("=== (c.3) Mean |dB diff| per frequency band ===")
    for b in bands:
        bar = '#' * int(min(40, b['mean_abs_diff_db']))
        print(f"  {b['lo_hz']:6.0f} - {b['hi_hz']:6.0f} Hz : "
              f"{b['mean_abs_diff_db']:6.2f} dB  {bar}")

    print()
    print("=== (d) Ring-SFX band (2-6 kHz) envelope divergence ===")
    ring = ring_band_envelope_diff(A, B, rate, lo=2000.0, hi=6000.0, topn=10)
    for i, r in enumerate(ring):
        sign = "NAT>ora" if r['diff'] > 0 else "ora>NAT"
        print(f"  #{i+1:02d} t={r['t_s']:7.3f}s  "
              f"env_nat={r['env_nat']:.5f}  env_ora={r['env_ora']:.5f}  "
              f"diff={r['diff']:+.5f}  {sign}")

    # Also check 6-12 kHz (where attenuated-output squelch artifacts often
    # appear on YM2612 — missing upper harmonics from LFO / envelope skew).
    print()
    print("=== (d.2) Upper band (6-12 kHz) envelope divergence ===")
    ring2 = ring_band_envelope_diff(A, B, rate, lo=6000.0, hi=12000.0, topn=5)
    for i, r in enumerate(ring2):
        sign = "NAT>ora" if r['diff'] > 0 else "ora>NAT"
        print(f"  #{i+1:02d} t={r['t_s']:7.3f}s  "
              f"env_nat={r['env_nat']:.5f}  env_ora={r['env_ora']:.5f}  "
              f"diff={r['diff']:+.5f}  {sign}")

    # ----- Timing-independent aggregate-power-spectrum comparison -----
    # If per-window diffs are dominated by mere time-misalignment of
    # identical musical events, the aggregate long-window spectra will
    # still match. Any mismatch here is a true timbre/squelch signal.
    print()
    print("=== Timing-independent long-window power spectra ===")
    # Use Welch over the full aligned range.
    fA, PA = signal.welch(A, fs=rate, nperseg=8192, noverlap=4096)
    fB, PB = signal.welch(B, fs=rate, nperseg=8192, noverlap=4096)
    # Normalize each to unit total power so level mismatches don't dominate.
    PA_n = PA / PA.sum()
    PB_n = PB / PB.sum()
    Ldb = 10.0 * np.log10(PA_n + 1e-20) - 10.0 * np.log10(PB_n + 1e-20)
    # Aggregate into bands.
    band_edges = [0, 100, 200, 500, 1000, 2000, 3000, 4000, 6000, 8000,
                  10000, 12000, 16000, 22050]
    print("  band              nat pow%     ora pow%    diff dB (norm.)")
    for lo, hi in zip(band_edges[:-1], band_edges[1:]):
        m = (fA >= lo) & (fA < hi)
        if not m.any():
            continue
        pa = float(PA_n[m].sum()) * 100.0
        pb = float(PB_n[m].sum()) * 100.0
        d = 10.0 * np.log10((pa + 1e-12) / (pb + 1e-12))
        bar = ('+' if d > 0 else '-') * int(min(20, abs(d) * 5))
        print(f"  {lo:6.0f} - {hi:6.0f} Hz : "
              f"{pa:7.3f}%    {pb:7.3f}%    {d:+6.2f}  {bar}")

    # Peak spectral lines where nat and ora diverge in sustained power.
    # Smooth each spectrum and find the top-10 bins with largest normalized
    # power ratio.
    print()
    print("=== Top 15 frequency bins by sustained power ratio (nat/ora, dB) ===")
    # Mask out < 50 Hz and DC (below that = capture DC / rumble).
    ok = fA >= 50.0
    ratio = 10.0 * np.log10((PA_n[ok] + 1e-20) / (PB_n[ok] + 1e-20))
    # Rank by absolute ratio but weight by actual power so we don't pick
    # inaudible noise bins.
    weight = np.maximum(PA_n[ok], PB_n[ok])
    score = np.abs(ratio) * np.log10(weight * 1e12 + 1.0)
    fok = fA[ok]
    order = np.argsort(score)[::-1]
    # Deduplicate by coarse frequency buckets (~40 Hz).
    seen = set()
    shown = 0
    for idx in order:
        bucket = int(fok[idx] / 40.0)
        if bucket in seen:
            continue
        seen.add(bucket)
        sign = "nat>ora" if ratio[idx] > 0 else "ora>nat"
        print(f"  f={fok[idx]:7.1f} Hz  nat={10.0*np.log10(PA_n[ok][idx]+1e-20):+7.1f}dB  "
              f"ora={10.0*np.log10(PB_n[ok][idx]+1e-20):+7.1f}dB  "
              f"ratio={ratio[idx]:+6.2f}dB  {sign}")
        shown += 1
        if shown >= 15:
            break

    # Time distribution of ring-band envelope divergence: 1-s histogram
    # of |env(nat) - env(ora)| in the 2-6 kHz band over the full run.
    print()
    print("=== Time-concentration of 2-6 kHz envelope divergence (1 s bins) ===")
    sos = signal.butter(6, [2000.0, 6000.0], btype='bandpass', fs=rate,
                        output='sos')
    Af_rb = signal.sosfiltfilt(sos, A)
    Bf_rb = signal.sosfiltfilt(sos, B)
    envA_rb = np.abs(signal.hilbert(Af_rb))
    envB_rb = np.abs(signal.hilbert(Bf_rb))
    win = int(rate * 1.0)
    nbin = len(envA_rb) // win
    EA = envA_rb[:nbin * win].reshape(-1, win).mean(axis=1)
    EB = envB_rb[:nbin * win].reshape(-1, win).mean(axis=1)
    dE = np.abs(EA - EB)
    # Normalize to peak.
    dEn = dE / (dE.max() + 1e-12)
    for i in range(nbin):
        bar = '#' * int(dEn[i] * 50)
        print(f"  t={i:3d}s  |diff|={dE[i]:.5f}  envA={EA[i]:.5f}  envB={EB[i]:.5f}  {bar}")

    # Global summary metrics.
    print()
    print("=== Summary metrics ===")
    diff_sig = A - B
    rms_diff = float(np.sqrt((diff_sig ** 2).mean()))
    rms_A = float(np.sqrt((A ** 2).mean()))
    rms_B = float(np.sqrt((B ** 2).mean()))
    print(f"  global RMS(nat)       = {rms_A:.5f}")
    print(f"  global RMS(ora)       = {rms_B:.5f}")
    print(f"  global RMS(nat-ora)   = {rms_diff:.5f}")
    print(f"  'noise' SNR (dB)      = {20.0 * np.log10(rms_B / (rms_diff + 1e-12)):.2f}")

    # Output first "divergence onset": first 100 ms window where rms diff
    # exceeds 5% of oracle RMS.
    win = int(rate * 0.1)
    nwin = len(A) // win
    Ar = A[:nwin * win].reshape(-1, win)
    Br = B[:nwin * win].reshape(-1, win)
    rAw = np.sqrt((Ar ** 2).mean(axis=1))
    rBw = np.sqrt((Br ** 2).mean(axis=1))
    rel = np.abs(rAw - rBw) / (rBw + 1e-9)
    onset = np.argmax(rel > 0.05)
    if rel[onset] > 0.05:
        print(f"  first >5%-rel divergence at t={onset * 0.1:.2f}s  "
              f"rel={rel[onset]*100:.1f}%")
    else:
        print("  no 100ms window exceeds 5% relative divergence.")


if __name__ == '__main__':
    main()
