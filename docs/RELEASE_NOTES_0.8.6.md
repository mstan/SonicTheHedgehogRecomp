# Sonic the Hedgehog v0.8.6

- Keeps level geometry and objects aligned when skipping an attract demo in
  widescreen, including high-refresh presentation.
- Joins the title-screen water into one continuous background while retaining
  the diagonal pop-in correction and original logo/sprite masking.
- Continues visible damage explosions around Eggman's escaping craft in the
  extended view, without adding guest objects, RNG calls or audio.
- Keeps Eggman's complete craft visible through Final Zone's ending fade.

Native rendering and original game-state timing are preserved. Fresh profiles
use adaptive Fit to screen; explicit saved choices are retained. Widescreen
remains experimental, and REV00 is the verified ROM target.
