Sonic the Hedgehog v1.2.1

Generated native calls now preserve explicit and captured return-slot skips,
including conditional paths and reconstructed stack frames. Acknowledging a
running FM Timer B preserves its expiry instead of restarting the deadline.

Audio output checks both stereo channels before repeating a stalled buffer.
Tails without a reliable common period decay smoothly instead of forming an
artificial sustained tone.

Fresh profiles use adaptive Widescreen / Fit to screen. Existing saved choices
and native campaign saves are preserved. Save states are build-specific.
