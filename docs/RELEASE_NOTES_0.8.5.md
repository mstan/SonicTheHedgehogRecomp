# Sonic the Hedgehog v0.8.5

- Removes diagonal pop-in/out in the title screen water at extended widescreen
  edges, including later32:9 scroll phases. The margins use the live background
  layout instead of repeating the original streaming cache.
- Keeps the damaged Final Zone escape cockpit visible with its craft across
  the wide view. The visual continuation preserves original game state,
  explosion behavior and the ending trigger.

Native rendering and the title's original center composition are preserved.
Fresh profiles use adaptive Fit to screen; explicit saved choices are retained.
Widescreen remains experimental, and REV00 is the verified ROM target.
