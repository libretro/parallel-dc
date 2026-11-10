#!/usr/bin/env python3
"""Check a picture of the plain test disc (live_prog.c) where OpenGL reads
textures of one channel: the polygon in fog, whose table is a texture of
bytes - half white, half green - and the polygon with the 8-bit paletted
texture, whose indices are a texture of bytes - blue. The picture is as
vk_hdr.c and gl_frames.c write one (a 16-byte header: "VKHR", a format,
the width and the height; then 8-bit R, G, B, A rows, top first). Used by
tools/gl/gl_channels.sh.

Usage: channel_check.py picture.raw
"""
import struct
import sys

d = open(sys.argv[1], 'rb').read()
w, h = struct.unpack('<II', d[8:16])
px = d[16:]


def at(x, y):
    # (the program's screen is 320 by 240)
    x, y = x * w // 320, y * h // 240
    o = (y * w + x) * 4
    return tuple(px[o:o + 3])


want = [((64, 42), (128, 255, 128), "the polygon in fog"),
        ((36, 34), (128, 255, 128), "the polygon in fog, top left"),
        ((92, 51), (128, 255, 128), "the polygon in fog, bottom right"),
        ((240, 90), (0, 0, 255), "the paletted texture"),
        ((196, 74), (0, 0, 255), "the paletted texture, top left"),
        ((284, 106), (0, 0, 255), "the paletted texture, bottom right")]
bad = 0
for (x, y), c, what in want:
    got = at(x, y)
    if any(abs(a - b) > 3 for a, b in zip(got, c)):
        print('  FAIL: %s is %s, not %s' % (what, got, c))
        bad += 1
sys.exit(1 if bad else 0)
