#!/usr/bin/env python3
# tools/mkpyexamples.py FILE.py ...: the Python IDE's examples as C
# (desk_pyexamples.c, made by the Makefile): each file's name and text
import os
import sys

out = ['/* Made by tools/mkpyexamples.py from the MicroPython port\'s',
       ' * examples: do not edit */', '', '#include "desk.h"', '',
       'const struct desk_pyexample_s g_desk_pyexamples[] =', '{']
for path in sys.argv[1:]:
    text = open(path, encoding='utf-8').read()
    lines = [json_line for json_line in text.split('\n')]
    out.append('  {')
    out.append('    "%s",' % os.path.basename(path))
    for i, ln in enumerate(lines):
        esc = ln.replace('\\', '\\\\').replace('"', '\\"').replace('\t', '\\t')
        nl = '\\n' if i < len(lines) - 1 else ''
        out.append('    "%s%s"' % (esc, nl))
    out[-1] += ','
    out.append('  },')
out += ['};', '',
        'const int g_desk_npyexamples = %d;' % len(sys.argv[1:]), '']
sys.stdout.write('\n'.join(out))
