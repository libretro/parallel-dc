#!/usr/bin/env python3
"""Every line the core logs ends with its own newline (core/log/Log.h).

The log macros (ERROR_LOG ... DEBUG_LOG) hand their format straight to the
frontend's log_cb, which adds none. This finds every call of them in core/,
and of a file's own macros made of them (#define debugf(...) DEBUG_LOG(...)),
and fails on a format that is not a string literal ending in "\\n".

   python3 tools/log/log_check.py
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
LOG = r'\b(?:ERROR|WARN|NOTICE|INFO|DEBUG)_LOG\(\s*[A-Za-z_0-9]+\s*,'
WRAP = re.compile(r'#\s*define\s+([A-Za-z_][A-Za-z_0-9]*)\s*\(([^)]*)\)\s*' + LOG + r'\s*(?:__VA_ARGS__|fmt)')
LIT = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def check(path, text, bad):
    names = [m.group(1) for m in WRAP.finditer(text)]
    call = re.compile('(?:' + LOG + (r'|\b(?:' + '|'.join(names) + r')\(' if names else '') + r')\s*')
    for m in call.finditer(text):
        line_start = text.rfind('\n', 0, m.start()) + 1
        if re.match(r'\s*#\s*define\b', text[line_start:m.start()]):
            continue
        j, last = m.end(), None
        while True:
            lm = LIT.match(text, j)
            if not lm:
                break
            last, j = lm, lm.end()
            k = j
            while k < len(text) and text[k] in ' \t\r\n':
                k += 1
            if text.startswith('"', k):
                j = k
                continue
            break
        if last is None or not last.group(0).endswith('\\n"'):
            bad.append('%s:%d' % (os.path.relpath(path, ROOT), text.count('\n', 0, m.start()) + 1))


def main():
    bad, n = [], 0
    for root, dirs, files in os.walk(os.path.join(ROOT, 'core')):
        dirs[:] = [d for d in dirs if d not in ('deps', 'libretro-common')]
        for f in files:
            if f.endswith(('.c', '.cpp', '.h', '.mm')):
                p = os.path.join(root, f)
                with open(p, encoding='utf-8', errors='surrogateescape') as fh:
                    t = fh.read()
                n += 1
                check(p, t, bad)
    if bad:
        print('log_check: a format with no "\\n" at its end, or not a string literal:')
        for b in bad:
            print('  ' + b)
        return 1
    print('log_check: %d files, every format ends with "\\n"' % n)
    return 0


if __name__ == '__main__':
    sys.exit(main())
