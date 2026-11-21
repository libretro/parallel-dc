#!/usr/bin/env python3
"""How many colours a picture the test frontends write has (tools/rend/d3d9_frames.c,
tools/hdr/d3d_hdr.c: a 16-byte header - 'VKHR', its format, width, height -
then the pixels, four bytes each): a picture of a few at most is one that
drew nothing.

    python3 tools/rend/raw_colours.py PICTURE.raw [LEAST]

Prints the count; with LEAST, exits 1 when there are fewer than that.
"""
import struct
import sys

data = open(sys.argv[1], 'rb').read()
if len(data) < 16 or data[:4] != b'VKHR':
    print('raw_colours: %s is not a picture' % sys.argv[1])
    sys.exit(1)
width, height = struct.unpack('<II', data[8:16])
pixels = data[16:16 + width * height * 4]
count = len(set(pixels[i:i + 4] for i in range(0, len(pixels), 4)))
print(count)
sys.exit(1 if len(sys.argv) > 2 and count < int(sys.argv[2]) else 0)
