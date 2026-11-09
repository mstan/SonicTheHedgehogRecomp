# Sonic the Hedgehog v1.0.7

This update improves how Sonic, freed animals and enemies appear in the extended widescreen view, and makes Sonic and the HUD participate in level-entry fades.

## Fixes

- **Smooth acceleration after a signpost.** If Sonic had already stopped at the original right-hand camera boundary, his widescreen act-clear continuation could immediately start at full running speed. It now accelerates from rest using the game's acceleration and maximum-speed values. Continuations that begin while Sonic is moving retain their existing velocity. The act-clear timer and actual player object are unchanged.
- **Correct grey-squirrel motion in Marble Zone Act 3.** Freed grey squirrels could stutter when their visible motion continued beyond the original viewport. Their continuation now uses the normal gravity and animation path for that animal instead of treating it as a flying animal.
- **Grounded Caterkiller previews in Marble Zone Act 2.** Caterkillers visible in the extended view could appear suspended above the floor or drop into place during initialization. Their resting preview now follows the floor while the original invisible floor-search initialization runs. This preserves the original object placement, activation timing and collisions.
- **Animated Buzz Bomber wings in Spring Yard Zone Act 1.** Buzz Bombers shown in the extended view before native activation now animate their wings at the original two-frame cadence. This removes the frozen-wing preview without starting their movement or attacks early. Once the original object is initialized, its own animation remains authoritative.
- **Sonic and HUD fade with widescreen level entries.** Sonic and HUD graphics could remain visible while the level scenery was still fading in. They now participate in the inferred widescreen palette fade. The original 4:3 fade behavior is preserved.

## Verification

- Compared eight scene sequences in both native 4:3 and 32:9 against v1.0.6, covering level entries, return-to-title transitions, stationary and moving act-clear starts, freed animals and a staff-roll sequence. All captured native 4:3 images and guest RAM/VRAM samples matched the previous release exactly. The intended widescreen changes matched the reviewed corrected images.
- Verified six save-state scenes both within the running game and after restarting it. Checked rollback self-tests, actual rewind, and 120 Hz act-clear transitions with pause, save, load and resume.
- Checked the packaged launcher and game with fresh Fit to screen settings and saved Off, 21:9 and 32:9 configurations. Saved aspect choices were retained.
- Focused game-video and shared-video tests passed. Matched runtime checks found no reproducible performance regression. These are targeted regression checks, not a new complete playthrough of every route.

## Updating and compatibility

- Fresh installations continue to use adaptive **Fit to screen**. Existing saved settings are retained.
- **32:9 remains experimental.** This update addresses the specific behaviors above; it does not claim that every ultrawide presentation issue has been eliminated.
- Supply your own verified **Sonic the Hedgehog REV00 ROM**. The release does not include a ROM.
- Save states are build-specific. Create new states for v1.0.7 and keep your existing settings when updating.
- This is a game-side presentation update. The shared engine and launcher UI revisions are unchanged from v1.0.6.

QA credit: TheGabeMaster29.
