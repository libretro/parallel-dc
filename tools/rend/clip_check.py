"""Judges a picture of the test disc built with -DLIVE_CLIP
(tools/threads/live_disc.py --clip), in vk_hdr's form (a 16-byte header:
"VKHR", the Vulkan format, the width, the height; then RGBA8 rows, top
first; 640x480, the disc's 320x240 doubled). Three rectangles of one kind
are drawn outside a clipping rectangle each, which leaves a hole in each:
cyan (over a yellow one) with its hole at x 64-127, y 256-319, and green
with its hole at x 192-239. The holes have to show the white background
and the rings their colours. A renderer that does not clip "inside" (draw
outside the rectangle) fills the holes; one that keeps the green one's
rectangle for the cyan one fills the cyan one's.

   python3 clip_check.py picture.raw"""
import struct
import sys

def main():
    try:
        data = open(sys.argv[1], 'rb').read()
    except OSError as e:
        print('  FAIL: no picture: %s' % e)
        return 1
    if len(data) < 16:
        print('  FAIL: not a picture')
        return 1
    magic, fmt, w, h = struct.unpack('<4I', data[:16])
    if magic != 0x52484b56 or fmt != 37 or (w, h) != (640, 480) or len(data) < 16 + w * h * 4:
        print('  FAIL: not a 640x480 RGBA8 picture: %x %d %dx%d' % (magic, fmt, w, h))
        return 1
    def px(x, y):
        o = 16 + (y * w + x) * 4
        return data[o:o + 3]
    checks = [
        ('the cyan one\'s hole', (96, 288), lambda c: min(c) >= 240),
        ('the cyan one\'s ring', (52, 244), lambda c: c[0] < 16 and c[1] > 240 and c[2] > 240),
        ('the cyan one\'s ring, below its hole', (96, 330), lambda c: c[0] < 16 and c[1] > 240 and c[2] > 240),
        ('the green one\'s hole', (215, 288), lambda c: min(c) >= 240),
        ('the green one\'s ring', (180, 244), lambda c: c[0] < 16 and c[1] > 240 and c[2] < 16),
    ]
    bad = 0
    for what, (x, y), ok in checks:
        c = px(x, y)
        if not ok(c):
            print('  FAIL: %s at %d,%d is %d,%d,%d' % (what, x, y, c[0], c[1], c[2]))
            bad += 1
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
