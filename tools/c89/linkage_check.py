#!/usr/bin/env python3
"""Data shared by the core's C and C++ files, with C++ linkage on one side.

A variable defined in a .c file has C linkage. A C++ file that declares it
itself - "extern u32 foo;" in the file, or in a header outside its
extern "C" block - gives it C++ linkage, and GCC and Clang name the two
alike: everything links, and works. MSVC does not, and the link fails.
So every such declaration is an error here, wherever it is built.

The other way round as well: a variable a C file uses and a C++ file
defines has C linkage only if that file has seen it declared in an
extern "C" block first (the C file's header, included). The C++ file is
run through the preprocessor to see that it has.

    python3 tools/c89/linkage_check.py [objects...]

The objects are those of the core's C files, built for this machine (the
default: every core/**/*.o that has a .c next to it); the C++ files are
preprocessed as make builds them here. Functions need no check: their C++
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

# the variables C files use that C++ files define
def nm_syms(o, flag):
    out = subprocess.run(['nm', '-g', flag, o], capture_output=True, text=True).stdout
    return out.splitlines()

used = set()
for o in objs:
    for line in nm_syms(o, '--undefined-only'):
        parts = line.split()
        if parts and parts[0] == 'U':
            used.add(parts[-1])
cxx_defs = {}
for d, _, files in os.walk('core'):
    if 'deps' in d.split(os.sep) or 'libretro-common' in d:
        continue
    for f in files:
        if not f.endswith('.o'):
            continue
        base = os.path.join(d, f[:-2])
        src = next((base + e for e in ('.cpp', '.mm') if os.path.exists(base + e)), None)
        if src is None:
            continue
        for line in nm_syms(base + '.o', '--defined-only'):
            parts = line.split()
            if len(parts) == 3 and parts[1] in 'BDRCGSV' and parts[2] in used:
                cxx_defs.setdefault(src, set()).add(parts[2])

def extern_c_text(src):
    """What of @src, preprocessed with the build's flags, is in extern "C" blocks"""
    obj = os.path.splitext(src)[0] + '.o'
    cmd = subprocess.run(['make', '-n', '-B', obj], capture_output=True, text=True).stdout
    line = next((l for l in cmd.splitlines() if src in l and ' -c' in l), None)
    if line is None:
        return None
    args = line.split()
    keep = [a for a in args if a.startswith(('-I', '-D', '-isystem', '-std', '-U'))]
    pre = subprocess.run([args[0], '-E', '-P'] + keep + [src], capture_output=True, text=True).stdout
    out = []
    for m in re.finditer(r'extern\s+"C"\s*\{', pre):
        i, depth = m.end(), 1
        while depth and i < len(pre):
            if pre[i] == '{':
                depth += 1
            elif pre[i] == '}':
                depth -= 1
            i += 1
        out.append(pre[m.end():i])
    return '\n'.join(out)

for src, names in sorted(cxx_defs.items()):
    text = extern_c_text(src)
    if text is None:
        bad.append('%s: cannot get its compile command' % src)
        continue
    for n in sorted(names):
        if not re.search(r'\b' + re.escape(n) + r'\b', text):
            bad.append('%s: defines %s, which C files use, without an extern "C" declaration of it before' % (src, n))

for b in bad:
    print(b)
if bad:
    print('linkage_check: %d variable(s) with C++ linkage on one side: declare them once, in extern "C", in a header both include' % len(bad))
    sys.exit(1)
print('linkage_check: %d variables of the C files and %d of C++ files the C files use, all with C linkage' % (len(data), sum(len(v) for v in cxx_defs.values())))
