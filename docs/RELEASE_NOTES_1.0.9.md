# Sonic the Hedgehog v1.0.9

This update corrects the widescreen transition from Scrap Brain Act 2's results screen into the Eggman trap sequence.

## Fixes

- **Sonic stays aligned with the controllable player.** Scrap Brain Act 2 returns control after the results instead of automatically leaving the level. Its widescreen display now follows the actual player's position through the camera pan, rather than retaining a separate act-clear runner farther ahead. Letting go or reversing direction responds immediately without a position jump or a second Sonic.
- **A stationary waiting pose during results.** When the original game locks a stopped Sonic against the end boundary, widescreen shows him waiting at that position. The original player animation resumes when control returns. Other acts retain their terrain-following departures and speed-dependent running animation.
- **Eggman is ready when the wide view reaches him.** The parked Scrap Brain Act 2 Eggman uses his original ROM artwork before the native camera activates the object. Presentation hands over to the original actor without advancing his behavior or creating a second actor. The switch also remains visible across its initialization boundary. Removed objects are not recreated, and the original trap timing and floor collapse remain authoritative.

## Verification

- Triggered the native signpost, allowed the full normal score tally to finish, and exercised hands-off waiting, Left/Right reversal and the trap approach. Native 4:3 images and gameplay RAM matched v1.0.8; widescreen changes were checked separately.
- Checked paused setups after save/load and a fresh launch, plus actual 120 Hz presentation through control return, Eggman's activation, the floor collapse and entry into Scrap Brain Act 3.
- The results, control-return and Eggman-approach setups received a visual gameplay check. Release qualification also covers packaged settings, current-build state/rewind behavior and production runtime. These are targeted checks, not a new complete playthrough of every route.

## Updating and compatibility

- Fresh installations use adaptive **Fit to screen**. Existing saved settings and aspect choices are retained.
- **32:9 remains experimental.** These fixes do not establish that every ultrawide scene is correct.
- Supply your own verified **Sonic the Hedgehog REV00 ROM**; no ROM is included. REV01 remains unverified.
- Save states are build-specific. Create new states for v1.0.9; preserve your settings and native saves when updating.
- The shared engine and launcher UI revisions are unchanged from v1.0.8. Native 4:3 behavior, controls, collision and event timing remain unchanged.

QA credit: TheGabeMaster29.
