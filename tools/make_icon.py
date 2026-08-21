"""Generate installer/echogppc.ico -- a speaker on a dark rounded square.

Written by hand rather than pulled from a library so the build has no image
dependency: the ICO container is a directory of BITMAPINFOHEADER images, and
at these sizes drawing it procedurally with supersampling is a few dozen
lines.

    python tools/make_icon.py
"""

import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "installer", "echogppc.ico")

SIZES = (16, 24, 32, 48, 64, 256)
SS = 4  # supersampling factor per axis

BG = (0x1B, 0x24, 0x30)      # dark slate, the body of the icon
FG = (0x4A, 0xDE, 0x80)      # phosphor green, the speaker
EDGE = (0x0E, 0x14, 0x1C)    # a slightly darker rim so it reads on any wallpaper


def rounded_rect(x, y, r):
    """Signed test: is (x, y) inside the unit rounded square?"""
    m = 0.045          # margin
    lo, hi = m, 1.0 - m
    cx = min(max(x, lo + r), hi - r)
    cy = min(max(y, lo + r), hi - r)
    if lo <= x <= hi and lo <= y <= hi:
        if (x < lo + r or x > hi - r) and (y < lo + r or y > hi - r):
            return math.hypot(x - cx, y - cy) <= r
        return True
    return False


def in_speaker(x, y):
    # Body of the speaker.
    if 0.20 <= x <= 0.37 and 0.39 <= y <= 0.61:
        return True
    # Cone: widens from the body out to the right.
    if 0.37 <= x <= 0.55:
        t = (x - 0.37) / (0.55 - 0.37)
        half = 0.11 + t * (0.29 - 0.11)
        if abs(y - 0.5) <= half:
            return True
    return False


def in_waves(x, y):
    """Two arcs to the right of the cone."""
    dx, dy = x - 0.55, y - 0.5
    dist = math.hypot(dx, dy)
    if dx <= 0.02:
        return False
    angle = abs(math.degrees(math.atan2(dy, dx)))
    if angle > 52:
        return False
    for radius in (0.20, 0.30):
        if abs(dist - radius) <= 0.028:
            return True
    return False


def render(size):
    """Returns BGRA bytes, top-down."""
    pixels = []
    step = 1.0 / (size * SS)
    radius = 0.18
    for py in range(size):
        row = []
        for px in range(size):
            acc_bg = acc_fg = acc_edge = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = (px * SS + sx + 0.5) * step
                    y = (py * SS + sy + 0.5) * step
                    if not rounded_rect(x, y, radius):
                        continue
                    # A thin darker rim just inside the outline.
                    if not rounded_rect(x, y, radius) or not rounded_rect(
                            0.5 + (x - 0.5) * 1.06, 0.5 + (y - 0.5) * 1.06, radius):
                        acc_edge += 1
                    elif in_speaker(x, y) or in_waves(x, y):
                        acc_fg += 1
                    else:
                        acc_bg += 1
            total = SS * SS
            covered = acc_bg + acc_fg + acc_edge
            if covered == 0:
                row.append((0, 0, 0, 0))
                continue
            r = (BG[0] * acc_bg + FG[0] * acc_fg + EDGE[0] * acc_edge) / covered
            g = (BG[1] * acc_bg + FG[1] * acc_fg + EDGE[1] * acc_edge) / covered
            b = (BG[2] * acc_bg + FG[2] * acc_fg + EDGE[2] * acc_edge) / covered
            alpha = int(round(255.0 * covered / total))
            row.append((int(b), int(g), int(r), alpha))
        pixels.append(row)
    return pixels


def encode_image(pixels, size):
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                         0, 0, 0, 0, 0)
    xor = bytearray()
    for py in range(size - 1, -1, -1):          # DIBs are stored bottom-up
        for px in range(size):
            b, g, r, a = pixels[py][px]
            xor += bytes((b, g, r, a))
    # The AND mask is unused for 32-bit icons but the format still demands it.
    stride = ((size + 31) // 32) * 4
    and_mask = bytes(stride * size)
    return header + bytes(xor) + and_mask


def main():
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    images = [(size, encode_image(render(size), size)) for size in SIZES]

    offset = 6 + 16 * len(images)
    directory = b""
    for size, data in images:
        directory += struct.pack("<BBBBHHII",
                                 size if size < 256 else 0,
                                 size if size < 256 else 0,
                                 0, 0, 1, 32, len(data), offset)
        offset += len(data)

    with open(OUT, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)))
        f.write(directory)
        for _, data in images:
            f.write(data)

    print("wrote %s (%d bytes, sizes %s)"
          % (OUT, os.path.getsize(OUT), ", ".join(str(s) for s in SIZES)))


if __name__ == "__main__":
    main()
