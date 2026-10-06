# Small helpers MicroPython lacks: paths, text

import os

import _agent

S_IFDIR = 0x4000


def home():
    return _agent.home()


def join(a, *parts):
    for p in parts:
        if p.startswith('/'):
            a = p
        elif not a or a.endswith('/'):
            a = a + p
        else:
            a = a + '/' + p
    return a


def normpath(p):
    absolute = p.startswith('/')
    out = []
    for part in p.split('/'):
        if part in ('', '.'):
            continue
        if part == '..':
            if out and out[-1] != '..':
                out.pop()
            elif not absolute:
                out.append('..')
            continue
        out.append(part)
    s = '/'.join(out)
    return '/' + s if absolute else (s or '.')


def abspath(p):
    if p.startswith('~/') or p == '~':
        p = join(home(), p[2:])
    if not p.startswith('/'):
        p = join(os.getcwd(), p)
    return normpath(p)


def dirname(p):
    i = p.rfind('/')
    if i < 0:
        return ''
    return p[:i] or '/'


def basename(p):
    return p[p.rfind('/') + 1:]


def stat(p):
    try:
        return os.stat(p)
    except OSError:
        return None


def exists(p):
    return stat(p) is not None


def isdir(p):
    st = stat(p)
    return st is not None and (st[0] & S_IFDIR) != 0


def makedirs(p):
    if not p or isdir(p):
        return
    makedirs(dirname(p))
    try:
        os.mkdir(p)
    except OSError:
        if not isdir(p):
            raise


def to_str(b):
    # Bytes as text: UTF-8, else the bytes that are not ASCII as '?'
    try:
        return b.decode()
    except Exception:
        return bytes(c if c < 128 else 63 for c in b).decode()


def size(n):
    if n < 1024:
        return '%d B' % n
    if n < 1024 * 1024:
        return '%.1f KB' % (n / 1024)
    return '%.1f MB' % (n / 1048576)


def clip(s, n, what='characters'):
    # The start and the end of a long text, the middle left out
    if len(s) <= n:
        return s
    head = s[:n * 2 // 3]
    tail = s[-(n // 3):]
    return '%s\n\n[... %d %s left out ...]\n\n%s' % (head, len(s) - len(head) - len(tail), what, tail)


def plural(n, word):
    return '%d %s%s' % (n, word, '' if n == 1 else 's')


def oneline(s, n=60):
    s = s.replace('\n', ' ').strip()
    return s if len(s) <= n else s[:n - 3] + '...'
