"""Judges two pictures of the test disc built with -DLIVE_KEEP
(tools/threads/live_disc.py --keep), in vk_hdr's form (a 16-byte header:
"VKHR", the Vulkan format, the width, the height; then RGBA8 rows, top
first; 640x480, the disc's 320x240 doubled): one from a plain run, one from
a run of one frame less with every frame run ahead (gl_frames GLF_AHEAD).
The disc renders a picture to a texture once, at the very end of a frame,
and then only draws with it, in the screen's top left: yellow, its top left
quarter cyan, at x and y 10-137. A state saved right after that frame is
saved while the picture is still on its way; the run ahead loads such a
state every frame, so it shows the picture only if the state has it. The
two pictures have to be the same, and show it.

   python3 ahead_check.py plain.raw ahead.raw"""
import struct
import sys

def load(path):
    try:
        data = open(path, 'rb').read()
    except OSError as e:
        print('  FAIL: no picture: %s' % e)
        return None
    if len(data) < 16:
        print('  FAIL: %s is not a picture' % path)
        return None
    magic, fmt, w, h = struct.unpack('<4I', data[:16])
    if magic != 0x52484b56 or fmt != 37 or (w, h) != (640, 480) or len(data) < 16 + w * h * 4:
        print('  FAIL: %s is not a 640x480 RGBA8 picture: %x %d %dx%d' % (path, magic, fmt, w, h))
        return None
    return data

def px(data, x, y):
    o = 16 + (y * 640 + x) * 4
    return tuple(data[o:o + 3])

def main():
    if len(sys.argv) != 3:
        sys.stderr.write(__doc__ + '\n')
        return 2
    plain, ahead = load(sys.argv[1]), load(sys.argv[2])
    if plain is None or ahead is None:
        return 1
    fails = 0
    for name, data in (('plain', plain), ('ahead', ahead)):
        for (x, y), want, what in (((40, 40), (0, 255, 255), 'cyan'), ((110, 40), (255, 255, 0), 'yellow'),
                                   ((110, 110), (255, 255, 0), 'yellow'), ((40, 110), (255, 255, 0), 'yellow')):
            got = px(data, x, y)
            if got != want:
                print('  FAIL: %s: the kept picture at %d,%d is %s, not %s' % (name, x, y, got, what))
                fails += 1
    if plain[16:] != ahead[16:]:
        n = sum(1 for i in range(16, len(plain), 4) if plain[i:i + 3] != ahead[i:i + 3])
        print('  FAIL: the run ahead is not the plain run: %d pixels differ' % n)
        fails += 1
    if fails == 0:
        print('  ok: the picture rendered to a texture is in the state, run ahead as run plain')
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
