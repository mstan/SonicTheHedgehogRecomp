# Sonic the Hedgehog v1.0.8

This update improves Sonic's widescreen act-clear departure and the final Green Hill staff-roll entry.

## Fixes

- **Ground contact throughout act-clear departures.** Sonic's displayed continuation beyond the original camera boundary now follows the level's floor height in every zone, rather than applying terrain tracking only in Marble Zone Act 3. Both foot sensors follow slopes and lower terrain, and the displayed player falls when support ends. Scrap Brain Act 2 also accounts for its scripted floor and the surviving solid span as that floor breaks away. The actual player, collision map, controls and act-clear timing are unchanged.
- **Acceleration from very low entry speeds.** A continuation that began with a small positive velocity could keep that velocity throughout the departure. It now accelerates toward the game's current maximum speed, including when Sonic starts near Scrap Brain Act 2's right camera lock. Starting from rest remains gradual, and existing entry speeds above the normal maximum are retained.
- **Running animation matches the displayed departure.** Long departures could revert to a slow walking pose even while the displayed Sonic kept moving quickly. The continuation now advances its own speed-dependent walk/run animation and floor-angle pose. Its graphics come from the corresponding original Sonic pattern cues, so the clamped player's last VRAM upload cannot leave the running pose with walking tiles.
- **Motobug visible during the final Green Hill staff-roll fade.** In widescreen, the resting Motobug now appears with the entry fade and stays visible through native initialization. The preview yields to the original sprite once it owns the display and cannot revive the placement after the original object has gone. Native 4:3 loading and gameplay timing are preserved.
- **Native visibility remains independent of the displayed continuation.** Moving or falling outside the wide view no longer changes the actual player's visibility flag merely because its displayed pose has different bounds.

## Verification

- Compared twelve scene sequences in native 4:3 and 32:9 with v1.0.7: level entries, title returns, signpost and capsule departures, tiny positive entry speeds, the Scrap Brain camera pan and the final Green Hill staff-roll sequence. Checks compare original guest RAM/VRAM and native 4:3 output separately from the intended widescreen changes.
- Focused game-video tests cover floor support in eight zone configurations, acceleration from zero and fractional velocities, retained above-maximum speed, running graphics, shrinking platform support, native visibility and Motobug initialization/removal.
- Release qualification includes current-build save/load after restarting, actual rewind and rollback checks, 120 Hz presentation, fresh and saved launcher settings, and matched production runtime checks. These are targeted regression checks, not a complete new playthrough of every route.

## Updating and compatibility

- Fresh installations use adaptive **Fit to screen**. Existing saved settings and aspect choices are retained.
- **32:9 remains experimental.** These fixes do not establish that every ultrawide scene is correct.
- Supply your own verified **Sonic the Hedgehog REV00 ROM**; no ROM is included. REV01 remains unverified.
- Save states are build-specific. Create new states for v1.0.8; preserve your settings and native saves when updating.
- The shared engine and launcher UI revisions are unchanged from v1.0.7.

QA credit: TheGabeMaster29.
