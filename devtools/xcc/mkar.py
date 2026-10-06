#!/usr/bin/env python3
############################################################################
# apps/devtools/xcc/mkar.py
#
# SPDX-License-Identifier: MIT
#
# A static library (System V / GNU ar, with the symbol index xcc's
# linkers look symbols up in) of xcc's objects: WebAssembly objects (their
# "linking" section) and 32-bit ELF objects.  Used in place of llvm-ar,
# which the build host may not have.
#
#   mkar.py LIB.a OBJ...
#
############################################################################

import os
import struct
import sys


def leb(b, i):
    """An unsigned LEB128 at b[i]: (value, next index)"""
    v = s = 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7f) << s
        s += 7
        if c < 0x80:
            return v, i


def wasm_symbols(b):
    """The global symbols a WebAssembly object defines"""
    syms = []
    i = 8
    while i < len(b):
        sid, i = b[i], i + 1
        size, i = leb(b, i)
        end = i + size
        if sid == 0:
            nlen, j = leb(b, i)
            name = b[j:j + nlen].decode()
            j += nlen
            if name == 'linking':
                _version, j = leb(b, j)
                while j < end:
                    kind, j = b[j], j + 1
                    ssize, j = leb(b, j)
                    send = j + ssize
                    if kind == 8:                       # WASM_SYMBOL_TABLE
                        count, k = leb(b, j)
                        for _ in range(count):
                            skind, k = b[k], k + 1
                            flags, k = leb(b, k)
                            undefined = flags & 0x10
                            sname = None
                            if skind in (0, 2, 4, 5):   # function global tag table
                                _index, k = leb(b, k)
                                if not undefined or flags & 0x40:
                                    n, k = leb(b, k)
                                    sname = b[k:k + n].decode()
                                    k += n
                            elif skind == 1:            # data
                                n, k = leb(b, k)
                                sname = b[k:k + n].decode()
                                k += n
                                if not undefined:
                                    for _ in range(3):
                                        _x, k = leb(b, k)
                            elif skind == 3:            # section
                                _index, k = leb(b, k)
                            if sname and not undefined and not flags & 0x02:
                                syms.append(sname)
                    j = send
        i = end
    return syms


def elf32_symbols(b):
    """The global symbols a little-endian 32-bit ELF object defines"""
    shoff, = struct.unpack_from('<I', b, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', b, 0x2e)
    secs = [struct.unpack_from('<IIIIIIIIII', b, shoff + n * shentsize)
            for n in range(shnum)]
    syms = []
    for s in secs:
        if s[1] != 2:                                   # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for off in range(s[4], s[4] + s[5], 16):
            name, _value, _size, info, _other, shndx = struct.unpack_from('<IIIBBH', b, off)
            bind = info >> 4
            if bind in (1, 2) and shndx != 0:           # GLOBAL, WEAK; defined
                n0 = strtab[4] + name
                syms.append(b[n0:b.index(b'\0', n0)].decode())
    return syms


def symbols(b):
    if b[:4] == b'\0asm':
        return wasm_symbols(b)
    if b[:4] == b'\x7fELF' and b[4] == 1:
        return elf32_symbols(b)
    sys.exit('mkar: not a WebAssembly or 32-bit ELF object')


def header(name, size):
    h = b'%-16s%-12d%-6d%-6d%-8o%-10d`\n' % (name, 0, 0, 0, 0o644, size)
    assert len(h) == 60
    return h


def main():
    if len(sys.argv) < 3:
        sys.exit('usage: mkar.py LIB.a OBJ...')
    out, objs = sys.argv[1], sys.argv[2:]
    datas = [open(o, 'rb').read() for o in objs]
    names = [os.path.basename(o) for o in objs]

    # Long names (over 15 characters) in the "//" member

    longnames = b''
    arnames = []
    for n in names:
        if len(n) <= 15:
            arnames.append(n.encode() + b'/')
        else:
            arnames.append(b'/%d' % len(longnames))
            longnames += n.encode() + b'/\n'

    syms = [(s, k) for k, d in enumerate(datas) for s in symbols(d)]
    strtab = b''.join(s.encode() + b'\0' for s, _ in syms)
    index_size = 4 + 4 * len(syms) + len(strtab)
    pos = 8 + 60 + index_size + (index_size & 1)
    if longnames:
        pos += 60 + len(longnames) + (len(longnames) & 1)
    offsets = []
    for d in datas:
        offsets.append(pos)
        pos += 60 + len(d) + (len(d) & 1)

    a = bytearray(b'!<arch>\n')
    a += header(b'/', index_size)
    a += struct.pack('>I', len(syms))
    for _, k in syms:
        a += struct.pack('>I', offsets[k])
    a += strtab
    if index_size & 1:
        a += b'\n'
    if longnames:
        a += header(b'//', len(longnames)) + longnames
        if len(longnames) & 1:
            a += b'\n'
    for n, d in zip(arnames, datas):
        a += header(n, len(d)) + d
        if len(d) & 1:
            a += b'\n'
    with open(out + '.tmp', 'wb') as f:
        f.write(a)
    os.replace(out + '.tmp', out)


main()
