# The agent's tools: files, search, NSH commands, web pages, a task list.
#
# Each tool: a name, a description and a JSON schema for the model; a
# kind for the permissions ('read' runs, 'write', 'exec' and 'net' ask
# unless allowed); run(args) -> (result for the model, summary for the
# screen).  Errors are ToolError: the model sees them as is_error results.

import json
import os
import time

import _agent

from . import llm
from . import ui
from . import util


class ToolError(Exception):
    pass


MAX_RESULT = 40000          # Characters of a result the model gets
READ_LINES = 2000

# Shell commands that an "always" never covers (asked every time): those
# that run other code (an "always" for python would allow any program) or
# destroy; and what a first word says nothing of: a redirection or a
# background job (writes, or keeps running), the agent's own files
NO_ALWAYS = ('python', 'sh', 'source', '.', 'exec', 'wasm', 'xcc', 'wcc', 'agent',
             'rm', 'rmdir', 'mw', 'mb', 'mh', 'dd', 'mkfatfs', 'mkrd', 'reboot',
             'kill', 'pkill', 'umount')

# The agent's files: the API key and the conversations.  read, grep and
# glob refuse them; a shell command naming them always asks.
SECRETS = ('.agent.json', '.agent_history')
LINE_MAX = 2000


def obj(props, required=()):
    return {'type': 'object', 'properties': props, 'required': list(required)}


def string(desc):
    return {'type': 'string', 'description': desc}


def integer(desc):
    return {'type': 'integer', 'description': desc}


def boolean(desc):
    return {'type': 'boolean', 'description': desc}


class Tools:
    def __init__(self, cfg, pool):
        self.cfg = cfg
        self.pool = pool
        self.read_files = {}     # path: mtime when read (edits need a read first)
        self.todos = []
        self.defs = [
            {'name': 'read', 'kind': 'read', 'run': self.t_read,
             'description': 'Read a text file. Lines come numbered (cat -n style: number, tab, '
                            'text). Up to 2000 lines from offset (1-based); use offset and limit '
                            'for long files. Read a file before editing it.',
             'schema': obj({'path': string('The file (absolute, or relative to the current '
                                           'directory)'),
                            'offset': integer('The first line to read (1-based)'),
                            'limit': integer('How many lines to read')}, ['path'])},
            {'name': 'write', 'kind': 'write', 'run': self.t_write,
             'description': 'Write a file whole: create it (and its folders) or replace it. '
                            'An existing file must have been read first. Prefer edit for '
                            'changes to an existing file.',
             'schema': obj({'path': string('The file'),
                            'content': string('Its whole new content')}, ['path', 'content'])},
            {'name': 'edit', 'kind': 'write', 'run': self.t_edit,
             'description': 'Replace an exact string in a file. old_string must occur exactly '
                            'once (give enough surrounding lines to make it unique) unless '
                            'replace_all is true. Match the file\'s text exactly, indentation '
                            'included, without the line numbers read shows. The file must have '
                            'been read first.',
             'schema': obj({'path': string('The file'),
                            'old_string': string('The text to replace'),
                            'new_string': string('The text to put instead'),
                            'replace_all': boolean('Replace every occurrence')},
                           ['path', 'old_string', 'new_string'])},
            {'name': 'ls', 'kind': 'read', 'run': self.t_ls,
             'description': 'List a folder: its entries, folders ending in /, with sizes.',
             'schema': obj({'path': string('The folder (default: the current one)')})},
            {'name': 'glob', 'kind': 'read', 'run': self.t_glob,
             'description': 'Find files by name pattern: * ? [abc] {a,b} within a name, ** for '
                            'any depth of folders (e.g. "**/*.py", "src/*.{c,h}"). Newest first.',
             'schema': obj({'pattern': string('The pattern'),
                            'path': string('The folder to search from (default: the current '
                                           'one)')}, ['pattern'])},
            {'name': 'grep', 'kind': 'read', 'run': self.t_grep,
             'description': 'Search file contents for a regular expression (MicroPython re: '
                            '. [] [^] * + ? | () ^ $ \\d \\w \\s; no {m,n}, no lookaround), '
                            'recursively. Results are path:line:text. Hidden folders are '
                            'skipped.',
             'schema': obj({'pattern': string('The regular expression (or plain text with '
                                              'fixed)'),
                            'path': string('A folder or a file (default: the current folder)'),
                            'glob': string('Only files whose names match, e.g. "*.c"'),
                            'ignore_case': boolean('Case-insensitive'),
                            'fixed': boolean('The pattern is plain text'),
                            'files_only': boolean('Only the names of the files that match'),
                            'max': integer('The most matches to return (default 100)')},
                           ['pattern'])},
            {'name': 'shell', 'kind': 'exec', 'run': self.t_shell,
             'description': 'Run a command line in NSH, the NuttX shell. It is NOT bash: '
                            'commands are NSH builtins (ls, cat, cp, mv, rm, mkdir, echo, ps, '
                            'free, df, mount, date, uname, kill, sleep, hexdump, help ...) and '
                            'programs (python, curl ...); ";" separates commands, "|" pipes, '
                            '">" and ">>" redirect; there is no "&&" or "||", no $(...), no '
                            'wildcards (use the glob tool), no "cd -". stdout and stderr are '
                            'captured, stdin is empty; the exit status is 0 or 1. Each call is '
                            'a new shell in the current directory. Long-running or '
                            'interactive programs will time out.',
             'schema': obj({'command': string('The NSH command line'),
                            'timeout': integer('Seconds before it is stopped (default 120)')},
                           ['command'])},
            {'name': 'web_fetch', 'kind': 'net', 'run': self.t_fetch,
             'description': 'Fetch a web page or file over HTTP(S) (GET). HTML comes back as '
                            'text (tags stripped, links kept), cut to max_chars.',
             'schema': obj({'url': string('The URL, http:// or https://'),
                            'max_chars': integer('The most characters to return (default '
                                                 '20000)')}, ['url'])},
            {'name': 'todo', 'kind': 'read', 'run': self.t_todo,
             'description': 'Keep a task list for multi-step work: give the whole list each '
                            'time, each item with a status (pending, in_progress, completed). '
                            'Use it for tasks of three steps or more; keep one in_progress.',
             'schema': obj({'todos': {'type': 'array', 'items': obj(
                 {'content': string('The task'),
                  'status': {'type': 'string', 'enum': ['pending', 'in_progress', 'completed']}},
                 ['content', 'status'])}}, ['todos'])},
        ]
        self.by_name = {}
        for d in self.defs:
            self.by_name[d['name']] = d

    def schemas(self):
        return [{'name': d['name'], 'description': d['description'], 'schema': d['schema']}
                for d in self.defs]

    # --- what the screen shows of a call

    def label(self, name, a):
        if name in ('read', 'write', 'edit'):
            s = a.get('path', '')
            if name == 'read' and a.get('offset'):
                s += ':%s' % a['offset']
            return s
        if name == 'ls':
            return a.get('path', '.')
        if name in ('glob', 'grep'):
            s = a.get('pattern', '')
            if a.get('path'):
                s += ' in ' + a['path']
            return s
        if name == 'shell':
            return util.oneline(a.get('command', ''), 70)
        if name == 'web_fetch':
            return a.get('url', '')
        return ''

    # --- permissions

    def rule(self, name, a):
        # What "always" allows: the command's first word, the host, the tool
        if name == 'shell':
            w = a.get('command', '').strip().split()
            return 'shell:' + (w[0] if w else '')
        if name == 'web_fetch':
            try:
                return 'web_fetch:' + llm.parse_url(a.get('url', ''))[1]
            except ValueError:
                return 'web_fetch'
        return name

    def allowed(self, name, a, session_allow):
        d = self.by_name[name]
        if d['kind'] == 'read' or self.cfg.get('auto_approve'):
            return True
        allow = list(self.cfg.get('allow') or []) + session_allow
        if name in allow:
            return True                  # The whole tool ("shell": any command)
        if name == 'shell':
            # Each command (; and |) by its first word, and nothing a first
            # word does not say
            cmd = a.get('command', '')
            if not self.can_always(name, a):
                return False
            for part in cmd.replace('|', ';').split(';'):
                w = part.strip().split()
                if w and ('shell:' + w[0]) not in allow:
                    return False
            return True
        return self.rule(name, a) in allow

    def can_always(self, name, a):
        # Whether "always" may be offered (and a rule may allow it)
        if name != 'shell':
            return True
        cmd = a.get('command', '')
        if '>' in cmd or '&' in cmd or any(s in cmd for s in SECRETS):
            return False
        for part in cmd.replace('|', ';').split(';'):
            w = part.strip().split()
            if w and w[0] in NO_ALWAYS:
                return False
        return True

    # --- running

    def run(self, name, a):
        d = self.by_name.get(name)
        if d is None:
            raise ToolError('no tool named ' + name)
        return d['run'](a)

    # --- the tools

    def path(self, a, key='path', default=None):
        p = a.get(key) or default
        if not p:
            raise ToolError('missing ' + key)
        if not isinstance(p, str):
            raise ToolError(key + ' must be a string')
        return util.abspath(p)

    def secret(self, p):
        if util.basename(p) in SECRETS:
            raise ToolError('refused: %s holds the API key or the conversations' % p)

    def t_read(self, a):
        p = self.path(a)
        self.secret(p)
        st = util.stat(p)
        if st is None:
            raise ToolError('no such file: ' + p)
        if st[0] & util.S_IFDIR:
            raise ToolError(p + ' is a folder (use ls)')
        offset = max(1, int(a.get('offset') or 1))
        limit = max(1, min(int(a.get('limit') or READ_LINES), READ_LINES))
        out = []
        n = 0
        total = 0
        more = False
        size = 0
        with open(p, 'rb') as f:
            head = f.read(1024)
            if b'\0' in head:
                raise ToolError('%s is a binary file (%s)' % (p, util.size(st[6])))
            f.seek(0)
            while True:
                ln = f.readline()
                if not ln:
                    break
                n += 1
                if n < offset:
                    continue
                if total >= limit or size > MAX_RESULT:
                    more = True
                    break
                s = util.to_str(ln).rstrip('\r\n')
                if len(s) > LINE_MAX:
                    s = s[:LINE_MAX] + ' [line cut]'
                out.append('%6d\t%s' % (n, s))
                size += len(s) + 8
                total += 1
        self.read_files[p] = st[8]
        if not out:
            if n == 0:
                return '(empty file)', 'empty'
            raise ToolError('the file has %d lines; offset %d is past the end' % (n, offset))
        text = '\n'.join(out)
        if more:
            text += '\n\n(more lines follow; read on with offset %d)' % (offset + total)
        return text, 'Read %s' % util.plural(total, 'line')

    def check_read(self, p):
        st = util.stat(p)
        if st is None:
            return None
        if p not in self.read_files:
            raise ToolError('read %s before changing it' % p)
        if self.read_files[p] != st[8]:
            raise ToolError('%s changed since it was read: read it again' % p)
        return st

    def t_write(self, a):
        p = self.path(a)
        content = a.get('content')
        if not isinstance(content, str):
            raise ToolError('content must be a string')
        if util.isdir(p):
            raise ToolError(p + ' is a folder')
        existed = self.check_read(p) is not None
        util.makedirs(util.dirname(p))
        with open(p, 'w') as f:
            f.write(content)
        st = util.stat(p)
        if st:
            self.read_files[p] = st[8]
        n = content.count('\n') + (0 if content.endswith('\n') or not content else 1)
        return ('%s %s (%s)' % ('Replaced' if existed else 'Created', p, util.plural(n, 'line')),
                '%s %s' % ('Wrote' if existed else 'Created', util.plural(n, 'line')))

    def t_edit(self, a):
        p = self.path(a)
        old = a.get('old_string')
        new = a.get('new_string')
        if not isinstance(old, str) or not isinstance(new, str):
            raise ToolError('old_string and new_string must be strings')
        if old == new:
            raise ToolError('old_string and new_string are the same')
        st = self.check_read(p)
        if st is None:
            raise ToolError('no such file: ' + p)
        with open(p, 'rb') as f:
            text = util.to_str(f.read())
        if old == '':
            raise ToolError('old_string is empty (use write to create a file)')
        n = text.count(old)
        if n == 0:
            raise ToolError('old_string was not found in %s (match the text exactly, '
                            'without line numbers)' % p)
        if n > 1 and not a.get('replace_all'):
            raise ToolError('old_string occurs %d times in %s: give more context to make it '
                            'unique, or replace_all' % (n, p))
        line = text[:text.find(old)].count('\n') + 1
        text = text.replace(old, new) if a.get('replace_all') else text.replace(old, new, 1)
        with open(p, 'w') as f:
            f.write(text)
        st = util.stat(p)
        if st:
            self.read_files[p] = st[8]
        self.last_diff = (old, new)
        what = 'Replaced %d occurrences' % n if n > 1 else 'Edited at line %d' % line
        return '%s in %s' % (what, p), what

    def t_ls(self, a):
        p = self.path(a, default='.')
        if not util.isdir(p):
            raise ToolError('not a folder: ' + p)
        rows = []
        for e in sorted(os.ilistdir(p), key=lambda e: e[0]):
            name = e[0]
            full = util.join(p, name)
            st = util.stat(full)
            if st is None:
                rows.append(name)
            elif st[0] & util.S_IFDIR:
                rows.append(name + '/')
            else:
                rows.append('%-40s %s' % (name, util.size(st[6])))
            if len(rows) >= 1000:
                rows.append('... (more)')
                break
        if not rows:
            return '(empty folder)', 'empty'
        return p + ':\n' + '\n'.join(rows), util.plural(len(rows), 'entry')

    def t_glob(self, a):
        pat = a.get('pattern')
        if not pat:
            raise ToolError('missing pattern')
        root = self.path(a, default='.')
        found = [f for f in _agent.glob(pat, root, max=500)
                 if util.basename(f[0]) not in SECRETS]
        if not found:
            return 'No files match', 'no files'
        lines = [f[0] for f in found]
        more = ' (the first 500)' if len(found) >= 500 else ''
        return '\n'.join(lines) + more, util.plural(len(found), 'file') + more

    def t_grep(self, a):
        pat = a.get('pattern')
        if not pat:
            raise ToolError('missing pattern')
        root = self.path(a, default='.')
        try:
            res, nfiles, trunc = _agent.grep(pat, root, glob=a.get('glob') or None,
                                             icase=bool(a.get('ignore_case')),
                                             fixed=bool(a.get('fixed')),
                                             files=bool(a.get('files_only')),
                                             max=int(a.get('max') or 100))
        except ValueError as e:
            raise ToolError('%s (MicroPython re syntax; or use fixed)' % e)
        except OSError as e:
            raise ToolError(str(e.args[-1]) if e.args else str(e))
        res = [r for r in res
               if util.basename(r if a.get('files_only') else r[0]) not in SECRETS]
        if not res:
            return 'No matches (%s searched)' % util.plural(nfiles, 'file'), 'no matches'
        if a.get('files_only'):
            lines = res
        else:
            lines = ['%s:%d:%s' % r for r in res]
        text = '\n'.join(lines)
        if trunc:
            text += '\n(more matches: narrow the search, or raise max)'
        return text, '%s in %s%s' % (util.plural(len(res), 'match' if not a.get('files_only') else 'file'),
                                     util.plural(nfiles, 'file'), ' (more)' if trunc else '')

    def t_shell(self, a):
        cmd = a.get('command')
        if not cmd or not isinstance(cmd, str):
            raise ToolError('missing command')
        timeout = int(a.get('timeout') or self.cfg.get('shell_timeout', 120))
        p = _agent.Proc(cmd)
        chunks = []
        size = 0
        t0 = time.ticks_ms()
        timed_out = False
        try:
            while True:
                b = p.read(150)
                if b is None:
                    break
                if b:
                    if size < 256 * 1024:
                        chunks.append(b)
                    size += len(b)
                if self.spinner is not None:
                    self.spinner.tick()
                if time.ticks_diff(time.ticks_ms(), t0) > timeout * 1000:
                    timed_out = True
                    break
            status = None if timed_out else p.wait(2000)
        finally:
            p.close()
        text = util.to_str(b''.join(chunks)).replace('\r\n', '\n')
        chunks = None
        text = text.rstrip('\n')
        if size > 256 * 1024:
            text += '\n[output cut at 256 KB of %s]' % util.size(size)
        text = util.clip(text, MAX_RESULT)
        if timed_out:
            text += '\n[stopped after %d s]' % timeout
        elif status:
            text += '\n[exit status %d]' % status
        if not text.strip():
            text = '(no output)'
        n = text.count('\n') + 1
        summary = '\n'.join(text.split('\n')[:4])
        if n > 4:
            summary += '\n... (%d more lines)' % (n - 4)
        return text, summary

    spinner = None
    permit = None                        # The session's: asks for a redirect's host

    def t_fetch(self, a):
        url = a.get('url')
        if not url:
            raise ToolError('missing url')
        maxc = int(a.get('max_chars') or 20000)
        for hop in range(6):
            try:
                tls, host, port, path = llm.parse_url(url)
            except ValueError as e:
                raise ToolError(str(e))
            c = None
            try:
                c, status = self.pool.request(tls, host, port, 'GET', path or '/',
                                              'User-Agent: fpc-agent/0.1\r\nAccept: */*\r\n',
                                              None)
            except OSError as e:
                raise ToolError(str(e.args[-1]) if e.args else str(e))
            if status in (301, 302, 303, 307, 308):
                loc = c.header('location')
                self.drain(c)
                if not loc:
                    raise ToolError('redirect without a Location')
                if loc.startswith('/'):
                    loc = ('https://' if tls else 'http://') + host + \
                        ('' if port in (80, 443) else ':%d' % port) + loc
                try:
                    ntls, nhost = llm.parse_url(loc)[:2]
                except ValueError as e:
                    raise ToolError('redirect to %s: %s' % (loc, e))
                if tls and not ntls:
                    raise ToolError('refused: a redirect from https to http (%s)' % loc)
                if nhost != host and self.permit is not None and \
                        not self.permit('web_fetch', {'url': loc}):
                    raise ToolError('refused: the redirect to %s' % loc)
                url = loc
                continue
            break
        else:
            raise ToolError('too many redirects')
        ctype = c.header('content-type') or ''
        data = []
        n = 0
        while n < 512 * 1024:
            b = c.read(4096)
            if not b:
                break
            data.append(b)
            n += len(b)
            if self.spinner is not None:
                self.spinner.tick()
        if not c.reusable:
            self.pool.drop(tls, host, port)
        body = b''.join(data)
        data = None
        if status >= 400:
            raise ToolError('HTTP %d from %s' % (status, url))
        if 'html' in ctype or body[:200].lstrip().lower().startswith(b'<!doctype html') or \
                body[:200].lstrip().lower().startswith(b'<html'):
            text = _agent.html2text(body, maxc + 1)
        else:
            if b'\0' in body[:1024]:
                raise ToolError('%s is binary (%s, %s)' % (url, ctype or '?', util.size(len(body))))
            text = util.to_str(body)
        cut = len(text) > maxc
        text = text[:maxc]
        if cut:
            text += '\n[cut at %d characters]' % maxc
        return text, 'HTTP %d, %s%s' % (status, util.size(len(body)), ', cut' if cut else '')

    def drain(self, c):
        while c.read(4096):
            pass

    def t_todo(self, a):
        todos = a.get('todos')
        if not isinstance(todos, list):
            raise ToolError('todos must be a list')
        self.todos = []
        for t in todos:
            if isinstance(t, dict) and t.get('content'):
                self.todos.append({'content': str(t['content']),
                                   'status': t.get('status', 'pending')})
        done = sum(1 for t in self.todos if t['status'] == 'completed')
        return ('The task list is updated (%d of %d done). Keep it current.'
                % (done, len(self.todos)), self.todo_text())

    def todo_text(self):
        rows = []
        for t in self.todos:
            s = t['status']
            box = '[x]' if s == 'completed' else '[>]' if s == 'in_progress' else '[ ]'
            text = t['content']
            if s == 'completed':
                text = ui.dim(text)
            elif s == 'in_progress':
                text = ui.bold(text)
            rows.append(box + ' ' + text)
        return '\n'.join(rows)
