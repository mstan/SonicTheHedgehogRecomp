# Sonic the Hedgehog v1.2.1

Fixes short random beeps during play, including while standing idle in
Green Hill.

Sonic 1's music driver runs on the 68000 while the Z80 streams drum samples
to the same YM2612. The game guards that shared chip with a Z80 busy flag.
The engine answered that flag with "not busy" during the vertical-blank
handler, so a music register write could land between the Z80's register
select and its sample write. The sample byte then keyed on an FM note
(a ~1.3 kHz beep of about a third of a second). The Z80 now runs alongside
the handler and the flag reads its real value, as on hardware.

Everything else is unchanged from v1.2.0. Save states from v1.2.0 are
build-specific and are not loaded by this build; native progress and settings
are kept.
