"""Render Echo GPPC / Textalker demo WAVs straight from echotalk*.dll.

Proof-of-life for the SAPI5 port: this script touches no registry key, no
SAPI4 interface and no COM at all.  It loads the emulator DLL, boots a
Textalker ROM pair, and writes PCM to disk.  If a voice renders here it
will render inside the SAPI5 engine, because the engine drives exactly
these entry points.

    python tools/render_samples.py [outdir]
"""

import ctypes
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(os.path.dirname(HERE), "bin")

# Library-unit ranges, straight out of the Textalker command set.
PITCH_MAX, VOLUME_MAX, DELAY_MAX, REPEAT_MAX = 63, 15, 15, 99
DEF_PITCH, DEF_VOLUME, DEF_DELAY, DEF_REPEAT = 24, 12, 0, 99
REQUIRED_ABI = 6

# Everything is rendered at one rate so the tour file can concatenate
# segments without resampling.  22050 sits above what the chip produces at
# any clock this script uses (8000 * 2.0 = 16000), so nothing is ever
# downsampled -- see _effectiveRate.
TOUR_RATE = 22050


def _dll_name():
    return "echotalk64.dll" if ctypes.sizeof(ctypes.c_void_p) == 8 else "echotalk32.dll"


class Echo:
    """One booted Textalker machine.  Only one may exist at a time."""

    lib = None

    @classmethod
    def load(cls):
        if cls.lib is not None:
            return cls.lib
        lib = ctypes.cdll.LoadLibrary(os.path.join(BIN, _dll_name()))
        p = ctypes.c_void_p
        lib.echotalk_abi_version.restype, lib.echotalk_abi_version.argtypes = ctypes.c_uint, []
        lib.echotalk_create.restype = p
        lib.echotalk_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                        ctypes.c_char_p, ctypes.c_size_t]
        lib.echotalk_destroy.restype, lib.echotalk_destroy.argtypes = None, [p]
        lib.echotalk_version.restype, lib.echotalk_version.argtypes = ctypes.c_char_p, [p]
        lib.echotalk_speak.restype, lib.echotalk_speak.argtypes = ctypes.c_int, [p, ctypes.c_char_p]
        lib.echotalk_read.restype = ctypes.c_size_t
        lib.echotalk_read.argtypes = [p, ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t]
        lib.echotalk_sample_rate.restype, lib.echotalk_sample_rate.argtypes = ctypes.c_uint, [p]
        lib.echotalk_overruns.restype, lib.echotalk_overruns.argtypes = ctypes.c_uint, [p]
        lib.echotalk_command_errors.restype = ctypes.c_uint
        lib.echotalk_command_errors.argtypes = [p]
        for name, arg in (("set_pitch", ctypes.c_int), ("set_flat", ctypes.c_int),
                          ("set_volume", ctypes.c_int), ("set_word_delay", ctypes.c_int),
                          ("set_repeat_filter", ctypes.c_int),
                          ("set_compressed", ctypes.c_int),
                          ("set_frame_rate", ctypes.c_int),
                          ("set_speed", ctypes.c_double),
                          ("set_clock_multiplier", ctypes.c_double),
                          ("set_sample_rate", ctypes.c_uint)):
            fn = getattr(lib, "echotalk_" + name)
            fn.restype, fn.argtypes = ctypes.c_int, [p, arg]
        abi = lib.echotalk_abi_version()
        if abi != REQUIRED_ABI:
            raise SystemExit("echotalk DLL reports ABI %d, expected %d" % (abi, REQUIRED_ABI))
        cls.lib = lib
        return lib

    def __init__(self, loader, obj):
        lib = self.load()
        err = ctypes.create_string_buffer(256)
        self.h = lib.echotalk_create(loader.encode("mbcs"), obj.encode("mbcs"), err, len(err))
        if not self.h:
            raise RuntimeError("echotalk_create failed: " + err.value.decode("mbcs", "replace"))

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        if self.h:
            self.lib.echotalk_destroy(self.h)
            self.h = None

    @property
    def banner(self):
        return self.lib.echotalk_version(self.h).decode("ascii", "replace")

    @property
    def rate(self):
        return self.lib.echotalk_sample_rate(self.h)

    def configure(self, pitch=DEF_PITCH, volume=DEF_VOLUME, delay=DEF_DELAY,
                  repeat=DEF_REPEAT, monotone=False, compressed=False,
                  speed=1.0, clock=1.0, samplerate=TOUR_RATE):
        lib, h = self.lib, self.h
        lib.echotalk_set_pitch(h, pitch)
        lib.echotalk_set_flat(h, 1 if monotone else 0)
        lib.echotalk_set_volume(h, volume)
        lib.echotalk_set_word_delay(h, delay)
        lib.echotalk_set_repeat_filter(h, repeat)
        lib.echotalk_set_compressed(h, 1 if compressed else 0)
        lib.echotalk_set_speed(h, speed)
        lib.echotalk_set_clock_multiplier(h, clock)
        # The chip really produces 8000 * clock; asking for less than that
        # would downsample through a filterless resampler and turn real
        # detail into aliasing.  So the request is a floor, exactly as the
        # SAPI5 engine treats it.
        lib.echotalk_set_sample_rate(h, max(samplerate, int(8000 * clock + 0.5)))

    def render(self, text):
        """Synthesises `text` and returns raw 16-bit mono PCM bytes."""
        buf = (ctypes.c_int16 * 4096)()
        self.lib.echotalk_speak(self.h, text.encode("utf-8"))
        out = bytearray()
        while True:
            n = self.lib.echotalk_read(self.h, buf, 4096)
            if not n:
                break
            out += ctypes.string_at(buf, n * 2)
        return bytes(out)


def write_wav(path, pcm, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    return len(pcm) // 2 / float(rate)


def silence(rate, seconds=0.45):
    return b"\0\0" * int(rate * seconds)


def image_pairs():
    found = []
    for name in sorted(os.listdir(BIN)):
        if name.endswith(".ram.bin"):
            stem = name[:-len(".ram.bin")]
            obj = os.path.join(BIN, stem + ".obj.bin")
            if os.path.isfile(obj):
                found.append((stem, os.path.join(BIN, name), obj))
    return found


SENTENCE = ("Hello. This is the Echo, from Street Electronics. "
            "The quick brown fox jumps over the lazy dog. "
            "Zero one two three four five six seven eight nine.")

# (file suffix, spoken label for the tour, configure kwargs)
DEMOS = [
    ("default",      "Default settings.",                 {}),
    ("pitch-low",    "Pitch, lowest.",                    dict(pitch=4)),
    ("pitch-high",   "Pitch, highest.",                   dict(pitch=60)),
    ("rate-slow",    "Rate, one half speed.",             dict(speed=0.5)),
    ("rate-fast",    "Rate, double speed.",               dict(speed=2.0)),
    ("volume-low",   "Volume, low and fuzzy.",            dict(volume=3)),
    ("volume-max",   "Volume, maximum.",                  dict(volume=15)),
    ("monotone",     "Monotone.",                         dict(monotone=True)),
    ("compressed",   "Compressed speech.",                dict(compressed=True)),
    ("clock-slow",   "Chip clock, slowed down.",          dict(clock=0.7)),
    ("clock-fast",   "Chip clock, sped up.",              dict(clock=1.6)),
    ("word-delay",   "Delay between words. Textalker three only.",
                     dict(delay=8)),
    ("repeat-filter", "Repeat character filter set to three. "
                      "aaaaaaaa bbbbbbbb.", dict(repeat=3)),
]


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(HERE), "samples")
    os.makedirs(outdir, exist_ok=True)

    pairs = image_pairs()
    if not pairs:
        raise SystemExit("no Textalker image pairs found in " + BIN)

    print("echotalk DLL : %s (ABI %d)" % (_dll_name(), Echo.load().echotalk_abi_version()))
    print("output       : %s\n" % outdir)

    manifest = []
    for stem, loader, obj in pairs:
        # Read the banner from a throwaway machine so the voice is labelled
        # with what the ROM says it is, not what the file is called.
        with Echo(loader, obj) as e:
            banner = e.banner
        print("=== %s -- Textalker %s (English) ===" % (stem, banner))

        tour = bytearray()
        for suffix, label, kwargs in DEMOS:
            with Echo(loader, obj) as e:
                e.configure(**kwargs)
                rate = e.rate
                pcm = e.render(SENTENCE)
                errs = e.lib.echotalk_command_errors(e.h)
                over = e.lib.echotalk_overruns(e.h)
            name = "%s_%s.wav" % (stem, suffix)
            secs = write_wav(os.path.join(outdir, name), pcm, rate)
            flag = ""
            if errs or over:
                flag = "   [command_errors=%d overruns=%d]" % (errs, over)
            print("  %-34s %6.2f s @ %d Hz%s" % (name, secs, rate, flag))
            manifest.append((name, secs, rate))

            # Spoken label, then the demo, then a gap.
            with Echo(loader, obj) as e:
                e.configure()
                tour += e.render(label) + silence(TOUR_RATE, 0.25)
            tour += pcm + silence(TOUR_RATE, 0.5)

        with Echo(loader, obj) as e:
            e.configure()
            intro = e.render("Textalker version %s. English." % banner) + silence(TOUR_RATE, 0.4)
        name = "%s_TOUR.wav" % stem
        secs = write_wav(os.path.join(outdir, name), bytes(intro) + bytes(tour), TOUR_RATE)
        print("  %-34s %6.2f s @ %d Hz  <-- everything above, back to back"
              % (name, secs, TOUR_RATE))
        manifest.append((name, secs, TOUR_RATE))
        print()

    print("%d files, %.1f s of audio total" % (len(manifest), sum(m[1] for m in manifest)))


if __name__ == "__main__":
    main()
