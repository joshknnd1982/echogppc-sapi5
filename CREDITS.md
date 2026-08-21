# Credits

This project is a SAPI5 shell around somebody else's emulator, built on the
bones of somebody else's SAPI5 wrapper. Neither is incidental: one is
everything you actually hear, the other is the COM architecture that carries
it. What is original here is the engine that sits between them, the
configuration utility, and the installer.

## EchoTalk — the emulator

<https://github.com/jayson-smith/echotalk> (see the add-on's own read me)

Copyright © Jayson Smith. BSD-3-Clause for the library and tools; the NVDA
add-on that wraps it is GPL-2.0, as NVDA add-ons must be.

**Everything that produces sound here is that project's work.**
`echotalk32.dll` and `echotalk64.dll` are its standalone builds: a MOS 6502
emulator running the real, unmodified Textalker firmware, driving a port of
MAME's TMS5220 speech chip emulation. This project does not synthesise
anything. It boots those images, pushes settings into them, and hands the
resulting samples to SAPI.

The C ABI this wrapper drives — `echotalk_create`, `echotalk_speak`,
`echotalk_read`, `echotalk_next_index`, `echotalk_set_clock_multiplier` and
the rest, all 44 of them — is that project's, unchanged.

Its NVDA driver (`reference/echotalk-nvda-driver/__init__.py`, GPL-2.0) was
the reference for how the library should be sequenced, and several of its
hard-won details are carried straight over into the SAPI5 engine:

- that only one instance may exist per process, because the 6502 core keeps
  its registers in globals;
- that the output sample rate has to be treated as a floor rather than a fixed
  value, because the chip really produces `8000 × the chip clock` and asking
  for less runs real detail through a resampler with no anti-aliasing filter;
- that Ctrl-D, Ctrl-E and Ctrl-V must be stripped from anything that came off
  the screen, or a document containing one silently changes the voice;
- that pitch and monotone are one Textalker setting and not two, so emitting
  `nP` where the voice is meant to be flat quietly un-flattens it.

The read me in that add-on is also the source for what each setting does and
why, and is installed alongside this engine.

## BestSpeech SAPI5 wrapper — the SAPI5 architecture

<https://github.com/gozaltech/bstspeech-sapi>

Copyright © Gozaltech.

The COM layer here is derived from that project: the `IUnknown`
implementation, the class factory and registrar, the `ISpDataKey` and
`IEnumSpObjectTokens` implementations, the in-memory voice token, and the
overall shape of the engine. `src/com.hpp`, `src/com.cpp`, `src/registry.*`,
`src/utils.hpp` and `src/ISpDataKeyImpl.*` are close to that project's, with
the namespace changed and a registry-root parameter added.

What is **not** carried over is its transport. BestSpeech's engine is 32-bit
only, so that wrapper needs a 32-bit surrogate process and a named-pipe bridge
to serve 64-bit hosts. EchoTalk ships a native build for each architecture, so
both engines here are plain in-process COM servers and the bridge is gone
entirely.

## MAME — TMS5220 speech synthesizer emulation

Copyright © Frank Palazzolo, Aaron Giles, Jonathan Gevaryahu, Raphael Nabet,
Couriersud, Michael Zapf. BSD-3-Clause.

Vendored inside the echotalk DLLs, ported out of MAME's `device_t` framework
into standalone C by the EchoTalk project. The LPC synthesis core — lattice
filter, chirp table, interpolation, parameter decode — is MAME's logic. Full
notices are in `bin/THIRD_PARTY_LICENSES.txt`.

MAME's Apple II Echo II card emulation (© R. Belmont, ready logic traced by
Lord Nightmare and Tony Diaz) documented the card's bus protocol and `/READY`
handshake for that port.

## Fake6502 — the CPU

Copyright © Mike Chambers, 2011. Public domain, credit given at the author's
request.

The 6502 emulation that runs Textalker itself. Also vendored inside the
echotalk DLLs.

## Textalker — the voice

Published by Street Electronics Corporation; version 3.1.3 additionally
carries a 1986 American Printing House for the Blind copyright.

Textalker is the actual speech program: the letter-to-sound rules, the
command set, the pitch and volume handling, all of it. It is executed here
under emulation, entirely unmodified. It is **proprietary and not licensed for
redistribution** — see `LICENSE` for what that means for this repository.

## Street Electronics Corporation — the hardware

The Echo II, the card this all reconstructs. For a lot of people it was the
first speech synthesizer they ever heard, and for some of them it was the only
one they had for years. That is the whole reason any of this exists.
