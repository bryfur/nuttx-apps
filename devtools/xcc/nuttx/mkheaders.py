#!/usr/bin/env python3
############################################################################
# apps/devtools/xcc/nuttx/mkheaders.py
#
# SPDX-License-Identifier: MIT
#
# The NuttX headers xcc's modules are compiled with (on the board: under
# CONFIG_DEVTOOLS_XCC_ROOT/arm/nuttx): the firmware's own, those the
# standard and POSIX headers of headers.list pull in, found by running
# xcc's preprocessor on them (its line markers name every file read).
#
#   mkheaders.py CPP NUTTX_INCLUDE OVERLAY HEADERS.LIST OUTDIR
#
############################################################################

import os
import re
import shutil
import subprocess
import sys
import tempfile

PREDEFINED = [
    '-D__XCC', '-D__ILP32__', '-D__arm__', '-D__thumb__', '-D__thumb2__',
    '-D__ARMEL__', '-D__ARM_EABI__', '-D__ARM_ARCH=8', '-D__ARM_ARCH_8M_MAIN__',
    '-D__ARM_PCS_VFP', '-D__VFP_FP__', '-D__ARM_FP=4', '-D__SOFTFP_DOUBLE__',
    '-D__STDC__', '-D__STDC_VERSION__=199901L',
    '-D__SIZEOF_POINTER__=4', '-D__SIZEOF_INT__=4', '-D__SIZEOF_LONG__=4',
    '-D__SIZEOF_LONG_LONG__=8', '-D__SIZEOF_SIZE_T__=4',
    '-D__NuttX__', '-D__asm__=__asm', '-D__volatile__=volatile',
    '-D__inline__=inline', '-D__restrict=restrict', '-D__restrict__=restrict',
]


def strip(text):
    """The comments and blank lines out: a third the size, and less for
    the board's preprocessor to read"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j) or ' ')
            i = j
        elif c == '/' and text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    lines = [l.rstrip() for l in ''.join(out).split('\n')]
    res = []
    for l in lines:
        if l == '':
            continue
        if res and res[-1].endswith('\\') and l.strip() == '':
            continue
        res.append(l)
    return '\n'.join(res) + '\n'


def main():
    cpp, incdir, overlay, listfile, outdir = sys.argv[1:6]
    incdir = os.path.normpath(incdir)
    headers = [h.strip() for h in open(listfile) if h.strip() and not h.startswith('#')]
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'all.c')
        with open(src, 'w') as f:
            for h in headers:
                f.write('#include <%s>\n' % h)
        p = subprocess.run([cpp] + PREDEFINED + ['-I', overlay, '-I', incdir, src],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if p.returncode != 0:
            sys.exit('mkheaders: %s' % p.stderr.decode(errors='replace'))
    files = set()
    for m in re.finditer(rb'^# \d+ "([^"]+)"', p.stdout, re.M):
        path = os.path.normpath(m.group(1).decode())
        if path.startswith(incdir + os.sep):
            files.add(os.path.relpath(path, incdir))
    if os.path.isdir(outdir):
        shutil.rmtree(outdir)
    for rel in sorted(files):
        dst = os.path.join(outdir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(os.path.join(incdir, rel), errors='replace') as f:
            text = f.read()
        with open(dst, 'w') as f:
            f.write(strip(text))
    print('mkheaders: %d headers' % len(files))


main()
