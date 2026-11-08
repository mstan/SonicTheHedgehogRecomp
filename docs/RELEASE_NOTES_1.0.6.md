# Sonic the Hedgehog v1.0.6

- Keeps widened rings visible through outgoing debug-exit fades.
- Displays complete Caterkiller and Marble boss artwork during entry before
  the native objects take over.
- Stabilizes Star Light foreground columns when the wide camera is clamped.
- Preserves the original horizontal speed during displayed act-clear runoff
  and follows Marble's ground height beyond the native view.
- Keeps released animals on a continuous interpolated trajectory when their
  native object slots are retired or reused.
- Applies the native sprite-fetch limit when composing the title emblem,
  preventing covered water from appearing through it during transitions.

Native game logic, timing and collisions are unchanged. Fresh installations
use adaptive Fit to screen; saved settings are retained. **32:9 remains
experimental.** Supply your own verified REV00 ROM. Save states are
build-specific; preserve native saves and settings when updating.
