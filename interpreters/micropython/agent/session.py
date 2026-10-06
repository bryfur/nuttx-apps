# The agent's loop: the conversation, a request streamed to the screen,
# the tools the model calls (asking first when they change things), their
# results back, again until the model is done.  Usage counted; the
# conversation compacted when it grows too big; Ctrl-C or Esc interrupts.
#
# The history is only ever appended to (the API binds the model's
# thinking to what came before it, and prompt caching wants the same
# prefix); compaction replaces it all with a summary.

import gc
import json
import time

import _agent

from . import llm
from . import prompt
from . import tools
from . import ui
from . import util

NAMES = {'read': 'Read', 'write': 'Write', 'edit': 'Edit', 'ls': 'List', 'glob': 'Glob',
         'grep': 'Grep', 'shell': 'Shell', 'web_fetch': 'Fetch', 'todo': 'Todo'}

COMPACT = ('Your task now is to write a summary of the conversation so far, which will '
           'replace it: the next part of the session will only have your summary. Keep what '
           'is needed to go on: the user\'s requests and intent, the decisions made, the files '
           'and commands involved (exact paths and names), changes made and their state, '
           'errors met and how they were solved, the task list, what was being worked on last '
           'and what is next. Be complete but brief; plain text with short sections. Do not '
           'call tools.')

INTERRUPTED = '[The user interrupted this response.]'


def result(blk, text, error=False):
    r = {'type': 'tool_result', 'tool_use_id': blk['id'], 'content': text}
    if error:
        r['is_error'] = True
    return r


class Session:
    def __init__(self, cfg, interactive=True):
        self.cfg = cfg
        self.interactive = interactive
        self.pool = llm.Pool(cfg.get('timeout', 300), cfg.get('verify_tls', True))
        self.provider = llm.make(cfg, self.pool)
        self.tools = tools.Tools(cfg, self.pool)
        self.spinner = ui.Spinner()
        self.tools.spinner = self.spinner
        self.allow = []                  # "always" this session
        self.seen = (None, 0)            # The tool input last previewed
        self.clear()
        self.requests = 0
        self.tok_in = 0
        self.tok_out = 0
        self.tok_cache_read = 0
        self.tok_cache_write = 0
        self.cost = 0.0
        self.priced = True

    def clear(self):
        self.messages = []
        self.system = prompt.system()
        self.last_input = 0
        self.provider.last_body = 0
        self.tools.read_files = {}

    def set_provider(self, name=None):
        self.provider = llm.make(self.cfg, self.pool, name)

    # --- the conversation

    def add_user(self, blocks):
        if self.messages and self.messages[-1]['role'] == 'user':
            self.messages[-1]['content'].extend(blocks)
        else:
            self.messages.append({'role': 'user', 'content': blocks})

    def ask(self, text):
        # One request of the user's, to the end of the model's work.  True
        # when it ended normally.
        self.maybe_compact()
        ui.LAST[0] = None
        self.add_user([{'type': 'text', 'text': text}])
        _agent.interrupt_keys(True)
        try:
            for step in range(200):
                res = self.request()
                if res is None:
                    return False
                if res.stop == 'refusal':
                    ui.note('(The model declined to continue.)')
                elif res.stop == 'max_tokens' and not res.tool_calls():
                    ui.note('(The answer was cut off: max_tokens.)')
                calls = res.tool_calls()
                if not calls:
                    return True
                if not self.run_tools(calls):
                    return False
            ui.note('(Stopped after 200 steps.)')
            return False
        finally:
            _agent.interrupt_keys(False)
            gc.collect()

    # --- a request, streamed

    def request(self, no_tools=False, max_tokens=0, show=True):
        res = None
        for attempt in range(4):
            res = llm.Result()
            st = {'md': None, 'thinking': False}
            if show:
                self.spinner.start('Thinking')

            def ev(kind, data):
                if kind == 'tool_delta':
                    if show:
                        self.preview(data)
                elif kind == 'wait':
                    self.spinner.tick()
                elif kind == 'text':
                    if not show:
                        return
                    self.spinner.stop()
                    if st['thinking']:
                        ui.out('\n' if ui.TTY else '')
                        st['thinking'] = False
                    if st['md'] is None:
                        ui.gap('text')
                        st['md'] = ui.Markdown()
                    st['md'].feed(data)
                elif kind == 'thinking':
                    if show and self.provider.s.get('thinking') == 'summarized':
                        self.spinner.stop()
                        st['thinking'] = True
                        ui.out(ui.dim(data))
                    else:
                        self.spinner.tick()
                elif kind == 'tool_start':
                    if st['md'] is not None:
                        st['md'].finish()
                        st['md'] = None
                    if show:
                        self.spinner.start('Writing ' + NAMES.get(data['name'], data['name']))

            try:
                self.provider.stream(self.system, self.tools.schemas(), self.messages, ev, res,
                                     max_tokens=max_tokens, no_tools=no_tools)
            except KeyboardInterrupt:
                self.spinner.stop()
                if st['md'] is not None:
                    st['md'].finish()
                self.pool.drop(self.provider.tls, self.provider.host, self.provider.port)
                _agent.flush_input()
                if no_tools:
                    raise
                content = res.content(partial=True)
                if content:
                    self.messages.append({'role': 'assistant', 'content': content})
                self.add_user([{'type': 'text', 'text': INTERRUPTED}])
                ui.out(ui.yellow('Interrupted') + ui.dim(' - what should I do instead?') + '\n')
                return None
            except llm.ApiError as e:
                self.spinner.stop()
                if st['md'] is not None:
                    st['md'].finish()
                if e.retry and attempt < 3 and not res.blocks:
                    wait = e.wait or (2 << attempt)
                    ui.note('%s; trying again in %d s' % (e, wait))
                    try:
                        self.sleep(wait)
                    except KeyboardInterrupt:
                        _agent.flush_input()
                        ui.out(ui.yellow('Interrupted') + '\n')
                        return None
                    continue
                ui.error(str(e))
                return None
            except OSError as e:
                self.spinner.stop()
                if st['md'] is not None:
                    st['md'].finish()
                self.pool.drop(self.provider.tls, self.provider.host, self.provider.port)
                ui.error(net_error(e))
                return None
            self.spinner.stop()
            if st['md'] is not None:
                st['md'].finish()
            elif st['thinking']:
                ui.out('\n')
            break
        self.account(res)
        content = res.content()
        if content and not no_tools:
            self.messages.append({'role': 'assistant', 'content': content})
        return res

    def preview(self, b):
        # A tool's input as it streams: what it will act on, how much is
        # written (the input closed by _agent.json_repair and parsed)
        now = time.ticks_ms()
        if self.seen[0] == b[0]['id'] and time.ticks_diff(now, self.seen[1]) < 400:
            self.spinner.tick()
            return
        self.seen = (b[0]['id'], now)
        src = ''.join(b[1])
        name = b[0]['name']
        label = ''
        try:
            a = json.loads(_agent.json_repair(src))
            if isinstance(a, dict):
                label = self.tools.label(name, a) if name in self.tools.by_name else ''
        except ValueError:
            pass
        text = 'Writing ' + NAMES.get(name, name)
        if label:
            text += '(' + util.oneline(label, 40) + ')'
        if len(src) > 400:
            text += ' ' + util.size(len(src))
        self.spinner.text = text
        self.spinner.tick(True)

    def sleep(self, s):
        t0 = time.ticks_ms()
        while time.ticks_diff(time.ticks_ms(), t0) < s * 1000:
            time.sleep_ms(100)

    def account(self, res):
        self.requests += 1
        self.tok_in += res.input_tokens
        self.tok_out += res.output_tokens
        self.tok_cache_read += res.cache_read_input_tokens
        self.tok_cache_write += res.cache_creation_input_tokens
        self.last_input = res.total_input() + res.output_tokens
        p = llm.price(res.model or self.provider.model) if self.provider.name == 'anthropic' else None
        if p is None:
            self.priced = False
        else:
            self.cost += (res.input_tokens * p[0] + res.output_tokens * p[1] +
                          res.cache_creation_input_tokens * p[2] +
                          res.cache_read_input_tokens * p[3]) / 1e6

    # --- tools

    def run_tools(self, calls):
        results = []
        stop = False
        interrupted = False
        for blk, err in calls:
            name = blk['name']
            a = blk['input']
            label = NAMES.get(name, name)
            if stop:
                results.append(result(blk, 'Not run: an earlier call was refused or interrupted.',
                                      True))
                continue
            if err or name not in self.tools.by_name:
                msg = err or ('there is no tool named %s' % name)
                ui.tool_line(label, '')
                ui.tool_result(msg, True)
                results.append(result(blk, msg, True))
                continue
            ui.tool_line(label, self.tools.label(name, a))
            try:
                ok = self.permit(name, a)
            except KeyboardInterrupt:
                ok = False
            if not ok:
                if self.interactive:
                    results.append(result(blk, 'The user refused this call. Stop and ask the '
                                               'user what to do instead.', True))
                    stop = True
                else:
                    results.append(result(blk, 'This call was refused: it is not allowed '
                                               'without asking, and nobody can be asked.', True))
                continue
            self.spinner.start('Running')
            error = False
            try:
                text, summary = self.tools.run(name, a)
            except tools.ToolError as e:
                text = summary = str(e)
                error = True
            except KeyboardInterrupt:
                self.spinner.stop()
                _agent.flush_input()
                ui.tool_result('Interrupted', True)
                results.append(result(blk, 'Interrupted by the user.', True))
                stop = interrupted = True
                continue
            except OSError as e:
                text = summary = net_error(e)
                error = True
            except Exception as e:
                text = summary = 'internal error: %s: %s' % (type(e).__name__, e)
                error = True
            self.spinner.stop()
            if name == 'edit' and not error and not self.asked:
                ui.diff(a.get('old_string', ''), a.get('new_string', ''), 6)
            ui.tool_result(summary, error)
            results.append(result(blk, util.clip(text, tools.MAX_RESULT), error))
        if interrupted:
            results.append({'type': 'text', 'text': INTERRUPTED})
            ui.out(ui.yellow('Interrupted') + ui.dim(' - what should I do instead?') + '\n')
        self.add_user(results)
        return not stop

    asked = False

    def permit(self, name, a):
        self.asked = False
        if self.tools.allowed(name, a, self.allow):
            return True
        if not self.interactive:
            ui.tool_result('Refused: not allowed without asking (agent -p ... --yes allows it)',
                           True)
            return False
        self.asked = True
        if name == 'edit':
            ui.diff(a.get('old_string', ''), a.get('new_string', ''))
        elif name == 'write':
            c = a.get('content', '')
            lines = c.split('\n')
            for ln in lines[:8]:
                ui.out('    ' + ui.green(ln) + '\n')
            if len(lines) > 8:
                ui.note('    ... %d more lines' % (len(lines) - 8))
        rule = self.tools.rule(name, a)
        what = {'shell': "'%s' commands" % rule[6:], 'web_fetch': rule[10:] or 'web_fetch'} \
            .get(name, NAMES.get(name, name).lower() + 's')
        k = ui.ask_key('  ' + ui.bold('Allow?') + ' ' + ui.accent('[y]') + 'es  ' +
                       ui.accent('[n]') + 'o  ' + ui.accent('[a]') + 'lways for ' + what + ' ',
                       'yna')
        if k == 'a':
            self.allow.append(rule)
        return k in 'ya'

    # --- compaction

    def maybe_compact(self):
        if len(self.messages) < 2:
            return
        gc.collect()
        used = gc.mem_alloc()
        heap_low = used > (used + gc.mem_free()) * 6 // 10
        if heap_low or self.last_input > self.cfg.get('compact_tokens', 100000) or \
                self.provider.last_body > self.cfg.get('max_history_kb', 384) * 1024:
            ui.note('(The conversation is long: compacting it.)')
            self.compact()

    def compact(self, focus=''):
        if not self.messages:
            ui.note('(Nothing to compact.)')
            return False
        saved = self.messages
        before = self.provider.last_body
        text = COMPACT + ((' Focus on: ' + focus) if focus else '')
        msgs = list(saved)
        if msgs[-1]['role'] == 'user':
            msgs[-1] = {'role': 'user', 'content': msgs[-1]['content'] + [{'type': 'text', 'text': text}]}
        else:
            msgs.append({'role': 'user', 'content': [{'type': 'text', 'text': text}]})
        self.messages = msgs
        self.spinner.start('Compacting')
        try:
            res = self.request(no_tools=True, max_tokens=8192, show=False)
        except KeyboardInterrupt:
            self.spinner.stop()
            _agent.flush_input()
            self.messages = saved
            ui.out(ui.yellow('Interrupted') + '\n')
            return False
        self.spinner.stop()
        summary = res.text().strip() if res is not None else ''
        if not summary:
            self.messages = saved
            ui.error('compaction failed; the conversation is kept as it was')
            return False
        self.messages = [{'role': 'user', 'content': [{'type': 'text', 'text':
                          'This session continues an earlier conversation, which was '
                          'compacted to this summary:\n\n' + summary}]}]
        self.last_input = 0
        self.provider.last_body = len(summary)
        self.tools.read_files = {}
        gc.collect()
        ui.note('(Compacted: %s of conversation to a %s summary.)' %
                (util.size(before) if before else 'the', util.size(len(summary))))
        return True

    # --- figures

    def cost_text(self):
        rows = ['Requests: %d' % self.requests,
                'Tokens: %d in, %d out; cache: %d read, %d written' %
                (self.tok_in, self.tok_out, self.tok_cache_read, self.tok_cache_write)]
        if self.requests:
            if self.priced:
                rows.append('Cost: about $%.4f' % self.cost)
            else:
                rows.append('Cost: unknown for this provider or model')
        rows.append('Context: %d tokens in the last request (compacts past %d); request %s' %
                    (self.last_input, self.cfg.get('compact_tokens', 100000),
                     util.size(self.provider.last_body)))
        gc.collect()
        rows.append('Memory: %s used of the heap, %s free' %
                    (util.size(gc.mem_alloc()), util.size(gc.mem_free())))
        return '\n'.join(rows)


def net_error(e):
    msg = e.args[-1] if e.args and isinstance(e.args[-1], str) else str(e)
    return msg
