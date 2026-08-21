# EchoTalk NVDA driver — reference copy

`__init__.py` is the speech driver from Jayson Smith's **EchoTalk** NVDA
add-on. It is here as documentation of the `echotalk` C ABI: it declares every
entry point with its argument types, and its comments record a number of
things about the library that are not obvious and were expensive to learn.

**It is licensed GPL-2.0**, as NVDA add-ons must be — not under this
repository's BSD-3-Clause licence. It is not compiled into, linked with, or
required by anything this project builds. Nothing here is derived from it in
the copyright sense; it was read, not copied.

The specific things it taught this project, all of which are carried into
`src/echo_core.cpp` and `src/ISpTTSEngineImpl.cpp`:

* Only one emulator instance may exist per process — the 6502 core keeps its
  registers in globals, so `echotalk_create` refuses a second one.
* The output sample rate is a floor, not a fixed value: the chip really
  produces `8000 × the chip clock`, and asking for less runs detail the chip
  generated through a resampler with deliberately no anti-aliasing filter.
* Ctrl-D, Ctrl-E and Ctrl-V have to be stripped from screen text, or a
  document containing one silently reconfigures the voice.
* Pitch and monotone are one Textalker setting rather than two, so sending
  `nP` where the voice should be flat quietly un-flattens it.
* Declaring `argtypes` is not optional detail — guessing works for ints on
  64-bit and breaks silently for doubles and for pointers above 2 GB.

See [CREDITS.md](../../CREDITS.md).
