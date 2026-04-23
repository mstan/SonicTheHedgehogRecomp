# Known issues

## Audio — occasional residual boops

**Symptom:** Extremely infrequent brief audio artifacts ("boops") during
gameplay. Nominal at worst — does not affect music melodies, SFX clarity,
or perceived audio quality in an obvious way. Not pinned to any specific
game event.

**Status:** Known, low priority. The major audio bugs — off-tune music,
ring-pickup squelches, missing DAC (SEGA voice, drums) — are all fixed
as of the audio architecture overhaul.

**Diagnostic tooling shipped:**
- `runner/audio/observability.c` — per-frame peak-derivative detector on
  FM + PSG output, flags anomalous frames via `[BOOP]` log line. Detector
  reports **0 boops over 60s demo** with the current renderer; any
  remaining audible artifacts are below detector threshold.
- `tools/wav_paired_boop_diff.py` — envelope-level diff between paired
  nat/ora WAV captures; finds divergent windows drift-invariantly.
- `tools/boop_window_dump.py` — dump raw sample values around a
  specified timestamp.
- `runner/main.c --audio-backend=ours|clownmdemu` — runtime switch
  between our cycle-stamped renderer and clownmdemu's (debug-only) for
  A/B comparison. Production builds drop clownmdemu; this flag is a
  diagnostic only.

**What's been ruled out:**
- FM register-stream divergence (writes match oracle bit-identically)
- Cycle-stamp precision (tried both 68K-cycle-truncated and master-cycle
  full-precision stamps — both land on boops)
- Event-queue overflow / ordering (validated with push counters)
- Per-chip advance-timing pattern (per-event vs per-write — no change)
- Leftover-cycle handling across frames
- Missing PSG low-pass filter — **fix applied**: port of clownmdemu's
  `LowPassFilter_FirstOrder_Apply` with (26.044, 24.044) coefficients
  now on the PSG output path. Eliminates the detector's 240-per-60s hit
  rate to 0.

**Next investigation directions, if the residual boops become audible
enough to matter:**
1. Apply the matching FM low-pass filters (clownmdemu applies FM LPF with
   (6.910, 4.910) coefficients after FM_OutputSamples — we currently
   don't).
2. Sample-level paired diff at specific timestamps via `boop_window_dump`
   against an oracle capture starting from the same demo seed.
3. Instrument `ym2612_advance` / `psg_advance` to dump chip state
   (attenuation, countdown, shift_register) per wall frame and diff
   against an oracle run, isolating the exact register that drifts.
