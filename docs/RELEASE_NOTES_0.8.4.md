# Sonic the Hedgehog v0.8.4

- Corrects Final Zone's escape framing in widescreen. The original camera
  remains centered after passing the foreground layout's right edge, without
  changing object lifetimes, activation, boss events or player movement.
- Removes solid colored side borders during Special Stage fade-in. The live
  background now fills the selected canvas before the first sprite list,
  preserving the original native-center colors and fade timing.

Native rendering is preserved. Fresh profiles use adaptive Fit to screen;
explicit saved choices are retained. Widescreen remains experimental, and
REV00 is the verified ROM target.

Save states are build-specific. Keep native saves and settings when updating.
