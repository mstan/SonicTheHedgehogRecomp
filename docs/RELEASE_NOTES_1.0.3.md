# Sonic the Hedgehog v1.0.3

- Adopts moving platforms by their placement origin when widescreen loading
  takes over, preventing duplicate platform objects.
- Uses complete ROM artwork for early capsule and giant-ring rendering, and
  keeps capsule ink independent of the boss hit flash in the wider view.
- Continues Sonic's act-end run beyond the visible edge. Capsule animals keep
  their native lifetimes while their graphics continue through wide margins,
  preserving the original results timing.
- Retains the outgoing background and scroll data until the results fade is
  finished, even after the game selects the next zone.
- Extends Marble's magma curtains and lava-wall rear fill through the left
  margin while retaining foreground stonework and original collision data.

Fresh installations use adaptive Fit to screen; saved settings are retained.
**32:9 remains experimental.** Supply your own verified REV00 ROM. Save states
are build-specific; preserve native saves and settings when updating.
