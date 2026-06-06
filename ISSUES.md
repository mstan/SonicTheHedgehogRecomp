# Known issues

## Own backend — V-int off-by-one at the title transition (attract only)

**Symptom:** On the OWN_BACKEND build, A/B against the clownmdemu oracle
(same recompiled 68K, same attract demo, `--framelog` diff) shows the
own-backend V-int runcount (`fcnt`) tracking the oracle perfectly through
the Sega screen (offset 0, F000–F150), then dropping **exactly one** V-int
at the title transition (~F177) and staying locked at −1 thereafter. That
one-frame nudge sends the *attract sequence* down a different branch
(own runs `GM_Demo $08`, oracle `GM_Level $0C`) and the title screen
lingers noticeably longer (~511 vs ~337 frames) before the demo starts.

**Consequence:** None for interactive play — it's a one-time 1/60 s skew on
the idle title/attract screens; gameplay, feel, animation, and sprite
rendering are unaffected (you drive Sonic via input, not the demo). It only
shows up if you sit on the title and watch the built-in attract demo.

**Likely cause:** No pending-V-int latch. On hardware a V-int raised while
the CPU has interrupts masked (`move #$2700,sr`, which Sonic does around
screen transitions) stays *pending* and fires the instant interrupts
re-enable. `glue_own_interrupt()` samples the mask **once** at the vblank
scanline and **drops** the V-int if masked at that instant, so one V-int is
lost during the masked title transition. (clownmdemu samples at a different
phase and catches it.) The ~174-frame attract title-length gap is larger
than one dropped V-int alone explains, so there is likely a second small
attract-specific factor (title demo-timer / input-idle check on our bus)
not yet isolated.

**Status:** Deferred — does not affect gameplay. Revisit if attract-mode
fidelity matters.

**Fix sketch (for bit-exactness):** add a pending-V-int latch in
`machine_run_frame` + `glue_own_interrupt`: when the vblank scanline hits
with interrupts masked, mark V-int pending and deliver it on the next
scanline boundary where the mask drops, instead of discarding it. Then
re-run the `--framelog` A/B (now backend-aware, reads `g_ram` under
OWN_BACKEND) and confirm `fcnt` offset stays 0.

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
