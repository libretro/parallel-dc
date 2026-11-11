#!/usr/bin/env python3
# Dumped custom textures (see custom_image_test.sh) read by a PNG reader of
# Python's own, zlib alone: each FILE.png the RGBA bytes of FILE.rgba.
import struct
import sys
import zlib


def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat, ihdr = 8, b'', None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        crc, = struct.unpack('>I', data[pos + 8 + n:pos + 12 + n])
        if zlib.crc32(kind + body) & 0xffffffff != crc:
            raise ValueError('bad CRC in ' + kind.decode())
        if kind == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
        elif kind == b'IEND':
            break
        pos += 12 + n
    w, h, depth, ctype, _, _, interlace = ihdr
    if (depth, ctype, interlace) != (8, 6, 0):
        raise ValueError('not 8-bit RGBA, not interlaced')
    raw, stride, out, prev = zlib.decompress(idat), w * 4, bytearray(), bytearray(w * 4)
    for y in range(h):
        f = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = row[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if f == 1:
                row[i] = (row[i] + a) & 255
            elif f == 2:
                row[i] = (row[i] + b) & 255
            elif f == 3:
                row[i] = (row[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                p = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                row[i] = (row[i] + p) & 255
            elif f != 0:
                raise ValueError('filter %d' % f)
        out += row
        prev = row
    return w, h, bytes(out)


bad = 0
for path in sys.argv[1:]:
    w, h, px = read_png(path)
    want = open(path[:-4] + '.rgba', 'rb').read()
    if px != want:
        print('%s: %dx%d, not the texels dumped' % (path, w, h))
        bad += 1
    else:
        print('%s: %dx%d, as dumped (Python)' % (path.rsplit('/', 1)[-1], w, h))
sys.exit(1 if bad else 0)
