# Sonic the Hedgehog v1.0.4

- Keeps complete shared sprite artwork available during loading and transitions,
  including Eggmobile effects and the early Labyrinth boss view.
- Preserves initial enemy poses and actual sprite overflow submissions in the
  wider view without changing the original game simulation.
- Corrects widescreen water-surface composition, Star Light seesaw visibility,
  and skyline wrapping and interpolation.
- Includes host scene and audio-event data in build-specific disk states.
- Clears abandoned native dispatch frames after a cold state restore, preventing
  subsequent rollback and rewind from following stale stack pointers.

Fresh installations use adaptive Fit to screen; saved settings are retained.
**32:9 remains experimental.** Supply your own verified REV00 ROM. Save states
are build-specific; preserve native saves and settings when updating.
