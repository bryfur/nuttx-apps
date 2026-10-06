# The terminal: colours, the streamed markdown, the spinner, tool lines,
# questions.  The desktop's Terminal (desk_vt.c) is an xterm subset:
# 256 colours, bold, dim, box drawing and block characters; no emoji.

import sys
import time

import _agent

ESC = '\x1b['
COLOR = True
TTY = True

RESET = ESC + '0m'


def setup(color):
    global COLOR, TTY
    import os
    TTY = _agent.isatty()
    COLOR = bool(color) and TTY and not os.getenv('NO_COLOR')


def c(code, s):
    if not COLOR:
        return s
    return ESC + code + 'm' + s + RESET


def accent(s):
    return c('38;5;173', s)


def bold(s):
    return c('1', s)


def dim(s):
    return c('2', s)


def red(s):
    return c('31', s)


def green(s):
    return c('32', s)


def yellow(s):
    return c('33', s)


def cyan(s):
    return c('36', s)


def out(s):
    sys.stdout.write(s)


def line(s=''):
    sys.stdout.write(s + '\n')


def width():
    return _agent.term_size()[0] if TTY else 100


MARK = '\u25cf'      # The tool line's mark (a bullet: the Terminal has it)
TREE = '\u2514'      # The result's corner


# ---------------------------------------------------------------------------
# The spinner, while waiting

class Spinner:
    FRAMES = '|/-\\'

    def __init__(self):
        self.text = None
        self.t0 = 0
        self.last = 0
        self.n = 0
        self.shown = False

    def start(self, text):
        self.text = text
        self.t0 = time.ticks_ms()
        self.last = 0
        self.tick(True)

    def tick(self, force=False):
        if self.text is None or not TTY:
            return
        now = time.ticks_ms()
        if not force and time.ticks_diff(now, self.last) < 150:
            return
        self.last = now
        self.n += 1
        secs = time.ticks_diff(now, self.t0) // 1000
        extra = (' %ds' % secs) if secs >= 2 else ''
        out('\r' + accent(self.FRAMES[self.n % 4]) + ' ' + dim(self.text + extra + ' (Esc to interrupt)') + ESC + 'K')
        self.shown = True

    def stop(self):
        if self.shown:
            out('\r' + ESC + 'K')
        self.shown = False
        self.text = None


# ---------------------------------------------------------------------------
# Markdown, as it streams: headings, lists, quotes, rules, code blocks,
# **bold**, *italic*, `code`; words wrapped at the terminal's width

PREFIX = '#`-*+ 0123456789.>|_='


class Markdown:
    def __init__(self):
        self.w = max(20, width() - 1)
        self.col = 0
        self.code = False          # In a ``` block
        self.skip = False          # The rest of this line is not shown
        self.start = ''            # A line's start, until its kind is known
        self.decided = False
        self.raw = False           # This line as it is (code, a table)
        self.indent = 0            # Where wrapped lines start
        self.word = []
        self.wordw = 0
        self.space = False
        self.bold = False
        self.ital = False
        self.icode = False
        self.star = False
        self.style = ''            # The line's own style (a heading)
        self.any = False

    def feed(self, s):
        for ch in s:
            self.char(ch)

    def char(self, ch):
        if not self.decided:
            if ch == '\n':
                self.decide(True)
                self.newline()
                return
            self.start += ch
            if ch in PREFIX and len(self.start) < 8:
                return
            self.decide(False)
            s, self.start = self.start, ''
            for x in s:
                self.body(x)
            return
        if ch == '\n':
            self.newline()
            return
        self.body(ch)

    # A line's kind, from its start
    def decide(self, whole):
        self.decided = True
        s = self.start
        st = s.lstrip()
        self.raw = False
        self.style = ''
        self.indent = 0
        if st.startswith('```'):
            self.code = not self.code
            self.skip = True
            self.start = ''
            return
        if self.code:
            self.raw = True
            self.emit('  ', 2)
            if COLOR:
                out(ESC + '36m')
            return
        if st.startswith('#'):
            n = len(st) - len(st.lstrip('#'))
            if n <= 6 and (len(st) == n or st[n] == ' ' or whole):
                self.start = st[n:].lstrip()
                self.style = '1;38;5;173' if n <= 2 else '1'
                self.setstyle()
                return
        if st.startswith('>'):
            self.emit(dim('\u2502 '), 2)
            self.indent = 2
            self.start = st[1:].lstrip()
            return
        if len(st) >= 3 and st.strip() in ('---', '***', '___') or \
                (st.startswith('---') and st.strip('-') == ''):
            if whole or len(st) >= 3:
                self.emit(dim('\u2500' * min(self.w, 40)), 0)
                self.skip = True
                self.start = ''
                return
        if st.startswith('|'):
            self.raw = True
            return
        lead = len(s) - len(st)
        if len(st) > 1 and st[0] in '-*+' and st[1] == ' ':
            self.emit(' ' * lead + accent('\u2022') + ' ', lead + 2)
            self.indent = lead + 2
            self.start = st[2:]
            return
        i = 0
        while i < len(st) and st[i] in '0123456789':
            i += 1
        if 0 < i < len(st) - 1 and st[i] == '.' and st[i + 1] == ' ':
            self.emit(' ' * lead + accent(st[:i + 1]) + ' ', lead + i + 2)
            self.indent = lead + i + 2
            self.start = st[i + 2:]
            return
        self.start = s

    def setstyle(self):
        codes = []
        if self.style:
            codes.append(self.style)
        if self.bold:
            codes.append('1')
        if self.ital:
            codes.append('3')
        if self.icode:
            codes.append('36')
        if COLOR:
            self.word.append(RESET + (ESC + ';'.join(codes) + 'm' if codes else ''))

    def emit(self, s, w):
        out(s)
        self.col += w
        self.any = True

    def body(self, ch):
        if self.skip:
            return
        if self.raw:
            out(ch)
            self.col += 1
            return
        if self.star:
            self.star = False
            if ch == '*':
                self.bold = not self.bold
                self.setstyle()
                return
            if ch != ' ' or self.ital:
                self.ital = not self.ital
                self.setstyle()
            else:
                self.put('*')
        if ch == '`':
            self.icode = not self.icode
            self.setstyle()
            return
        if ch == '*' and not self.icode:
            self.star = True
            return
        if ch == ' ' or ch == '\t':
            self.flush()
            self.space = True
            return
        self.put(ch)

    def put(self, ch):
        self.word.append(ch)
        self.wordw += 1

    def flush(self):
        if not self.word:
            return
        sp = 1 if (self.space and self.col > self.indent) else 0
        if self.col + sp + self.wordw > self.w and self.col > self.indent:
            out('\n' + ' ' * self.indent)
            self.col = self.indent
            sp = 0
        out(' ' * sp + ''.join(self.word))
        self.col += sp + self.wordw
        self.word = []
        self.wordw = 0
        self.space = False
        self.any = True

    def newline(self):
        if self.star:
            self.star = False
            self.put('*')
        self.flush()
        if self.raw and COLOR:
            out(RESET)
        if not (self.skip and self.col == 0):      # A fence shows nothing
            out('\n')
        self.col = 0
        self.space = False
        self.decided = False
        self.skip = False
        self.raw = False
        self.indent = 0
        if self.style or self.bold or self.ital or self.icode:
            self.style = ''
            self.bold = self.ital = self.icode = False
            if COLOR:
                out(RESET)

    def finish(self):
        if not self.decided and self.start:
            self.decide(True)
            s, self.start = self.start, ''
            for x in s:
                self.body(x)
        if self.star:
            self.star = False
            self.put('*')
        self.flush()
        if COLOR:
            out(RESET)
        if self.col > 0:
            out('\n')
        self.col = 0
        self.decided = False


# ---------------------------------------------------------------------------
# Tool lines

# What was shown last ('text', 'tool'): a blank line between the two

LAST = [None]


def gap(kind):
    if LAST[0] is not None and LAST[0] != kind:
        out('\n')
    LAST[0] = kind


def tool_line(name, arg):
    gap('tool')
    out(accent(MARK) + ' ' + bold(name) + (dim('(') + arg + dim(')') if arg else '') + '\n')


def tool_result(text, error=False):
    if not text:
        return
    lines = text.split('\n')
    first = True
    for ln in lines:
        pre = ('  ' + TREE + ' ') if first else '    '
        out(dim(pre) + (red(ln) if error else dim(ln)) + '\n')
        first = False


def diff(old, new, limit=12):
    # A few lines of an edit: what went, what came
    olines = old.split('\n')
    nlines = new.split('\n')
    shown = 0
    for ln in olines:
        if shown >= limit:
            out(dim('    ...\n'))
            break
        out('    ' + red('- ' + ln) + '\n')
        shown += 1
    for ln in nlines:
        if shown >= limit * 2:
            out(dim('    ...\n'))
            break
        out('    ' + green('+ ' + ln) + '\n')
        shown += 1


def error(msg):
    out(red('Error: ') + msg + '\n')


def note(msg):
    out(dim(msg) + '\n')


# ---------------------------------------------------------------------------
# Questions

def read_line():
    # A line of input that is not a terminal (a file): None at its end.
    # (sys.stdin.readline() never ends there: the port's input gives
    # Ctrl-D for ever)
    chars = []
    while True:
        ch = sys.stdin.read(1)
        if not ch or ch == '\x04':
            return ''.join(chars) if chars else None
        if ch == '\n' or ch == '\r':
            return ''.join(chars)
        chars.append(ch)


def ask_key(prompt, keys):
    # One key of keys (lower case); Enter is the first, Esc and Ctrl-C 'n'
    out(prompt)
    if not TTY:
        ln = read_line() or ''
        k = ln.strip().lower()[:1] or keys[0]
        out(k + '\n')
        return k if k in keys else 'n'
    while True:
        k = _agent.getkey()
        if k is None:
            continue
        if k == 'enter':
            k = keys[0]
        elif k in ('esc', 'ctrl-c', 'eof'):
            k = 'n'
        k = k.lower()
        if k in keys:
            out(k + '\n')
            return k


def ask_line(prompt, default='', mask=False):
    if not TTY:
        out(prompt)
        ln = read_line()
        if ln is None:
            raise EOFError
        out((('*' * len(ln)) if mask else ln) + '\n')
        return ln or default
    s = _agent.readline(prompt, None, mask=mask, text='' if mask else default)
    return s.strip() or default
