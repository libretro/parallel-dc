#!/usr/bin/env python3
"""The disc images tools/imgread/run.sh reads (see there).

  make_images.py OUT

Every sector's bytes say which file and which sector of it they are, so a
track read from the wrong place reads differently. Writes into OUT/ one
directory per image - sheets well made and not, DiscJuggler images of
versions 2, 3 and 3.5, and CHDs chdman makes of some of the sheets - and
prints the images' paths, one a line, in the order expected.txt has them.
"""
import os, struct, subprocess, sys, zlib

OUT = sys.argv[1]
paths = []


def sector(tag, i, size):
    seed = zlib.crc32(('%s:%d' % (tag, i)).encode())
    b = bytearray(size)
    for k in range(0, size, 4):
        seed = (seed * 1103515245 + 12345) & 0xffffffff
        b[k:k + 4] = struct.pack('<I', seed)[:min(4, size - k)]
    # a mode byte where a raw sector has one: mode 1 or 2 alternately
    if size in (2352, 2448):
        b[0:12] = b'\x00' + b'\xff' * 10 + b'\x00'
        b[15] = 1 + (i & 1)
    return bytes(b)


def data_file(path, tag, count, size):
    with open(path, 'wb') as f:
        for i in range(count):
            f.write(sector(tag, i, size))


def image(name, files, sheet_name, sheet):
    d = os.path.join(OUT, name)
    os.makedirs(d, exist_ok=True)
    for fname, (count, size) in files.items():
        data_file(os.path.join(d, fname), name + '/' + fname, count, size)
    p = os.path.join(d, sheet_name)
    with open(p, 'wb') as f:
        f.write(sheet.encode('latin-1'))
    paths.append(p)
    return d


# ---- CUE --------------------------------------------------------------
image('cue_basic', {'a.bin': (300, 2352)}, 'a.cue',
      'FILE "a.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'
      '  TRACK 02 AUDIO\n    INDEX 00 00:02:00\n    INDEX 01 00:02:10\n')
image('cue_two_files', {'d t.bin': (200, 2352), 'mus.bin': (120, 2352)}, 'x.cue',
      'FILE "d t.bin" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n'
      'FILE mus.bin BINARY\r\n  TRACK 02 AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:00:30\r\n')
image('cue_2336_2048', {'m2.bin': (100, 2336), 'm1.bin': (90, 2048)}, 'y.cue',
      'REM COMMENT "made up"\nFILE "m2.bin" BINARY\n TRACK 01 MODE2/2336\n INDEX 01 00:00:00\n'
      'FILE "m1.bin" BINARY\n TRACK 02 MODE1/2048\n INDEX 01 00:00:00')
image('cue_gd', {'t1.bin': (300, 2352), 't2.bin': (200, 2352), 't3.bin': (400, 2352)}, 'gd.cue',
      'REM SINGLE-DENSITY AREA\nFILE "t1.bin" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n'
      'FILE "t2.bin" BINARY\n TRACK 02 AUDIO\n INDEX 00 00:00:00\n INDEX 01 00:02:00\n'
      'REM HIGH-DENSITY AREA\nFILE "t3.bin" BINARY\n TRACK 03 MODE1/2352\n INDEX 01 00:00:00\n')
image('cue_sessions', {'s1.bin': (150, 2352), 's2.bin': (250, 2352)}, 's.cue',
      'REM SESSION 01\nFILE "s1.bin" BINARY\n TRACK 01 AUDIO\n INDEX 01 00:00:00\n'
      'REM SESSION 02\nFILE "s2.bin" BINARY\n TRACK 02 MODE2/2352\n INDEX 00 00:00:00\n INDEX 01 00:01:00\n')
image('cue_sessions_long', {'s1.bin': (150, 2352), 's2.bin': (450, 2352)}, 's.cue',
      'REM SESSION 01\nFILE "s1.bin" BINARY\n TRACK 01 AUDIO\n INDEX 01 00:00:00\n'
      'REM SESSION 02\nFILE "s2.bin" BINARY\n TRACK 02 MODE2/2352\n INDEX 00 00:00:00\n INDEX 01 00:03:00\n')
image('cue_no_file', {'a.bin': (50, 2352)}, 'b.cue', 'TRACK 01 AUDIO\nINDEX 01 00:00:00\n')
image('cue_bad_type', {'a.bin': (50, 2352)}, 'b.cue', 'FILE a.bin BINARY\nTRACK 01 MODE9/1\nINDEX 01 00:00:00\n')
image('cue_index_past', {'a.bin': (50, 2352)}, 'b.cue', 'FILE a.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 09:00:00\n')
image('cue_index_word', {'a.bin': (50, 2352)}, 'b.cue', 'FILE a.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 soon\n')
image('cue_track_word', {'a.bin': (50, 2352)}, 'b.cue', 'FILE a.bin BINARY\nTRACK one AUDIO\nINDEX 01 00:00:00\n')
image('cue_index_word2', {'a.bin': (50, 2352)}, 'b.cue',
      'FILE a.bin BINARY\nTRACK 01 AUDIO\nINDEX x1 00:00:00\nINDEX 01 00:00:05\n')
image('cue_glued', {'a.bin': (50, 2352)}, 'b.cue', 'FILE a.bin BINARY\nTRACK 01AUDIO\nINDEX 01 00:00:00\n')
image('cue_open_quote', {'a.bin': (50, 2352)}, 'b.cue', 'FILE "a.bin BINARY\nTRACK 01 AUDIO\n')
image('cue_empty', {}, 'b.cue', '')
image('cue_spaces', {'a.bin': (60, 2352)}, 'b.cue', '\n\n  FILE   a.bin   BINARY  \n TRACK 01  AUDIO \n INDEX 01 00:00:00   \n\n\n')
image('cue_missing_bin', {}, 'b.cue', 'FILE nothere.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n')
image('cue_odd_size', {'a.bin': (70, 2048)}, 'b.cue', 'FILE a.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n')
image('cue_three_in_one', {'a.bin': (400, 2352)}, 'b.cue',
      'FILE a.bin BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 00 00:01:00\nINDEX 01 00:01:10\n'
      'TRACK 03 AUDIO\nINDEX 01 00:03:00\n')
image('cue_neg', {'a.bin': (50, 2352)}, 'b.cue', 'REM SESSION -1\nFILE a.bin BINARY\nTRACK -1 AUDIO\nINDEX 01 00:00:00\n')
image('cue_big', {'a.bin': (50, 2352)}, 'b.cue', 'REM SESSION 99999999999\nFILE a.bin BINARY\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n')
image('cue_rem_junk', {'a.bin': (50, 2352)}, 'b.cue', 'FILE a.bin BINARY\nREM GENRE Puzzle\nTRACK 01 AUDIO\nINDEX 01 00:00:00\n')
image('cue_plus', {'a.bin': (50, 2352)}, 'b.cue', 'FILE a.bin BINARY\nTRACK +1 AUDIO\nINDEX +01 00:00:00\n')

# ---- GDI --------------------------------------------------------------
image('gdi_basic', {'track01.bin': (300, 2352), 'track02.raw': (200, 2352), 'track03.bin': (500, 2352)}, 'd.gdi',
      '3\n1 0 4 2352 track01.bin 0\n2 450 0 2352 track02.raw 0\n3 45000 4 2352 track03.bin 0\n')
image('gdi_quoted', {'t 1.bin': (300, 2048), 't2.raw': (100, 2352), 't 3.iso': (400, 2048)}, 'd.gdi',
      '3\r\n1 0 4 2048 "t 1.bin" 0\r\n2 450 0 2352 "t2.raw" 0\r\n3 45000 4 2048 "t 3.iso" 0')
image('gdi_sizes', {'a.bin': (100, 2336), 'b.bin': (100, 2448), 'c.bin': (100, 2352)}, 'd.gdi',
      '3\n1 0 4 2336 a.bin 0\n2 300 4 2448 b.bin 0\n3 45000 4 2352 c.bin 0\n')
image('gdi_offset', {'a.bin': (110, 2352), 'b.bin': (100, 2352), 'c.bin': (100, 2352)}, 'd.gdi',
      '3\n1 0 4 2352 a.bin 23520\n2 300 0 2352 b.bin 0\n3 45000 4 2352 c.bin 0\n')
image('gdi_nofile', {'a.bin': (100, 2352), 'c.bin': (100, 2352)}, 'd.gdi',
      '3\n1 0 4 2352 a.bin 0\n2 300 0 0 none 0\n3 45000 4 2352 c.bin 0\n')
image('gdi_short', {'a.bin': (100, 2352)}, 'd.gdi', '3\n1 0 4 2352 a.bin 0\n')
image('gdi_zero', {}, 'd.gdi', '0\n')
image('gdi_hundred', {}, 'd.gdi', '100\n')
image('gdi_missing', {}, 'd.gdi', '1\n1 0 4 2352 gone.bin 0\n')
image('gdi_badsize', {'a.bin': (10, 2352)}, 'd.gdi', '1\n1 0 4 1234 a.bin 0\n')
image('gdi_oddlen', {'a.bin': (10, 2352)}, 'd.gdi', '1\n1 0 4 2048 a.bin 0\n')
image('gdi_nooffset', {'a.bin': (10, 2352)}, 'd.gdi', '1\n1 0 4 2352 a.bin')
image('gdi_words', {'a.bin': (10, 2352)}, 'd.gdi', '1\none 0 4 2352 a.bin 0\n')
image('gdi_openquote', {'a.bin': (10, 2352)}, 'd.gdi', '1\n1 0 4 2352 "a.bin 0\n')
image('gdi_neg', {'a.bin': (12, 2352)}, 'd.gdi', '1\n1 0 4 2352 a.bin -2352\n')
image('gdi_two', {'a.bin': (100, 2352), 'b.bin': (100, 2352)}, 'd.gdi', '2\n1 0 4 2352 a.bin 0\n2 300 0 2352 b.bin 0\n')
image('gdi_x', {'x': (20, 2352), 'b.bin': (100, 2352)}, 'd.gdi', '2\n1 0 4 2352 x 0\n2 300 0 2352 b.bin 0\n')

# ---- CDI --------------------------------------------------------------
V2, V3, V35 = 0x80000004, 0x80000005, 0x80000006
SIZES = {2048: 0, 2336: 1, 2352: 2, 2448: 4}


def cdi(name, version, sessions, truncate=0, junk_mark=False):
    """sessions: [[(mode, size, pregap, length, extra_total), ...], ...]"""
    d = os.path.join(OUT, name)
    os.makedirs(d, exist_ok=True)
    data = bytearray()
    table = bytearray(struct.pack('<H', len(sessions)))
    lba = 0
    n = 0
    for s in sessions:
        table += struct.pack('<H', len(s))
        for (mode, size, pregap, length, extra) in s:
            total = pregap + length + extra
            for i in range(total):
                data += sector('%s/%d' % (name, n), i, size)
            mark = bytes([0, 0, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF])
            if junk_mark and n == 1:
                mark = b'\x09' * 10
            fname = b'image.cdi'
            e = struct.pack('<I', 0) + mark + mark + b'\0' * 4
            e += bytes([len(fname)]) + fname + b'\0' * 11 + b'\0' * 4 + b'\0' * 4
            e += struct.pack('<I', 0) + b'\0' * 2
            e += struct.pack('<ii', pregap, length) + b'\0' * 6
            e += struct.pack('<I', mode) + b'\0' * 12
            e += struct.pack('<Ii', lba, total) + b'\0' * 16
            e += struct.pack('<I', SIZES[size]) + b'\0' * 29
            if version != V2:
                e += b'\0' * 5 + struct.pack('<I', 0)
            table += e
            lba += total
            n += 1
        table += b'\0' * 12 + (b'\0' if version != V2 else b'')
        lba += 11400 if len(sessions) > 1 else 0
    header_at = len(data)
    blob = data + table
    off = (len(blob) + 8 - header_at) if version == V35 else header_at
    blob += struct.pack('<II', version, off)
    if truncate:
        blob = blob[:header_at] + blob[header_at:len(blob) - 8 - truncate] + blob[-8:]
    p = os.path.join(d, 'image.cdi')
    with open(p, 'wb') as f:
        f.write(blob)
    paths.append(p)


cdi('cdi_v2', V2, [[(1, 2352, 150, 200, 2)]])
cdi('cdi_v3_two', V3, [[(0, 2352, 150, 100, 2)], [(2, 2336, 150, 300, 0)]])
cdi('cdi_v35_mixed', V35, [[(0, 2352, 150, 80, 0), (0, 2352, 150, 60, 0)], [(2, 2048, 150, 200, 0), (1, 2448, 0, 30, 0)]])
cdi('cdi_truncated', V3, [[(1, 2352, 150, 100, 0)]], truncate=30)
cdi('cdi_badmark', V3, [[(0, 2352, 150, 50, 0), (1, 2352, 150, 50, 0)]], junk_mark=True)
cdi('cdi_short_track', V3, [[(1, 2352, 150, 100, -60)]])
p = os.path.join(OUT, 'cdi_garbage')
os.makedirs(p, exist_ok=True)
with open(os.path.join(p, 'image.cdi'), 'wb') as f:
    f.write(b'\x01' * 100)
paths.append(os.path.join(p, 'image.cdi'))

# ---- CHD (from the sheets above) ---------------------------------------
for src, c in (('cue_basic/a.cue', 'cdzs'), ('cue_two_files/x.cue', 'cdlz'), ('gdi_basic/d.gdi', 'cdzs'),
               ('cue_sessions/s.cue', 'cdzl'), ('cue_2336_2048/y.cue', 'cdzs'), ('gdi_sizes/d.gdi', 'cdlz')):
    o = os.path.join(OUT, 'chd_' + src.split('/')[0] + '.chd')
    r = subprocess.run(['chdman', 'createcd', '-f', '-c', c, '-i', os.path.join(OUT, src), '-o', o],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode == 0:
        paths.append(o)
    else:
        sys.stderr.write('chdman refused %s\n' % src)

print('\n'.join(paths))
