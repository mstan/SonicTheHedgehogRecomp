# Sonic the Hedgehog v0.8.2

- Fixes the reproduced Labyrinth/Scrap Brain3 art corruption by restoring the
  original interrupt ownership of pattern streaming.
- Restores widescreen object, ring, spark and captured-sprite state on rewind.
- Keeps the wide scene through giant-ring act clear and Special Stage entry.
- Makes regular and Special Stage results travel across the selected canvas.
- Fades widescreen world objects with the live scene palette and restores
  the title emblem's original Sonic sprite masking.
- Keeps ending animals visible in wide margins without changing the original
  cutscene simulation or timing.
- Hides the unused Player2 controls and uses the original North American cover.

Fresh profiles use adaptive Fit to screen; explicit saved choices are retained.
Widescreen remains experimental. REV00 is the verified ROM target.

Save states are build-specific. Keep native saves and settings when updating.
