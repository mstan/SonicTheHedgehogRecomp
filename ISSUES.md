# Known issues

## Widescreen (16:9) — brief stale/partial object sprites in the right margin (2026-06-16)

**Symptom:** With `GENESIS_WIDESCREEN=1`, an *occasional, very brief* object-like
artifact appears in the right-hand widescreen margin **ahead of** the camera —
a sprite drawn partially or at a displaced position for a frame or two as it
scrolls in. Worse for **wide, multi-piece objects** (e.g. Marble Zone's
sinking-into-lava platforms). Reproduces in the **attract demos** (no input
needed). 4:3 (default) is unaffected.

**Root cause (strong hypothesis, not visually confirmed):** the **9-bit VDP
sprite-X field wraps** at the widescreen right edge. At
`GENESIS_WIDESCREEN_COLUMNS=8` the per-side margin is `extra=64px`, so the
widened right edge sits at screen-relative X `320+64 = 384`, which the 68K
encodes as VDP sprite X `384+128 = 512`. But sprite X is only 9 bits (max
511): `_inc/BuildSprites.asm` masks every sprite piece with `andi.w #$1FF,d0`
(all four `BuildSpr_Normal/FlipX/FlipY/FlipXY` paths), and the engine reads it
back as `xword = vram_read_word(...) & 0x1FF` (`runner/video/genesis_vdp.c`
`sprite_render_line`). So any piece whose intended sprite X reaches 512+ wraps
to near 0 and renders off the **left** edge (clipped) instead of the right —
leaving a wide object drawn only partially for the few frames its reference
point sits in the wrap zone (`relX ≈ 384–416`) as it scrolls into view. The
object-cull (`BuildSprites`) and tile-load widening are correct; this is purely
the sprite-coordinate encoding hitting a hardware-width limit.

**Evidence:** engine masks sprite X to 9 bits (`genesis_vdp.c:475`); 68K masks
to `$1FF` at 4 sites; the symptom (brief, right-margin, worse for wide
multi-piece sprites, demo-reproducible) matches the wrap geometry. The opposite
direction (artifacts on the *left*) would also be consistent with the wrap —
left-edge artifacts were not reported, only right.

**Consequence:** Cosmetic and brief; only in 16:9. 4:3 is bit-identical (at 4:3
no sprite X ever exceeds ~480, so the `$1FF`/`$3FF` mask choice is moot).

**Candidate fix (~5 edits, unverified):** widen the sprite-X path to 10 bits so
off-edge pieces *clip* (screen_x ≥ total) instead of wrapping:
- disasm: `andi.w #$1FF,d0` → `andi.w #$3FF,d0` in `_inc/BuildSprites.asm`
  (`BuildSpr_Normal`, `BuildSpr_FlipX`, `BuildSpr_FlipY`, `BuildSpr_FlipXY`);
- engine: `& 0x1FF` → `& 0x3FF` for `xword` in `genesis_vdp.c` `sprite_render_line`.
  This is safe in 4:3 (bit 9 is never set there, so the result is identical) and
  only affects widescreen. NOTE: the same limit applies to **all** widescreen
  games (S2/S3) since the sprite read is shared engine code — fix once in the
  engine, plus per-game in each disasm's `BuildSprites`/`Render_Sprites`.
- Alternative (incomplete): cap `extra ≤ 63` — avoids the reference-point wrap
  but pieces extending past the edge still wrap, so not a full fix.

**Status:** Deferred (2026-06-16, user choice — artifact is too brief to chase
via screenshots and is cosmetic). Documented for follow-up; the fix looks small
and well-scoped.

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

## Own backend — VRAM/rendering divergence grows through the attract demo (UNDER INVESTIGATION, 2026-06-06)

**Symptom:** A full native(own-backend)-vs-oracle(clownmdemu) per-subsystem
parity audit (`segagenesisrecomp/tools/oracle_parity_audit.py`, run boot →
attract demo on the same input-free attract sequence) shows **VRAM diverging
and growing monotonically**: ~227 differing bytes at `vint_runcount`=600,
~2761 at 1000, ~3134 at 2000, **~7151 at 3600**. WRAM object-RAM pages
(`$FFCxxx`, e.g. `$FFCC/CD/CE/CF`) also diverge at depth. The growth (far more
than any single DMA transfer's worth) means this is a *real* accumulating
divergence, not just intra-frame phase skew.

**What is NOT the cause (measured, same audit):**
- **Z80 + its 8KB sound-driver RAM are AT PARITY** at every audited point
  (boot → vint 3900). So the divergence has **not** fed back into the audio
  path — sound-command state stays identical even while VRAM drifts.
- VDP **address registers** (plane A/B, window, sprite table, hscroll) match;
  the divergence is in VRAM *contents* + scroll (`vsram[0]/[1]` differ slightly),
  not the register configuration.

**Likely cause (hypotheses, unconfirmed):** (a) the known V-int off-by-one at
the title transition (section above) sends the own backend down a *different
attract branch* (`GM_Demo $08` vs oracle `GM_Level $0C`), so from that point on
the two are rendering genuinely different content — i.e. this VRAM divergence
may be a *downstream consequence* of the V-int latch bug, not an independent VDP
bug; (b) a DMA-timing / VRAM-write-path difference in the own backend; (c)
attract demo-timer / input-idle drift on our bus. Distinguish by checking
whether `Game_Mode` ($FFF600) has already diverged before VRAM does.

**Consequence:** Visual only, and only on the idle attract demo (you drive
gameplay via input, not the demo). Distinct from the audio bug. Lower priority.

**Status:** Logged. Tooling: `oracle_parity_audit.py` reproduces it on demand
(`--vints 600,1000,2000,3600`). Revisit after the audio chip-stream work; first
check if it collapses into the V-int latch bug above.

## Audio — own-backend GHZ SFX inconsistent / music partly off (UNDER INVESTIGATION, 2026-06-06)

**Supersedes the "major audio bugs fixed" claim below for the OWN_BACKEND
build.** On the own backend, parts of the GHZ music sound off, and one-shot
SFX (rings, enemy pops, item box, ring-spill on damage) are **inconsistent**:
sometimes correct, sometimes quiet/faint, sometimes *partial* (you only hear
the tail — the onset is clipped), sometimes missing entirely. The **GHZ
attract demo reliably reproduces it** (no input needed — sit on the title and
let the demo run ~1 min in).

### Confirmed this session (evidence-based, via always-on rings)

- **The Z80 V-int is NOT the cause — and is correct as-is.** Sonic 1's Z80
  sound driver is *poll-driven with interrupts deliberately disabled*. Proof
  from the live Z80 RAM image (`z80_ram.bin`): the driver's main loop is
  `$0035: ld a,(hl) / or a / jp p,$0035` polling its mailbox, and the IM1
  vector `$0038` lands *inside* that `jp` instruction's operand (`f2 35 00`)
  — there is no interrupt handler there. Over 2611 frames: `iff1=0` at every
  vblank and **zero** V-ints ever accepted. So our never-accepted, sticky
  Z80 V-int is harmless/correct. (The earlier "sticky V-int / tempo"
  hypothesis is dead.)
- **The sound-command mailbox is the single byte Z80-RAM `$1FFF`.** The 68K
  deposits a sound ID (≥`$80`, bit7 set) there under a BUSREQ handshake; the
  driver wakes, does `sub $81`, and dispatches. *Every* 68K→Z80 sound write
  targets `$1FFF` — there is no second slot/queue in Z80 RAM.
- **No mailbox clobbering observed** in captured windows (no deposit landed
  on a still-unconsumed command), and **the command→chip path is healthy when
  exercised** — every delivered command produced a full FM/PSG register
  burst.
- **Render granularity is ruled out** as a cause of the music being off: both
  the ymfm FM (`s_master_accum`) and the clean-room PSG
  (`s_leftover_master_cycles`) are sample-accurate (carry the remainder across
  per-scanline advances), and the per-frame drain buffers
  (`FM_ACCUM_FRAMES=4096`, `PSG_ACCUM_FRAMES=16384`) far exceed per-frame
  generation (~889 FM / ~3734 PSG), so no samples are dropped/truncated.

### Localized to RENDERING (delivery/dispatch ruled out)

Captured the GHZ attract demo ~1 min in (where it reproduces) and correlated
every `$1FFF` deposit with the chip writes that follow. Result across the
whole window: **every command produces a full, consistent FM/PSG burst**
(`$82`→~4100 FM writes, `$81`→~1450, no weak/empty bursts, no drops, no
mailbox overwrites), and the **PSG byte stream is healthy and musical**
(proper decaying ch3-noise envelopes, ch2 tone setup). So:

- **(1) 68K-side drop — RULED OUT.** Deposits arrive normally.
- **(2) Z80-side drop — RULED OUT.** Every deposit yields a full chip burst.
- **(3) Synth/render — CONFIRMED as the locus.** The register streams reaching
  our chips are correct and complete; the faint/partial/missing/inconsistent
  result is in how we render them.

Two concrete render mechanisms (both code-visible), to test with audio
verification (do NOT ship unverified — current audio is partly-good):

- **(3a) PSG/FM mix imbalance — prime suspect for "faint rings/enemies."**
  `runner/audio.c` mixes **PSG ÷8, FM ÷1**, a constant lifted from
  clownmdemu's mixer that was tuned for *clownmdemu's* PSG scale. Our
  clean-room `sn76489.c` has its own scale (`SN_VOL` peak 6000 → ÷8 ≈ 750/ch,
  ~3000 max for 4 ch) while ymfm FM runs ±32k — so PSG-based SFX (rings, enemy
  pops, ring-spill) sit ~10× under the FM music and get buried.
- **(3b) OWN_BACKEND per-scanline write-collapse — suspect for "parts of music
  off" / inconsistency.** `genesis_machine.c` applies all of a scanline's FM
  writes then advances ~3.4 samples; it does NOT use `mixer.c`'s
  advance-between-writes path (whose own comment names write-collapse as the
  cause of "boop/squelch"). At the DAC's ~8-16 kHz rate this aliases drums and
  can clip short SFX onsets. Measured corroboration: native (per-scanline) is
  ~24% louder RMS (1912) than the event-queue path feeding the *same* synth
  (1538) over the same demo.

**A/B limitation found:** the in-build `--audio-backend=ours|clownmdemu`
switch can't isolate the synth here — `PERMISSIVE_FM` links our ymfm/SN76489
and first-wins-overrides clownmdemu's, so both backend paths produce
byte-identical WAVs. A true clownmdemu-synth reference needs a non-PERMISSIVE
build.

Captured artifacts (in `build/Release/`): `native_ours.wav` (shipping path,
70 s demo), `oracle_ours.wav`, `oracle_clown.wav`.

Note: a headless scripted run that reached GHZ went *silent* on the level
transition (no GHZ music-start command delivered after ~frame 708; Z80 healthy
but idle) — possibly a separate level-transition sound-init issue, or a
degenerate scripted state. Not yet confirmed as a real bug.

### Diagnostic tooling added (TEMP — `[SND-TRACE]`/`[CHIP-TRACE]`, strip before commit)

- `runner/video/genesis_machine.c` — `SndEvt` ring (mailbox deposits, Z80
  write-backs, V-int assert/accept phase, BUSREQ) **plus** a dedicated
  `ChipEvt` ring capturing the raw FM port/value + PSG byte stream; one-shot
  `z80_ram_dump`.
- `runner/video/genesis_bus.c` — `snd_trace_chip` hooked at all 6 FM/PSG
  write sites (68K word/byte + Z80 buses).
- `runner/main.c` — **F12 dumps both rings** (`snd_ring.txt` + `chip_ring.txt`)
  coincidentally; `--snd-dump-frame N` headless auto-dump (+ `z80_ram.bin`).
- `tools/chip_trace_analyze.py` — V-int cadence check, FM/PSG/DAC rate
  summary, and per-`$1FFF`-deposit → chip-burst correlation with
  drop/overwrite flags.

**Ruled out (3a): PSG/FM mix balance.** Made `PSG_VOL_DIV` a runtime knob
(`--psg-vol-div`) and A/B'd ÷8/÷4/÷2/÷1 by ear — no real improvement. The
symptom is not a simple level imbalance.

### Synth parity harness (ours vs theirs) — `tools/synth_replay/`

Built an offline harness that replays the *identical* captured chip-write
stream (`chip_ring.txt`, timing derived from the (frame,scanline) stamps)
through BOTH synths and emits `ours.wav`, `theirs.wav` (clownmdemu reference =
"what we had before"), and `diff.wav`, plus a level-vs-content decomposition
and per-window divergence:

- OURS   = `runner/audio/ym2612_ymfm.cpp` (ymfm) + `runner/audio/sn76489.c`
- THEIRS = clownmdemu-core `fm.c`/`psg.c` + `low-pass-filter.c` (DEV-ONLY;
  links the AGPL synth purely to observe, like the `_oracle` build — never
  shipped)

Both run through the same mixer, so only the synth core + its LPF differ. This
is the rigorous "measure ours vs theirs" instrumentation, immune to
scheduler/content divergence (no `_oracle` demo-branch mismatch).

Findings (GHZ demo, 15.4 s window):
- **FM levels match** the reference closely (ours RMS 1420 vs theirs 1393) —
  our FM gain is fine; the earlier "ymfm ~2× louder" reading was an artifact of
  an uninitialised clownmdemu `configuration` in an early harness rev (the
  `*_Initialise` funcs don't clear `configuration`; must `memset` the struct).
- **PSG is anti-correlated** with the reference (best-fit scale −0.55) — our
  SN76489 has an opposite polarity / phase convention vs clownmdemu's PSG.
- Per 0.25 s window, ours is **sometimes quieter** than theirs (candidate
  faint/missing-SFX windows, e.g. t≈2.5 s: ours 1075 vs theirs 1804) and
  sometimes louder — a per-window, content-dependent divergence, not a global
  gain error.
- Raw sample-diff is inflated by square-wave **phase drift**; the WAVs (for
  listening) and an envelope/spectral metric are the meaningful comparisons.

### Fixes applied (measured via the harness; pending user ear-validation)

Using the harness as the objective loop (phase-invariant envelope corr +
spectral log-distance vs the clownmdemu reference render of the same stream):

1. **PSG volume table** (`sn76489.c` `SN_VOL`) — was peak 6000/ch, ~22% under
   the reference; set to the reference table (peak 0x1FFF). PSG RMS 5175 → 6600
   (theirs 6626).
2. **PSG low-pass** (`sn76489.c`) — replaced the bright `>>1` one-pole with the
   reference first-order LPF (coefficients 26.044/24.044). PSG envelope
   meanRelErr 0.29 → **0.08**, corr 0.956, spectral distance 4.61 dB, band
   ratios 0.86–1.02 across the spectrum → **PSG at parity**.
3. **FM output low-pass** (`ym2612_ymfm.cpp`) — ymfm emits bare chip output with
   excess >8 kHz energy; added the hardware FM LPF (coefficients 6.910/4.910,
   what clownmdemu/the real Genesis apply). A cutoff sweep confirmed these
   minimise spectral distance. FM spectral distance 14.4 → **6.72 dB**.

The PSG was genuinely broken (too quiet + too bright) — the likely cause of the
faint/buried rings, enemy SFX, and PSG-carried music parts. The residual **FM
6.72 dB / envelope corr 0.78 is the inherent ymfm-vs-clownmdemu core
difference** (ymfm has a 1–2 kHz mid deficit + bass/DAC excess vs clownmdemu);
it cannot be closed by filtering and ymfm is the more hardware-accurate core, so
this is treated as acceptable, not a bug.

**Shared-code note:** the fixes are in the shared runner (`sn76489.c`,
`ym2612_ymfm.cpp`) → they affect S2/S3 too (toward the same clownmdemu
reference). Re-validate S2/S3 audio before committing.

### Cross-backend chip-stream tap + parity audit (2026-06-06, session 2)

Built the oracle-side half of the chip-stream A/B and a full per-subsystem
parity audit, and reached a sharper conclusion:

- **Own-backend debug snapshots ported** (`frame_snapshots.c`): the own backend
  now fills the SAME `FrameRecord` fields the oracle does (Z80 regs + 8KB Z80
  RAM, VDP semantic regs + VRAM/CRAM/VSRAM, WRAM) — previously these were
  zeroed/stubbed on native, so no cross-backend diff was possible at all.
- **Oracle chip-stream tap with ZERO AGPL edits**: the clownmdemu fork already
  routes every FM/PSG write through the runner's `audio_event_push`
  (bus-z80.c / bus-main-m68k.c), so a tap there (`event_queue.c`) captures the
  oracle stream into a shared dev-only ring (`runner/chip_trace.c`). Both builds
  now dump `chip_ring.txt`. Diff with `tools/chip_stream_diff.py`.
- **Stream stamped by `vint_runcount`, not wall frame** — the cross-backend sync
  key (native & oracle hit a given vint at the same logical sound state but at
  different wall frames). New `--snd-dump-vint N` trigger.

**Decisive finding — the attract-demo A/B is confounded by content divergence.**
A direct, vint-synced state comparison (`tools/oracle_parity_audit.py`) shows
that up to **vint 175** (just before the title transition) the own backend is
audio-faithful: **`z80.ram` is bit-identical (0/8192 bytes differ)** and
`Game_Mode` matches. At **vint ~177 the documented V-int off-by-one flips the
attract branch** (native `GM_Demo $08` vs oracle `GM_Level $0C`), after which
the two play *different content*, so their chip streams diverge for legitimate
reasons (different DAC samples, different SFX). The SEGA-voice DAC loop also
advances `vint` differently on each backend. **Net: the attract demo cannot
give a clean synth/cadence A/B past vint 177.**

**What this leaves as the audio suspect.** With the driver state proven
identical and the synth core proven close (PSG at parity, FM 6.72 dB via
synth_replay), the standing prime suspect for "many sounds wrong" is the one
mechanism NO existing tool exercises: the **(3b) own-backend per-scanline FM/PSG
advance cadence** — `genesis_machine.c` applies all of a scanline's writes then
advances ~3.4 samples in one step, vs clownmdemu advancing between writes. This
collapses rapid music/DAC writes onto one sample boundary.

**Next (Phase 3):** (1) Fix the V-int off-by-one latch (section above) so the
attract demo stays content-synced — this unblocks a clean deep chip-stream +
audio A/B AND fixes the VRAM divergence (both are downstream of the same branch
flip). (2) Make the own backend advance FM/PSG between writes (route through
`mixer.c`'s advance-between-writes path instead of per-scanline batching) and
validate by ear. Tooling added this session: `tools/oracle_parity_audit.py`,
`tools/chip_stream_diff.py`, shared `runner/chip_trace.c`, `--snd-dump-vint`.

**Status:** Synth fixes in; objective parity reached for PSG, FM at core floor.
Awaiting user ear-validation (live build + `ours.wav`/`theirs.wav` A/B). If the
user still hears missing/partial SFX after this, the remaining suspect is the
OWN_BACKEND per-scanline write-collapse (3b) — which the harness does NOT
exercise (it replays line-quantized timing through both cores equally).

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
