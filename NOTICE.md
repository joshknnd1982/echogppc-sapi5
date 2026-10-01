# Notices

The code written for this project is licensed under the MIT License (see [LICENSE](LICENSE)). The material
below is not covered by that licence and stays under its own terms.

## BSD 3-Clause (third-party portions)

The COM layer is derived from Gozaltech's BestSpeech SAPI5 wrapper (see [CREDITS.md](CREDITS.md)): `src/com.hpp`, `src/com.cpp`, `src/registry.*`, `src/utils.hpp` and `src/ISpDataKeyImpl.*` are close to that project's, with the namespace changed and a registry-root parameter added, and `src/IEnumSpObjectTokensImpl.*` and `src/voice_token.*` retain most of that project's text. Those files are not covered by the MIT License; they keep Gozaltech's notice and stay under the BSD 3-Clause terms below.

`bin/echotalk32.dll` and `bin/echotalk64.dll` are Jayson Smith's EchoTalk builds, with the MAME and Fake6502 code vendored inside them; their notices are carried in the third-party notices section further down.

```
Copyright (c) Jayson Smith (EchoTalk — the Echo II emulator library this
              project drives, and the NVDA driver used as its reference)
Copyright (c) Frank Palazzolo, Aaron Giles, Jonathan Gevaryahu,
              Raphael Nabet, Couriersud, Michael Zapf
              (MAME TMS5220 speech synthesizer emulation, vendored by EchoTalk)
Copyright (c) Gozaltech (BestSpeech SAPI5 wrapper, from which the COM layer
              of this project is derived)

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Files not covered by the MIT License

Two sets of files in this repository are under different terms. Read this
section before redistributing anything from here.

### 1. The Textalker ROM images — PROPRIETARY, NOT LICENSED

    bin/textalker.ram.bin
    bin/textalker.obj.bin
    bin/textalker_v13.ram.bin
    bin/textalker_v13.obj.bin

These are Textalker, proprietary software published by Street Electronics
Corporation. The version 3.1.3 release additionally carries a 1986 American
Printing House for the Blind copyright.

They are NOT covered by the MIT License, by the BSD licence above, nor by any
other licence. No permission to redistribute them has been granted by their
copyright holders, and none is claimed or implied here. They are executed under
6502 emulation rather than reimplemented.

The upstream EchoTalk project deliberately does not ship these files and asks
users to supply their own; its own notices state that "any distributed build
needs the question of how the end user supplies these files settled separately".
That question is not settled by this repository. They are included here for the
convenience of people who already own Textalker, and their presence is not a
grant of any right to copy or redistribute them.

If you are a rights holder and want these removed, please open an issue.

### 2. The EchoTalk NVDA driver — GPL-2.0

    reference/echotalk-nvda-driver/__init__.py

Part of the EchoTalk NVDA add-on by Jayson Smith, distributed under the GNU
General Public License version 2 as NVDA add-ons must be. It is included as
reference documentation of the echotalk C ABI and is not compiled into, linked
with, or required by anything this project builds.

### 3. Third-party notices carried by the emulator

bin/THIRD_PARTY_LICENSES.txt reproduces the notices required by the code
vendored inside echotalk32.dll and echotalk64.dll: MAME's TMS5220 emulation
(BSD-3-Clause) and Mike Chambers' Fake6502 (credit given as its author asks).
That file must accompany any redistribution of those DLLs, and the installer
installs it.
