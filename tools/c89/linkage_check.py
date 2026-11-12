#!/usr/bin/env python3
"""Data the core's C files define, declared with C++ linkage.

A variable defined in a .c file has C linkage. A C++ file that declares it
itself - "extern u32 foo;" in the file, or in a header outside its
extern "C" block - gives it C++ linkage, and GCC and Clang name the two
alike: everything links, and works. MSVC does not, and the link fails.
So every such declaration is an error here, wherever it is built.

    python3 tools/c89/linkage_check.py [objects...]

The objects are those of the core's C files, built (the default: every
core/**/*.o that has a .c next to it). Functions need no check: their C++
names are mangled everywhere, and the link fails on every compiler.
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.chdir(ROOT)

objs = sys.argv[1:]
if not objs:
    for d, _, files in os.walk('core'):
        if 'deps' in d.split(os.sep) or 'libretro-common' in d:
            continue
        for f in files:
            if f.endswith('.o') and os.path.exists(os.path.join(d, f[:-2] + '.c')):
                objs.append(os.path.join(d, f))
if not objs:
    sys.exit('linkage_check: no objects of C files: build the core first')

data = set()
for o in objs:
    out = subprocess.run(['nm', '-g', '--defined-only', o], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'BDRCGSV':
            data.add(parts[2].lstrip('_') if sys.platform == 'darwin' else parts[2])

def c_regions(text):
    """For each line, whether it is inside an extern "C" block."""
    inside = []
    depth = 0
    pending = False
    for line in text.split('\n'):
        t = line.strip()
        if re.match(r'extern\s+"C"\s*\{', t) or t == 'RETRO_BEGIN_DECLS':
            depth += 1
        inside.append(depth > 0)
        if depth > 0 and ((t == '}' and pending) or t == 'RETRO_END_DECLS'):
            depth -= 1
        pending = t.startswith('#ifdef __cplusplus') or t.startswith('#if defined(__cplusplus)')
    return inside

decl = re.compile(r'^\s*extern\s+(?!"C")[^;(){}=]*?\b(\w+)\s*(\[[^\]]*\]\s*)*;')
bad = []
for d, _, files in os.walk('core'):
    if 'deps' in d.split(os.sep) or 'libretro-common' in d:
        continue
    for f in files:
        if not f.endswith(('.cpp', '.h', '.mm')):
            continue
        p = os.path.join(d, f)
        text = open(p, encoding='utf-8', errors='replace').read()
        inside = c_regions(text) if f.endswith('.h') else None
        for n, line in enumerate(text.split('\n')):
            m = decl.match(re.sub(r'DECL_ALIGN\(\w+\)', '', line))
            if not m or m.group(1) not in data:
                continue
            if inside is not None and inside[n]:
                continue
            bad.append('%s:%d: %s' % (p, n + 1, line.strip()))

for b in bad:
    print(b)
if bad:
    print('linkage_check: %d declaration(s) of C data with C++ linkage: take them from the C file\'s header' % len(bad))
    sys.exit(1)
print('linkage_check: %d C data symbols, none declared with C++ linkage' % len(data))
