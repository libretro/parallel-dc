#!/usr/bin/env python3
"""Nothing in the tree the core does not use.

  python3 tools/unused_sources.py [MAKE ARGS...]

Run from the top of the tree once the core is built (with the same make
arguments, e.g. platform=win): fails, naming them, on

  - an object of the build that defines something and that nothing the
    core exports (retro_*) or constructs at load reaches: code compiled
    and linked for nothing;
  - a libretro-common source that no build lists, and a libretro-common
    header nothing names: libretro-common is taken from RetroArch only as
    far as the core uses it.

nm and objdump are $NM and $OBJDUMP (default nm, objdump).
"""
import os
import re
import subprocess
import sys

LRC = 'core/libretro-common'
BUILD_FILES = ['Makefile', 'Makefile.common', 'jni/Android.mk']


def objects(make_args):
    out = subprocess.run(
        ['make', '-s', '-f', 'Makefile', '-f', '-'] + make_args + ['unused-sources-objects'],
        input='unused-sources-objects: ; @echo $(OBJECTS)\n',
        capture_output=True, text=True, check=True).stdout
    return [o for o in out.split() if o.endswith('.o')]


def unreachable(objs):
    nm = os.environ.get('NM', 'nm')
    objdump = os.environ.get('OBJDUMP', 'objdump')
    defs, undef, roots, defines = {}, {}, set(), {}
    for o in objs:
        if not os.path.exists(o):
            sys.exit('unused_sources: %s not built: build the core first' % o)
        out = subprocess.run([nm, '-P', '-g', o], capture_output=True,
                             text=True, check=True).stdout
        wants, has = set(), 0
        for line in out.splitlines():
            p = line.split()
            if len(p) < 2:
                continue
            name, kind = p[0], p[1]
            if kind == 'U':
                wants.add(name)
            elif kind not in 'wv':
                has += 1
                defs.setdefault(name, o)
                if name.lstrip('_').startswith('retro_'):
                    roots.add(o)
        undef[o], defines[o] = wants, has
        heads = subprocess.run([objdump, '-h', o], capture_output=True,
                               text=True).stdout
        if re.search(r'\.(init_array|ctors|CRT\$XC)', heads):
            roots.add(o)
    seen, todo = set(roots), list(roots)
    while todo:
        for name in undef[todo.pop()]:
            d = defs.get(name)
            if d and d not in seen:
                seen.add(d)
                todo.append(d)
    # (an object of another processor's code, empty here, is not one)
    return [o for o in objs if o not in seen and defines[o]]


def tracked(*specs):
    return subprocess.run(['git', 'ls-files'] + list(specs),
                          capture_output=True, text=True,
                          check=True).stdout.split()


def main():
    bad = []
    for o in unreachable(objects(sys.argv[1:])):
        bad.append('%s: built, and nothing the core runs reaches it' % o)

    builds = ''.join(open(f).read() for f in BUILD_FILES if os.path.exists(f))
    for src in tracked(LRC + '/*.c'):
        if src[len(LRC) + 1:] not in builds:
            bad.append('%s: in no build' % src)

    named = {}
    for f in tracked('core', 'tools', *BUILD_FILES):
        if f.endswith(('.c', '.cpp', '.cc', '.h', '.hpp', '.inc', '.mk',
                       '.sh', '.py', 'Makefile', 'Makefile.common')):
            try:
                named[f] = open(f, errors='replace').read()
            except OSError:
                pass
    for h in tracked(LRC + '/include/*.h'):
        base = re.compile(r'[<"/]' + re.escape(os.path.basename(h)) + r'[>"]')
        # (compat/msvc's stand in for the compiler's own, by name)
        if '/compat/msvc/' in h:
            continue
        if not any(base.search(text) for f, text in named.items() if f != h):
            bad.append('%s: named by nothing' % h)

    for line in bad:
        print('unused_sources: ' + line)
    if bad:
        return 1
    print('unused_sources: nothing unused')
    return 0


if __name__ == '__main__':
    sys.exit(main())
