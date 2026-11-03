# Sonic the Hedgehog v0.8.1

This bug-fix release restores the v0.7.6 corrections omitted from v0.8.0 and
keeps its high-refresh and audio options.

- Corrects title-water seams, returning-title scenery, level-select margins,
  wide fades, ending presentation and left-boundary background scrolling.
- Keeps recorded attract/staff-roll simulation native at wider aspects.
  Title cards retain native timing while entering, holding and departing
  across the actual canvas.
- Retains wide sprites through fades by matching the active sprite-table
  links. Prepares button/signpost art and previews the reproduced staff-roll
  and Final Zone objects without advancing their original game logic.
- Corrects fractional GHZ background clamping with high-refresh presentation.
- Enables adaptive Fit to screen for fresh profiles, preserving saved choices.

REV00 remains the verified ROM target. Enhancement suggestions, including
REV01 support and optional original-game bug fixes, remain tracked separately.
