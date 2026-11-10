#!/usr/bin/env python3
"""Check a picture of the test disc built with -DLIVE_ODD (live_prog.c): at
the top right, a render to a texture of a size no power of two - 96 by 40
pixels, after frames of 120 by 50 and 96 by 40 in turn - drawn as the 128
by 64 texture it is in, a texel a pixel: red where the render drew, black
everywhere past it, the strip the larger render drew too. The picture is as
vk_hdr.c writes one (a 16-byte header: "VKHR", a format, the width and the
height; then 8-bit R, G, B, A rows, top first). Used by
tools/vulkan/vk_vram.sh.

Usage: odd_check.py picture.raw
"""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
w, h = struct.unpack('<II', d[8:16])
px = d[16:]


def at(x, y):
    x, y = x * w // 640, y * h // 480
    o = (y * w + x) * 4
    return tuple(px[o:o + 3])


# (the texture's top left is at 470, 2)
want = [((480, 10), (255, 0, 0), "the render"),
        ((560, 38), (255, 0, 0), "the render's bottom right"),
        ((580, 20), (0, 0, 0), "past the render, where the larger one drew"),
        ((594, 20), (0, 0, 0), "past both"),
        ((480, 47), (0, 0, 0), "below the render, where the larger one drew"),
        ((480, 60), (0, 0, 0), "below both"),
        ((594, 60), (0, 0, 0), "the texture's bottom right")]
bad = 0
for (x, y), c, what in want:
    got = at(x, y)
    if any(abs(a - b) > 2 for a, b in zip(got, c)):
        print('  FAIL: %s is %s, not %s' % (what, got, c))
        bad += 1
sys.exit(1 if bad else 0)
