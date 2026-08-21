# Echo GPPC SAPI5

A Microsoft SAPI5 text-to-speech engine, 32-bit and 64-bit, for the emulated
Street Electronics Echo II — a TMS5220 speech chip and a 6502 running the real
Textalker ROMs, both emulated from MAME components by `echotalk`.

Every SAPI5 application on the machine gets the Echo as an ordinary voice:
screen readers, Narrator, browsers, Balabolka, anything that speaks.

> ### A note on the Textalker ROMs
>
> The four `bin/textalker*.bin` files are **Textalker, proprietary software
> published by Street Electronics Corporation**; the 3.1.3 images additionally
> carry a 1986 American Printing House for the Blind copyright. They are not
> covered by this project's licence or by any other, and no permission to
> redistribute them has been granted by their copyright holders.
>
> They are here for the convenience of people who already own Textalker. Their
> presence is not a grant of any right to copy or redistribute them. The
> upstream EchoTalk project deliberately ships without them and asks users to
> supply their own. See [LICENSE](LICENSE) for the full statement.
>
> If you are a rights holder and want them removed, please open an issue.

## Status

| Piece | State |
|---|---|
| 64-bit SAPI5 engine | done |
| 32-bit SAPI5 engine | done |
| All parameters exposed | done |
| Configuration utility | done |
| Inno Setup installer | done |

## Voices

Voices are discovered from the Textalker ROM pairs found on disk, not from a
hard-coded table and not from the registry. Dropping in another
`<stem>.ram.bin` + `<stem>.obj.bin` pair adds a voice, labelled with the
version banner read out of the ROM itself.

| Voice | ROM stem | From | Language |
|---|---|---|---|
| Echo GPPC Textalker 3.1.3 | `textalker` | 1986 | English (en-US) |
| Echo GPPC Textalker 1.3 | `textalker_v13` | 1981 | English (en-US) |

English is the only language the Echo has ever had: Textalker's
letter-to-sound rules are English and no other ROM exists.

## Architecture

The `echotalk` library ships a **native build for each architecture**, so both
SAPI5 engines are plain in-process COM servers that load the matching
`echotalk32.dll` / `echotalk64.dll` directly. No surrogate process and no
named-pipe bridge — unlike the BestSpeech engine this was adapted from, which
needed both because its engine was 32-bit only. That reference tree is kept
under `reference/bestspeech/`.

**The emulator is a hard singleton.** `echotalk_create` refuses a second
instance in a process: the emulated 6502 keeps its registers in globals. A
SAPI host can easily hold several voice objects at once, so every engine
object shares **one machine behind a critical section** (`src/echo_core.cpp`).
An utterance takes a `Core::Session`, which locks the core and re-boots the
machine if a different ROM is wanted; booting costs well under 10 ms. This is
covered by `EchoGPPCTest --stress`.

### Files

| File | Purpose |
|---|---|
| `src/echo_core.*` | Loads the emulator, owns the shared machine and the lock |
| `src/echo_settings.*` | INI settings, with change detection |
| `src/echo_log.*` | Diagnostic log |
| `src/ISpTTSEngineImpl.*` | The SAPI5 engine: `Speak`, `GetOutputFormat` |
| `src/IEnumSpObjectTokensImpl.*` | Voice enumeration from ROM files |
| `src/voice_token.*` | Per-voice attributes, built in memory |
| `src/sapi_main.cpp` | COM entry points and registration |
| `src/sapi_selftest.cpp` | End-to-end test harness |
| `src/config_ui.*` | The configuration utility |

## Configuration utility

`EchoGPPCConfig.exe` adjusts the settings below without touching the INI by
hand. Changes are written as they are made and apply to the next thing spoken,
so a screen reader picks them up without being restarted.

Its controls, in tab order:

| Control | Type | Range |
|---|---|---|
| Voice | combo box | the installed Textalker ROMs |
| Rate | combo box | 0.25x - 4.00x |
| Pitch | edit with spin buttons | 0 - 63 |
| Volume | edit with spin buttons | 0 - 15 |
| Delay between words (Textalker 3 only) | edit with spin buttons | 0 - 15 |
| Repeat-character filter | edit with spin buttons | 0 - 99 |
| Chip clock | combo box | 0.25x - 4.00x |
| Output sample rate | combo box | 8000 - 48000 Hz |
| Monotone | check box | |
| Compressed speech | check box | |
| Frame rate | combo box | 0 - 3 |
| Log detail | combo box | off - everything |
| Speak test, Reset to defaults, Open log folder, Close | buttons | |

Settings are **per voice**: Textalker 3.1.3 and 1.3 each keep their own, and
switching the Voice control saves the one being left before loading the next.

### Accessibility

The people most likely to want an emulated Echo II use a screen reader, so
this is a requirement rather than a polish item:

* Standard Win32 controls throughout -- combo boxes, edit boxes with spin
  buttons, check boxes -- all of which expose name, role and value through
  MSAA without any help from the application.
* Every label sits immediately before its control in the dialog template,
  which is what makes each field announce its own name.
* **Nothing is ever disabled.** A disabled control drops out of the tab order
  entirely, so the two Textalker-3-only fields stay enabled and say so in
  their labels instead.
* Values are shown in the library's own units, so what is read aloud is the
  number that ends up in the file. No percentages to convert.
* Unique mnemonics across the whole dialog, so every Alt+letter goes straight
  to one control.
* Ranges are spelled out in the labels, because a spin control announces its
  value but not its limits.
* A status line reports what just happened and fires an accessibility event so
  it is announced.

`python tools/check_msaa.py` verifies all of this by asking MSAA -- the
interface NVDA actually uses -- what each control reports, and walks the real
tab order via `GetNextDlgTabItem`.

## Settings

Settings live in `%APPDATA%\EchoGPPC\settings.ini` — a plain file, **not the
registry**. The engine reads no registry key to speak. The registry is used
only to register the COM server at install time, which is SAPI's own discovery
mechanism and unavoidable.

The file is re-read whenever its timestamp or size moves, so a change takes
effect on the next utterance without restarting the host application.

Settings are per-voice, under `[Voice.<rom stem>]`:

| Key | Range | Meaning |
|---|---|---|
| `Pitch` | 0–63 | Textalker's pitch (`nP`) |
| `Volume` | 0–15 | Textalker's volume (`nV`) — low is fuzzy, high distorts |
| `WordDelay` | 0–15 | Delay between words (`nD`) — **Textalker 3 only** |
| `RepeatFilter` | 0–99 | Collapse repeated characters (`nR`) — **Textalker 3 only** |
| `Speed` | 0.25–4.0 | Continuous rate, pitch preserved |
| `ChipClock` | 0.25–4.0 | TMS5220 clock — speed *and* pitch, like tape speed |
| `SampleRate` | 8000…48000 | Output rate; a **floor**, see below |
| `Monotone` | 0/1 | Flat pitch (`nF` instead of `nP`) |
| `Compressed` | 0/1 | Textalker's own compressed/expanded speech |
| `FrameRate` | 0–3 | The chip's own four-step frame rate |
| `Raw` | 0/1 | 1 disables UTF-8 / typographic folding (diagnostic) |
| `LetterMode` | 0–1 | Accepted by the library, no audible effect in ABI 6 |
| `Punctuation` | 0–2 | Accepted by the library, no audible effect in ABI 6 |
| `ChunkSize` | 8–80 | Textalker's text buffer |
| `IndexBreak` | 0/1 | Index-mark behaviour |

`[General]` holds `Voice` (the default ROM stem) and `LogLevel`
(`off`/`error`/`warn`/`info`/`debug`/`trace`, default `info`).

### Sample rate is a floor, not a fixed value

The chip really produces `8000 × ChipClock`. Asking for less would downsample
through a resampler that deliberately has no anti-aliasing filter, folding
real detail back into the audible band as noise. So the configured rate is
raised to meet the chip when the clock outruns it — at `ChipClock=3.0` the
engine negotiates 24000 Hz — and the chosen rate applies again as soon as the
clock comes back down.

Because SAPI negotiates the audio format once per stream, a change to
`SampleRate` or `ChipClock` applies to the **next** stream a host opens.
Everything else applies to the very next utterance.

### How SAPI's own controls combine

- **Rate** (−10…+10) multiplies `Speed` as `2^(rate/5)`, so 0 is exactly
  `Speed`, ±5 is half/double and ±10 spans the library's full 0.25×–4× range.
- **Volume** (0–100) is applied as a clean digital gain, *not* through
  Textalker's volume register. The register stays available as the
  "character" control, since low values are meant to sound fuzzy and high ones
  distorted — mapping the host's volume onto it too would leave no way to get
  clean quiet speech.
- **Pitch** rides on top of `Pitch` at about two Textalker steps per SAPI unit.

## Events

Bookmarks are placed at exact byte offsets. Word-boundary events use the
library's own index marks — a marker is inserted before each word and comes
back when that word's audio is produced — so highlighting follows real timing
rather than a guess. Marks are only inserted when a host asks for the events.

## Building

Needs CMake 3.15+, Visual Studio 2022 (or Build Tools) with the C++ workload,
and the Windows SDK.

```bat
build_all.bat
```

That builds both architectures, stages `output\`, and compiles the installer
with Inno Setup if it is present (it is looked for under `%LOCALAPPDATA%` as
well as Program Files, since Inno installs per-user by default). Without Inno
Setup the staged files are still complete and usable.

The staged tree:

```
output\
    EchoGPPCSAPI.dll         64-bit engine
    EchoGPPCTest.exe         64-bit test harness
    echotalk64.dll
    textalker*.bin           ROM pairs, shared by both architectures
    x86\
        EchoGPPCSAPI.dll     32-bit engine
        EchoGPPCTest.exe
        echotalk32.dll
```

The engine looks for ROMs beside itself and then one directory up, which is
why the 32-bit build in `x86\` needs no copy of them.

## Installing

Run `output\EchoGPPC_SAPI5_Setup.exe`. It needs administrator rights, because
SAPI reads voice enumerators only from HKLM.

It installs both engines, both Textalker ROM pairs, both emulator DLLs, the
configuration utility and the test harness; registers the 64-bit engine in the
64-bit registry view and the 32-bit engine in the 32-bit one; and offers a
desktop shortcut plus a Start Menu group.

```
C:\Program Files\Echo GPPC SAPI5    EchoGPPCSAPI.dll         64-bit engine        (registered, 64-bit view)
    EchoGPPCConfig.exe       configuration utility
    EchoGPPCTest.exe         test harness
    echotalk64.dll
    echotalk32.dll
    textalker*.bin           ROM pairs, shared by both architectures
    THIRD_PARTY_LICENSES.txt
    readme.html
    x86        EchoGPPCSAPI.dll     32-bit engine        (registered, 32-bit view)
        EchoGPPCTest.exe
```

On 32-bit Windows only the 32-bit half is installed.

### Installer behaviour worth knowing

* **It never closes running applications.** `CloseApplications=no` is
  deliberate: the most likely host for this engine is a screen reader, and
  shutting one down mid-install would leave a blind user with no speech and a
  modal dialog they cannot read. A file that is in use is scheduled for
  replacement on the next restart instead.
* **Settings survive uninstall.** `%APPDATA%\EchoGPPC\settings.ini` is left
  in place, because someone reinstalling almost always wants their voice back
  the way it was.
* **It logs everything.** `SetupLogging=yes`, and at the end of the run the log
  is copied to `%LOCALAPPDATA%\EchoGPPC\logs\install-<timestamp>.log`, next
  to the engine's own logs, so a bug report can carry both.
* **It checks its own work.** After installing, setup reads back the voice
  enumerator from both registry views. If either is missing it says so and
  prints the exact `regsvr32` commands to fix it, rather than reporting a
  success that produced no voices.

### Installer accessibility

The stock Inno Setup wizard is built from standard Win32 controls and is read
correctly as it stands, so the work here is in not undermining it: no custom
or owner-drawn pages, no images carrying information, `WizardStyle=classic`
for the plain long-established layout, and every page left enabled so none is
just a title and a Next button.

One change was needed. The tasks list is a custom check-list control that
hands MSAA the raw caption including any `&` accelerator marker -- unlike a
standard checkbox, where Windows strips it -- so the desktop-shortcut task
description has no accelerator in it.

`python tools/check_installer_a11y.py` walks the wizard page by page and
reports the MSAA name and role of every control, including the items inside
the tasks list. **It must run elevated**: Windows blocks a lower-integrity
process from reading an elevated window's accessibility tree, and an
unelevated run sees nothing at all -- which looks exactly like an inaccessible
wizard.

## Registering by hand

Only needed when working from the build tree rather than the installer.

**Registration must be elevated.** SAPI reads voice token enumerators only
from `HKLM`; an otherwise identical registration under `HKCU` is silently
ignored, so `DllRegisterServer` fails with `E_ACCESSDENIED` rather than
reporting a success that delivers no voices.

From an elevated prompt:

```bat
regsvr32 output\EchoGPPCSAPI.dll
regsvr32 output\x86\EchoGPPCSAPI.dll
```

To remove:

```bat
regsvr32 /u output\EchoGPPCSAPI.dll
regsvr32 /u output\x86\EchoGPPCSAPI.dll
```

## Testing

```bat
output\EchoGPPCTest.exe --list
output\EchoGPPCTest.exe --wav C:\temp
output\EchoGPPCTest.exe --speak
output\EchoGPPCTest.exe --speak --bookmarks
output\EchoGPPCTest.exe --stress 6
```

`--bookmarks` needs `--speak`: SAPI advances its event queue from the audio
device's playback position, so a file stream delivers no events to the
application even though the engine raises them.

Two scripts drive the whole thing from Python:

```bat
python tools\render_samples.py          rem straight from the DLL, no SAPI
python tools\check_settings.py x64      rem every setting, through SAPI
```

`check_settings.py` renders the same sentence once per setting and reports
duration, level and sample rate — a setting that moves none of them is not
reaching the audio.

## Logs

`%LOCALAPPDATA%\EchoGPPC\logs\echogppc-<arch>-<pid>.log`, one file per process
(several hosts commonly load the engine at once). Rolls at 4 MB. Set
`LogLevel=debug` in the INI for per-fragment detail, `trace` for more.

## Licence and credits

BSD 3-Clause — see [LICENSE](LICENSE), which also sets out the two sets of
files that are **not** under it: the proprietary Textalker ROM images, and the
GPL-2.0 EchoTalk NVDA driver kept under `reference/` as ABI documentation.

[CREDITS.md](CREDITS.md) covers who actually built what. The short version:
every sound this makes is the EchoTalk project's work, the COM layer is
derived from the BestSpeech SAPI5 wrapper, the speech chip is MAME's, and the
voice itself is Street Electronics' Textalker running unmodified under
emulation.
