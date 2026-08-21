"""Drive the SAPI5 engine through every setting and measure the result.

Writes %APPDATA%\\EchoGPPC\\settings.ini, renders the same sentence through
real SAPI5, and reports duration, level and sample rate.  A setting that does
not move at least one of those is not reaching the audio, whatever the code
looks like.

    python tools/check_settings.py [x64|x86]
"""

import array
import os
import subprocess
import sys
import tempfile
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARCH = sys.argv[1] if len(sys.argv) > 1 else "x64"
EXE = os.path.join(ROOT, "build_" + ARCH, "bin", "Release", "EchoGPPCTest.exe")
INI = os.path.join(os.environ["APPDATA"], "EchoGPPC", "settings.ini")
OUT = os.path.join(tempfile.gettempdir(), "echo_settings_check")

SENTENCE = "The quick brown fox jumps over the lazy dog."

# (label, extra INI keys for [Voice.textalker], optional text override).
#
# The repeat filter only does anything to a run of identical characters, so it
# gets its own sentence; measured against the default one it looks inert when
# it is working perfectly well.
REPEAT_TEXT = "Score aaaaaaaaaa and bbbbbbbbbb here."

CASES = [
    ("defaults",        {}),
    ("Pitch=4",         {"Pitch": "4"}),
    ("Pitch=60",        {"Pitch": "60"}),
    ("Volume=3",        {"Volume": "3"}),
    ("Volume=15",       {"Volume": "15"}),
    ("Speed=0.5",       {"Speed": "0.5"}),
    ("Speed=2.0",       {"Speed": "2.0"}),
    ("ChipClock=0.7",   {"ChipClock": "0.7"}),
    ("ChipClock=1.6",   {"ChipClock": "1.6"}),
    ("ChipClock=3.0",   {"ChipClock": "3.0"}),   # 8000*3 = 24000, above 22050
    ("Monotone=1",      {"Monotone": "1"}),
    ("Compressed=1",    {"Compressed": "1"}),
    ("FrameRate=3",     {"FrameRate": "3"}),
    ("WordDelay=8",     {"WordDelay": "8"}),
    ("RepeatFilter=99", {"RepeatFilter": "99"}, REPEAT_TEXT),
    ("RepeatFilter=3",  {"RepeatFilter": "3"},  REPEAT_TEXT),
    ("SampleRate=8000", {"SampleRate": "8000"}),
    ("SampleRate=44100", {"SampleRate": "44100"}),
]


def write_ini(overrides):
    values = {
        "Pitch": "24", "Volume": "12", "WordDelay": "0", "RepeatFilter": "99",
        "Speed": "1.0", "ChipClock": "1.0", "SampleRate": "22050",
        "Monotone": "0", "Compressed": "0", "FrameRate": "0",
    }
    values.update(overrides)
    body = ["[General]", "Voice=textalker", "LogLevel=info", "", "[Voice.textalker]"]
    body += ["%s=%s" % kv for kv in values.items()]
    os.makedirs(os.path.dirname(INI), exist_ok=True)
    with open(INI, "w", encoding="utf-8") as f:
        f.write("\n".join(body) + "\n")


def measure(path):
    with wave.open(path) as w:
        n, rate = w.getnframes(), w.getframerate()
        a = array.array("h")
        a.frombytes(w.readframes(n))
    if not len(a):
        return 0.0, 0, 0.0, rate
    peak = max(max(a), -min(a))
    rms = (sum(x * x for x in a) / len(a)) ** 0.5
    return n / float(rate), peak, rms, rate


def main():
    if not os.path.isfile(EXE):
        raise SystemExit("not built: " + EXE)
    os.makedirs(OUT, exist_ok=True)
    wav = os.path.join(OUT, "sapi_textalker.wav")

    print("Driving %s through SAPI5, one setting at a time.\n" % os.path.basename(EXE))
    print("%-20s %8s %8s %8s %8s   %s" % ("setting", "seconds", "rate", "peak", "rms", "verdict"))

    baseline = None
    repeat_baseline = None
    failures = []
    for case in CASES:
        label, overrides = case[0], case[1]
        text = case[2] if len(case) > 2 else None
        write_ini(overrides)
        if os.path.exists(wav):
            os.remove(wav)
        argv = [EXE, "--wav", OUT]
        if text:
            argv += ["--say", text]
        result = subprocess.run(argv, capture_output=True, text=True)
        if not os.path.exists(wav):
            print("%-20s  RENDER FAILED\n%s" % (label, result.stdout[-800:]))
            failures.append(label)
            continue
        secs, peak, rms, rate = measure(wav)
        if baseline is None:
            baseline = (secs, peak, rms, rate)
            verdict = "(baseline)"
        elif text and repeat_baseline is None:
            # First case using this sentence: it is the reference the next one
            # is judged against, not a change in its own right.
            repeat_baseline = (secs, peak, rms, rate)
            verdict = "(baseline for its own sentence)"
        else:
            against = repeat_baseline if text else baseline
            moved = (abs(secs - against[0]) > 0.05 or
                     abs(rms - against[2]) > 60 or
                     rate != against[3])
            verdict = "changed the audio" if moved else "NO EFFECT"
            if not moved:
                failures.append(label)
        print("%-20s %8.2f %8d %8d %8.0f   %s" % (label, secs, rate, peak, rms, verdict))

    write_ini({})   # leave the machine on defaults
    print()
    if failures:
        print("Settings with no measurable effect: %s" % ", ".join(failures))
        return 1
    print("Every setting changed the rendered audio.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
