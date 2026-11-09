#!/usr/bin/env python3
"""Names the core's allocation sites vk_hdr printed (VKH_ALLOC_SITES) and
checks them against an allowlist (tools/vulkan/vk_allocs.sh).

  vk_alloc_sites.py core.so allowed.txt log...

A site is a chain of the core's frames, innermost first. Its name is the
first function in it, inlined ones included, that is not a library's (the
C++ standard library's, Xbyak's, vulkan.hpp's): what in the core asked.
Prints each name with its allocations, and fails when a name is not on
the allowlist; names on it no longer seen are printed, to be taken off.
A site reached from the core's signal handler fails whatever the list
says: malloc is not safe there.
"""
import re
import subprocess
import sys

HANDLER = 'signal_handler'
LIBRARY = ('std::', '__gnu_cxx::', 'Xbyak::', 'vk::', 'operator new', 'operator delete', 'VULKAN_HPP')


def qualified(name):
    """the function's qualified name, without its return type, template
    arguments and parameters"""
    depth = 0
    out = ''
    for ch in name:
        if ch == '<':
            depth += 1
        elif ch == '>':
            depth -= 1
        elif depth == 0:
            if ch == '(':
                break
            out += ch
    out = re.sub(r'\[abi:[^]]*\]', '', out)
    return out.split(' ')[-1] if out.strip() else name


def main():
    core, allowed_path, logs = sys.argv[1], sys.argv[2], sys.argv[3:]
    sites = []
    addrs = set()
    for path in logs:
        for line in open(path, errors='replace'):
            m = re.match(r'vk_hdr: site (heap|vulkan) (\d+)((?: 0x[0-9a-f]+)+)$', line.strip())
            if m:
                chain = m.group(3).split()
                sites.append((m.group(1), int(m.group(2)), chain, path))
                addrs.update(chain)
    addrs = sorted(addrs)
    frames = {}
    if addrs:
        out = subprocess.run(['addr2line', '-f', '-C', '-i', '-a', '-e', core] + addrs,
                             capture_output=True, text=True, check=True).stdout.splitlines()
        cur = None
        i = 0
        while i < len(out):
            if out[i].startswith('0x') and re.match(r'0x[0-9a-f]+$', out[i]):
                cur = '0x%x' % int(out[i], 16)
                frames[cur] = []
                i += 1
                continue
            frames[cur].append(qualified(out[i]))
            i += 2   # (the function, then its file and line)
    named = {}
    in_handler = set()
    for kind, n, chain, path in sites:
        name = None
        handler = any(f == HANDLER for a in chain for f in frames.get('0x%x' % int(a, 16), []))
        for a in chain:
            for f in frames.get('0x%x' % int(a, 16), []):
                if f != '??' and not f.startswith(LIBRARY):
                    name = f
                    break
            if name:
                break
        name = '%s %s' % (kind, name or '?')
        if handler:
            in_handler.add(name)
        named.setdefault(name, [0, set()])
        named[name][0] += n
        named[name][1].add(path.rsplit('/', 1)[-1].rsplit('.', 1)[0])
    allowed = set()
    for line in open(allowed_path):
        line = line.split('#', 1)[0].strip()
        if line:
            allowed.add(line)
    bad = 0
    for name in sorted(named, key=lambda k: -named[k][0]):
        ok = name in allowed
        bad += not ok
        print('%s %7d  %s  (%s)' % ('  ' if ok else '!!', named[name][0], name,
                                   ', '.join(sorted(named[name][1]))[:80]))
    for name in sorted(allowed - set(named)):
        print('   gone: %s (take it off %s)' % (name, allowed_path))
    for name in sorted(in_handler):
        print('!! %s allocates in the signal handler' % name)
        bad += 1
    if bad:
        print('vk_alloc_sites: %d allocation sites not on the list (!!): '
              'AGENTS.md, "No allocations during play"' % bad)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
