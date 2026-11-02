#!/usr/bin/env python3
"""Check a picture of the test disc built with -DLIVE_DMA (live_prog.c):
the three textures DMA writes into, where the disc draws them at the top
right of the screen - cyan over the yellow fetched from a render, cyan, and
green - and beside the green, the bottom half of a render made every frame,
drawn as a texture of another shape: blue, what it is rendered last, not
the red of its first frames, nor the green of its top half. The picture is as vk_hdr.c writes one (a 16-byte header: "VKHR", a
format, the width and the height; then 8-bit R, G, B, A rows, top first).
Used by tools/vulkan/vk_dma.sh, tools/gl/gl_dma.sh and tools/rend/d3d_dma.sh.

Usage: dma_check.py picture.raw
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


want = [((530, 20), (0, 255, 255), "the half: the DMA's cyan"),
        ((530, 60), (255, 255, 0), "the half: the render's yellow, fetched"),
        ((600, 40), (0, 255, 255), "the whole: the DMA's cyan"),
        ((530, 110), (0, 255, 0), "the texture: the DMA's green"),
        ((600, 110), (0, 0, 255), "a render's bottom half as another shape: the last one's blue")]
bad = 0
for (x, y), c, what in want:
    got = at(x, y)
    if any(abs(a - b) > 2 for a, b in zip(got, c)):
        print('  FAIL: %s is %s, not %s' % (what, got, c))
        bad += 1
sys.exit(1 if bad else 0)
