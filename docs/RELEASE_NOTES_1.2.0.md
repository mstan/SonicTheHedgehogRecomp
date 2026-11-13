# Sonic the Hedgehog v1.2.0

Adds two optional shortcuts to the original game's level-select, sound-test
and debug features. Both are disabled by default and can be selected
independently in Mods > Gameplay.

With **Easy level select and sound test** enabled, Start on the title screen
opens the original menu directly. The normal stage and sound selections remain
available without re-entering the cheat code at every launch.

With **Easy debug mode** enabled, B toggles the original debug controls during
normal levels and Special Stages without entering a code or holding A while
the stage loads. Attract and credits demos retain their original debug
behavior. The optional debug-transporter escape correction remains separate.

The choices persist in the game profile and apply on restart. Active choices
are included in state and replay compatibility, so loading and rewinding do
not change the selected runtime rules. Existing sound-driver, widescreen,
controller and bug-fix choices are preserved.

Active normal-level and Special Stage disk-state restores continue after the
saved VBlank wait, avoiding an extra audio tick from re-entering the loop top.
Paused states retain their existing pause-loop continuation.

The release retains v1.1.1's quick-load hold, title composition, enemy
visibility and sound-driver fixes. Fresh profiles use adaptive widescreen
with Fit to screen. Existing explicit aspect choices are preserved. The
qualified engine, core and launcher pins remain unchanged.

See [EASY_ACCESS.md](EASY_ACCESS.md) for controls and compatibility details.
The older intermittent title-stripe case remains unconfirmed separately.
