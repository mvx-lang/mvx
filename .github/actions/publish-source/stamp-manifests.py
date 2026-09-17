#!/usr/bin/env python3
# MVX — a native compiler and runtime for Pick/MultiValue BASIC.
# Copyright (C) 2026 Gordon Heydon.  GPL-2.0-only.
#
# stamp-manifests.py <dir> <version>
#
# Write a release's version into the manifests its source tarball ships:
# mvpkg.json's top-level "version", and line 2 of PKG where a package still has
# one (line 1 name, 2 version, 3 description, 4 systems).
#
# Without this the source tarball says whatever was last committed to those
# files, which is right only when someone remembered to edit them before
# tagging (mvx#205): mvpkg 1.24.0 shipped declaring itself 1.16.0, getopt 1.1.0
# declared 1.0.  The binaries were never wrong -- their jobs take the version
# from the tag -- so the source package gets the same source of truth.
#
# The JSON is rewritten IN PLACE, keeping the file's own layout: only the
# top-level "version" value changes.  A regex cannot tell a top-level key from
# a nested one, so every candidate line is tried and the result is parsed and
# compared; the first rewrite that changes the top-level version and nothing
# else is kept.  When none qualifies (no version key yet, or it shares a line
# with other keys) the file is re-serialised instead -- layout changes, content
# does not.  Either way the result is checked before this exits 0.
import json
import os
import re
import sys

VERSION_LINE = re.compile(r'^([ \t]*"version"[ \t]*:[ \t]*)"(?:[^"\\]|\\.)*"', re.M)


def fail(msg):
    sys.stderr.write('stamp-manifests: %s\n' % msg)
    sys.exit(1)


def stamp_json(path, ver):
    with open(path, encoding='utf-8') as f:
        text = f.read()
    try:
        data = json.loads(text)
    except ValueError as e:
        fail('%s is not valid JSON: %s' % (path, e))
    if not isinstance(data, dict):
        fail('%s: the top level is not an object' % path)

    want = dict(data)
    want['version'] = ver

    out = None
    for m in VERSION_LINE.finditer(text):
        trial = text[:m.start()] + m.group(1) + json.dumps(ver) + text[m.end():]
        try:
            if json.loads(trial) == want:
                out = trial
                break
        except ValueError:
            continue
    if out is None:
        out = json.dumps(want, indent=2, ensure_ascii=False) + '\n'

    with open(path, 'w', encoding='utf-8') as f:
        f.write(out)

    # Positive check on what is actually on disk now.
    with open(path, encoding='utf-8') as f:
        if json.load(f) != want:
            fail('%s: the stamped file does not read back as intended' % path)


def stamp_pkg(path, ver):
    with open(path, encoding='utf-8') as f:
        lines = f.read().split('\n')
    if len(lines) < 2:
        fail('%s has no line 2 to hold the version' % path)
    lines[1] = ver
    with open(path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))


def main(argv):
    if len(argv) != 3:
        fail('usage: stamp-manifests.py <dir> <version>')
    root, ver = argv[1], argv[2]
    if not ver:
        fail('empty version')
    if '\n' in ver or '"' in ver:
        fail('refusing a version containing a newline or quote: %r' % ver)

    done = []
    j = os.path.join(root, 'mvpkg.json')
    if os.path.isfile(j):
        stamp_json(j, ver)
        done.append('mvpkg.json')
    p = os.path.join(root, 'PKG')
    if os.path.isfile(p):
        stamp_pkg(p, ver)
        done.append('PKG')

    if done:
        print('stamped %s into: %s' % (ver, ', '.join(done)))
    else:
        print('stamped nothing: no mvpkg.json or PKG in %s' % root)


if __name__ == '__main__':
    main(sys.argv)
