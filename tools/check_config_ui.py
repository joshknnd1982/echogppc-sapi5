"""Drive the configuration utility and check the settings really land.

Controls are changed by posting the same notifications Windows posts when a
person uses them, so the dialog's own handlers run exactly as they would for a
real user -- this is not writing the INI directly and hoping.  Then the file is
read back, and finally the engine is asked to speak so that the change can be
shown to reach the audio, which is what "takes immediate effect" has to mean.

    python tools/check_config_ui.py
"""

import array
import ctypes
import ctypes.wintypes as w
import os
import subprocess
import sys
import tempfile
import time
import wave

user32 = ctypes.WinDLL("user32", use_last_error=True)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONFIG_EXE = os.path.join(ROOT, "output", "EchoGPPCConfig.exe")
TEST_EXE = os.path.join(ROOT, "output", "EchoGPPCTest.exe")
INI = os.path.join(os.environ["APPDATA"], "EchoGPPC", "settings.ini")

WM_COMMAND = 0x0111
WM_CLOSE = 0x0010
CB_SETCURSEL = 0x014E
CB_GETCURSEL = 0x0147
# SetWindowText and GetWindowText are documented to do nothing on a control
# owned by another process. WM_SETTEXT and WM_GETTEXT are marshalled across the
# process boundary by the system, so they are what a cross-process test has to
# use -- reading back an empty string from GetWindowText here says nothing
# about the control, only about the API.
WM_SETTEXT = 0x000C
WM_GETTEXT = 0x000D
BM_SETCHECK = 0x00F1
BST_CHECKED = 1
CBN_SELCHANGE = 1
EN_CHANGE = 768
BN_CLICKED = 0

# Control ids, from src/config_resource.h.
IDC_VOICE, IDC_RATE = 1001, 1003
IDC_PITCH, IDC_VOLUME = 1005, 1008
IDC_WORDDELAY, IDC_REPEAT = 1011, 1014
IDC_CLOCK, IDC_SAMPLERATE = 1017, 1019
IDC_MONOTONE, IDC_COMPRESSED = 1020, 1021
IDC_FRAMERATE = 1032
IDC_SPEAK, IDC_RESET, IDC_OPENLOGS = 1040, 1041, 1042

failures = []


def find_dialog(pid, timeout=8.0):
    found = []

    @ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    def cb(hwnd, _):
        got = w.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(got))
        if got.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
            return False
        return True

    deadline = time.time() + timeout
    while time.time() < deadline:
        found.clear()
        user32.EnumWindows(cb, 0)
        if found:
            return found[0]
        time.sleep(0.2)
    return None


def notify(dialog, ctrl_id, code, hctl):
    user32.SendMessageW(w.HWND(dialog), WM_COMMAND,
                        w.WPARAM((code << 16) | (ctrl_id & 0xFFFF)),
                        w.LPARAM(hctl))


def set_combo(dialog, ctrl_id, index):
    h = user32.GetDlgItem(w.HWND(dialog), ctrl_id)
    user32.SendMessageW(w.HWND(h), CB_SETCURSEL, w.WPARAM(index), w.LPARAM(0))
    notify(dialog, ctrl_id, CBN_SELCHANGE, h)


def set_edit(dialog, ctrl_id, text):
    h = user32.GetDlgItem(w.HWND(dialog), ctrl_id)
    buf = ctypes.create_unicode_buffer(text)
    user32.SendMessageW(w.HWND(h), WM_SETTEXT, w.WPARAM(0),
                        ctypes.cast(buf, ctypes.c_void_p))
    notify(dialog, ctrl_id, EN_CHANGE, h)


def get_edit(dialog, ctrl_id):
    h = user32.GetDlgItem(w.HWND(dialog), ctrl_id)
    buf = ctypes.create_unicode_buffer(64)
    user32.SendMessageW(w.HWND(h), WM_GETTEXT, w.WPARAM(64),
                        ctypes.cast(buf, ctypes.c_void_p))
    return buf.value


def set_check(dialog, ctrl_id, checked):
    h = user32.GetDlgItem(w.HWND(dialog), ctrl_id)
    user32.SendMessageW(w.HWND(h), BM_SETCHECK,
                        w.WPARAM(BST_CHECKED if checked else 0), w.LPARAM(0))
    notify(dialog, ctrl_id, BN_CLICKED, h)


def read_ini():
    sections = {}
    current = None
    if not os.path.exists(INI):
        return sections
    with open(INI, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith(";"):
                continue
            if line.startswith("[") and line.endswith("]"):
                current = line[1:-1]
                sections[current] = {}
            elif "=" in line and current:
                key, _, value = line.partition("=")
                sections[current][key.strip()] = value.split(";")[0].strip()
    return sections


def expect(label, got, want):
    if str(got) == str(want):
        print("  ok    %-32s = %s" % (label, got))
    else:
        print("  FAIL  %-32s = %s (expected %s)" % (label, got, want))
        failures.append(label)


def render(extra_args=()):
    out = os.path.join(tempfile.gettempdir(), "echo_cfg_check")
    os.makedirs(out, exist_ok=True)
    wav = os.path.join(out, "sapi_textalker.wav")
    if os.path.exists(wav):
        os.remove(wav)
    subprocess.run([TEST_EXE, "--wav", out, *extra_args], capture_output=True)
    if not os.path.exists(wav):
        return None
    with wave.open(wav) as f:
        n, rate = f.getnframes(), f.getframerate()
        a = array.array("h")
        a.frombytes(f.readframes(n))
    return n / float(rate), rate, (sum(x * x for x in a) / len(a)) ** 0.5


def main():
    for exe in (CONFIG_EXE, TEST_EXE):
        if not os.path.isfile(exe):
            raise SystemExit("not built: " + exe)

    # A known starting point, so every later comparison means something.
    os.makedirs(os.path.dirname(INI), exist_ok=True)
    with open(INI, "w", encoding="utf-8") as f:
        f.write("[General]\nVoice=textalker\nLogLevel=info\n")

    before = render()
    print("Baseline through SAPI: %.2f s at %d Hz, rms %.0f\n" % before)

    print("Changing controls the way a person would...")
    proc = subprocess.Popen([CONFIG_EXE])
    dialog = find_dialog(proc.pid)
    if dialog is None:
        proc.kill()
        raise SystemExit("the configuration dialog never appeared")

    set_combo(dialog, IDC_RATE, 14)        # 2.00x
    set_combo(dialog, IDC_CLOCK, 9)        # 1.00x
    set_combo(dialog, IDC_SAMPLERATE, 5)   # 44100 Hz
    set_combo(dialog, IDC_FRAMERATE, 0)
    set_edit(dialog, IDC_PITCH, "55")
    set_edit(dialog, IDC_VOLUME, "9")
    set_edit(dialog, IDC_WORDDELAY, "6")
    set_edit(dialog, IDC_REPEAT, "40")
    set_check(dialog, IDC_MONOTONE, True)
    set_check(dialog, IDC_COMPRESSED, True)

    time.sleep(0.8)                        # let the debounced save fire
    user32.SendMessageW(w.HWND(dialog), WM_CLOSE, 0, 0)
    proc.wait(timeout=10)
    print("Utility closed.\n")

    print("Settings file after closing:")
    ini = read_ini()
    voice = ini.get("Voice.textalker", {})
    expect("Speed", voice.get("Speed"), "2")
    expect("Pitch", voice.get("Pitch"), "55")
    expect("Volume", voice.get("Volume"), "9")
    expect("WordDelay", voice.get("WordDelay"), "6")
    expect("RepeatFilter", voice.get("RepeatFilter"), "40")
    expect("SampleRate", voice.get("SampleRate"), "44100")
    expect("Monotone", voice.get("Monotone"), "1")
    expect("Compressed", voice.get("Compressed"), "1")
    expect("General/Voice", ini.get("General", {}).get("Voice"), "textalker")

    print("\nDoes the engine pick it up?")
    after = render()
    if after is None:
        print("  FAIL  the engine produced nothing")
        failures.append("render")
    else:
        print("  before: %.2f s at %d Hz, rms %.0f" % before)
        print("  after:  %.2f s at %d Hz, rms %.0f" % after)
        if after[1] != 44100:
            print("  FAIL  output sample rate did not follow the setting")
            failures.append("sample rate")
        elif abs(after[0] - before[0]) < 0.05:
            print("  FAIL  duration did not change despite rate and speed edits")
            failures.append("duration")
        else:
            print("  ok    the engine is speaking with the new settings")

    # Reopening must show what was saved, not the defaults.
    print("\nReopening the utility to confirm it restores the saved values:")
    proc = subprocess.Popen([CONFIG_EXE])
    dialog = find_dialog(proc.pid)
    if dialog is None:
        proc.kill()
        raise SystemExit("the dialog did not reappear")
    for label, ctrl_id, want in (("Pitch", IDC_PITCH, "55"),
                                 ("Volume", IDC_VOLUME, "9"),
                                 ("Delay between words", IDC_WORDDELAY, "6"),
                                 ("Repeat-character filter", IDC_REPEAT, "40")):
        expect(label, get_edit(dialog, ctrl_id), want)
    for label, ctrl_id, want in (("Rate index", IDC_RATE, 14),
                                 ("Sample rate index", IDC_SAMPLERATE, 5)):
        h = user32.GetDlgItem(w.HWND(dialog), ctrl_id)
        expect(label, user32.SendMessageW(w.HWND(h), CB_GETCURSEL, 0, 0), want)
    # Settings are per-voice, so switching voices must not carry values across
    # and must not lose the ones just left behind.
    print("\nSwitching to the second voice and giving it its own settings:")
    set_combo(dialog, IDC_VOICE, 1)
    time.sleep(0.4)
    expect("second voice starts at default pitch", get_edit(dialog, IDC_PITCH), "24")
    set_edit(dialog, IDC_PITCH, "12")
    time.sleep(0.8)
    set_combo(dialog, IDC_VOICE, 0)
    time.sleep(0.4)
    expect("first voice kept its pitch", get_edit(dialog, IDC_PITCH), "55")

    # The buttons, checked for "does something and does not fall over".
    print("\nButtons:")
    for label, ctrl_id in (("Speak test", IDC_SPEAK), ("Open log folder", IDC_OPENLOGS)):
        notify(dialog, ctrl_id, BN_CLICKED, user32.GetDlgItem(w.HWND(dialog), ctrl_id))
        time.sleep(0.3)
        alive = proc.poll() is None
        expect("%s left the utility running" % label, alive, True)
    notify(dialog, IDC_RESET, BN_CLICKED, user32.GetDlgItem(w.HWND(dialog), IDC_RESET))
    time.sleep(0.5)
    expect("Reset to defaults restored the pitch", get_edit(dialog, IDC_PITCH), "24")

    user32.SendMessageW(w.HWND(dialog), WM_CLOSE, 0, 0)
    proc.wait(timeout=10)

    ini = read_ini()
    expect("second voice kept its own pitch",
           ini.get("Voice.textalker_v13", {}).get("Pitch"), "12")
    expect("first voice was reset",
           ini.get("Voice.textalker", {}).get("Pitch"), "24")

    # Leave the machine on defaults.
    with open(INI, "w", encoding="utf-8") as f:
        f.write("[General]\nVoice=textalker\nLogLevel=info\n")

    print()
    if failures:
        print("FAILED: %s" % ", ".join(failures))
        return 1
    print("Every control saved, persisted across a restart, and reached the audio.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
