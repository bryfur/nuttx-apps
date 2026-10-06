# The model's APIs: Anthropic's Messages API and OpenAI's Chat
# Completions (and the many servers that speak it: OpenRouter, Ollama,
# llama.cpp, LM Studio ...), both streamed.  The conversation is kept in
# the Messages API's form (content blocks: text, tool_use, tool_result,
# thinking); the OpenAI adapter translates.
#
# HTTP is _agent.Http (C): one connection kept alive per server, so the
# coprocessor's TLS handshake is paid once.

import json
import time

import _agent

from . import util


class ApiError(Exception):
    def __init__(self, msg, status=0, retry=False, wait=0):
        super().__init__(msg)
        self.status = status
        self.retry = retry
        self.wait = wait


def parse_url(url):
    # (tls, host, port, path prefix)
    tls = url.startswith('https://')
    if tls:
        rest = url[8:]
    elif url.startswith('http://'):
        rest = url[7:]
    else:
        raise ValueError('not an http(s) URL: ' + url)
    i = rest.find('/')
    hostport, path = (rest, '') if i < 0 else (rest[:i], rest[i:])
    port = 443 if tls else 80
    if hostport.startswith('['):
        j = hostport.find(']')
        host = hostport[1:j]
        if hostport[j + 1:j + 2] == ':':
            port = int(hostport[j + 2:])
    elif ':' in hostport:
        host, p = hostport.rsplit(':', 1)
        port = int(p)
    else:
        host = hostport
    return tls, host, port, path.rstrip('/')


# ---------------------------------------------------------------------------
# Connections, kept

class Pool:
    def __init__(self, timeout=300, verify=True):
        self.conns = {}
        self.timeout = timeout
        self.verify = verify

    def get(self, tls, host, port):
        key = (tls, host, port)
        c = self.conns.get(key)
        if c is not None and c.reusable:
            return c, True
        if c is not None:
            c.close()
        c = _agent.Http(host, port, tls, self.verify, self.timeout)
        self.conns[key] = c
        return c, False

    def drop(self, tls, host, port):
        key = (tls, host, port)
        c = self.conns.pop(key, None)
        if c is not None:
            c.close()

    def close(self):
        for c in self.conns.values():
            c.close()
        self.conns = {}

    def request(self, tls, host, port, method, path, headers, body):
        # The response's connection, its status read.  A kept connection
        # the server has closed is tried again on a new one.
        for attempt in range(2):
            c, reused = self.get(tls, host, port)
            try:
                status = c.request(method, path, headers, body)
                return c, status
            except OSError as e:
                self.drop(tls, host, port)
                if not reused or attempt:
                    raise
            except KeyboardInterrupt:
                self.drop(tls, host, port)
                raise


def http_error(c, status):
    # The body of an error answer: its message
    data = []
    n = 0
    while n < 16384:
        b = c.read(4096)
        if not b:
            break
        data.append(b)
        n += len(b)
    text = util.to_str(b''.join(data))
    msg = text.strip()
    try:
        j = json.loads(text)
        e = j.get('error', j) if isinstance(j, dict) else j
        if isinstance(e, dict):
            msg = e.get('message') or msg
            if e.get('type'):
                msg = '%s: %s' % (e['type'], msg)
        elif isinstance(e, str):
            msg = e
    except Exception:
        pass
    wait = 0
    ra = c.header('retry-after')
    if ra:
        try:
            wait = int(float(ra))
        except ValueError:
            pass
    retry = status in (408, 409, 425, 429, 500, 502, 503, 504, 529)
    return ApiError('HTTP %d: %s' % (status, util.oneline(msg, 300)), status, retry, wait)


def dumps(o):
    try:
        return json.dumps(o, separators=(',', ':'))
    except TypeError:
        return json.dumps(o)


# ---------------------------------------------------------------------------
# The common part

class Provider:
    name = ''

    def __init__(self, cfg, section, key, pool):
        self.cfg = cfg
        self.s = section
        self.key = key
        self.pool = pool
        self.model = section.get('model') or ''
        self.tls, self.host, self.port, self.prefix = parse_url(section['base_url'])
        self.last_body = 0

    def post(self, path, headers, body, ev):
        data = dumps(body)
        self.last_body = len(data)
        ev('send', len(data))
        c, status = self.pool.request(self.tls, self.host, self.port, 'POST',
                                      self.prefix + path, headers, data)
        data = None
        if status != 200:
            err = http_error(c, status)
            self.pool.drop(self.tls, self.host, self.port)
            raise err
        return c

    def events(self, c, ev):
        # The response's server-sent events; ev('wait') while nothing comes
        while True:
            e = c.sse(150)
            if e is False:
                ev('wait', None)
                continue
            if e is None:
                return
            yield e


def make(cfg, pool, name=None):
    from . import config
    name = name or cfg['provider']
    s = cfg[name]
    key = config.api_key(cfg, name)
    if name == 'anthropic':
        return Anthropic(cfg, s, key, pool)
    if name == 'openai':
        return OpenAI(cfg, s, key, pool)
    raise ValueError('unknown provider: ' + name)


# ---------------------------------------------------------------------------
# Anthropic: the Messages API

PRICES = [
    # (model id prefix, $ per million: input, output, cache write, cache read)
    ('claude-opus-5-5', 4.0, 20.0, 5.0, 0.20),
    ('claude-sonnet-5-5', 2.0, 10.0, 2.5, 0.20),
    ('claude-haiku-4-5', 1.0, 5.0, 1.25, 0.10),
    ('claude-opus-5', 5.0, 25.0, 6.25, 0.50),
    ('claude-sonnet-5', 2.0, 10.0, 2.5, 0.20),
    ('claude-sonnet-4-6', 3.0, 15.0, 3.75, 0.30),
    ('claude-opus-4', 5.0, 25.0, 6.25, 0.50),
]


def price(model):
    for p in PRICES:
        if model.startswith(p[0]):
            return p[1:]
    return None


class Anthropic(Provider):
    name = 'anthropic'

    def context_window(self):
        return 200000 if 'haiku' in self.model else 1000000

    def headers(self):
        h = 'Content-Type: application/json\r\nAccept: text/event-stream\r\n'
        h += 'anthropic-version: 2023-06-01\r\nUser-Agent: fpc-agent/0.1\r\n'
        if self.key:
            h += 'x-api-key: ' + self.key + '\r\n'
        return h

    def tools(self, tools):
        out = []
        eager = self.s.get('eager_tools') and 'anthropic.com' in self.host
        for t in tools:
            d = {'name': t['name'], 'description': t['description'],
                 'input_schema': t['schema']}
            if eager:
                d['eager_input_streaming'] = True
            out.append(d)
        return out

    def stream(self, system, tools, messages, ev, res, max_tokens=0, no_tools=False):
        s = self.s
        body = {'model': self.model, 'max_tokens': max_tokens or s.get('max_tokens', 16000),
                'stream': True}
        sysblock = {'type': 'text', 'text': system}
        if s.get('cache', True):
            sysblock['cache_control'] = {'type': 'ephemeral'}
            body['cache_control'] = {'type': 'ephemeral'}
        body['system'] = [sysblock]
        if tools:
            body['tools'] = self.tools(tools)
            if no_tools:
                body['tool_choice'] = {'type': 'none'}
        body['messages'] = messages
        if s.get('effort'):
            body['output_config'] = {'effort': s['effort']}
        if s.get('thinking') == 'summarized' and 'haiku' not in self.model:
            body['thinking'] = {'type': 'adaptive', 'display': 'summarized'}

        c = self.post('/v1/messages', self.headers(), body, ev)
        body = None
        blocks = {}                  # index: [block, text parts]
        for name, data in self.events(c, ev):
            if name == 'ping':
                continue
            try:
                d = json.loads(data)
            except ValueError:
                continue
            t = d.get('type', name)
            if t == 'content_block_delta':
                b = blocks.get(d['index'])
                if b is None:
                    continue
                delta = d['delta']
                dt = delta.get('type')
                if dt == 'text_delta':
                    b[1].append(delta['text'])
                    ev('text', delta['text'])
                elif dt == 'input_json_delta':
                    b[1].append(delta['partial_json'])
                    ev('tool_delta', b)
                elif dt == 'thinking_delta':
                    b[1].append(delta['thinking'])
                    ev('thinking', delta['thinking'])
                elif dt == 'signature_delta':
                    b[0]['signature'] = b[0].get('signature', '') + delta['signature']
            elif t == 'content_block_start':
                blk = d['content_block']
                bt = blk.get('type')
                if bt == 'tool_use':
                    blk = {'type': 'tool_use', 'id': blk['id'], 'name': blk['name'], 'input': {}}
                    ev('tool_start', blk)
                elif bt == 'text':
                    blk = {'type': 'text', 'text': ''}
                    ev('text_start', None)
                elif bt == 'thinking':
                    blk = {'type': 'thinking', 'thinking': '', 'signature': blk.get('signature', '')}
                    ev('thinking_start', None)
                blocks[d['index']] = [blk, []]
                res.blocks.append(blocks[d['index']])
            elif t == 'content_block_stop':
                b = blocks.get(d['index'])
                if b is not None:
                    finish_block(b)
                    if b[0]['type'] == 'tool_use':
                        ev('tool_ready', b[0])
            elif t == 'message_start':
                m = d.get('message', {})
                res.usage_in(m.get('usage') or {})
                res.model = m.get('model', self.model)
            elif t == 'message_delta':
                res.stop = d.get('delta', {}).get('stop_reason') or res.stop
                u = d.get('usage') or {}
                if 'output_tokens' in u:
                    res.output_tokens = u['output_tokens']
                for k in ('input_tokens', 'cache_read_input_tokens', 'cache_creation_input_tokens'):
                    if u.get(k):
                        setattr(res, k, u[k])
            elif t == 'message_stop':
                res.done = True
            elif t == 'error':
                e = d.get('error', {})
                et = e.get('type', '')
                raise ApiError('%s: %s' % (et or 'error', e.get('message', data)), 0,
                               et in ('overloaded_error', 'api_error', 'rate_limit_error'))
        if not res.done:
            self.pool.drop(self.tls, self.host, self.port)
            raise ApiError('the response ended early', 0, True)
        return res


def finish_block(b):
    blk, parts = b
    t = blk['type']
    if t == 'text':
        blk['text'] = ''.join(parts)
    elif t == 'thinking':
        blk['thinking'] = ''.join(parts)
    elif t == 'tool_use':
        src = ''.join(parts).strip()
        try:
            v = json.loads(src) if src else {}
            if not isinstance(v, dict):
                raise ValueError('not an object')
            blk['input'] = v
        except ValueError:
            blk['input'] = {}
            b.append('the tool input was not valid JSON: ' + util.oneline(src, 120))
    b[1] = []
    if len(b) < 3:
        b.append(None)


class Result:
    def __init__(self):
        self.blocks = []          # [block, parts, error]
        self.stop = None
        self.done = False
        self.model = ''
        self.input_tokens = 0
        self.output_tokens = 0
        self.cache_read_input_tokens = 0
        self.cache_creation_input_tokens = 0

    def usage_in(self, u):
        self.input_tokens = u.get('input_tokens', 0) or 0
        self.output_tokens = u.get('output_tokens', 0) or 0
        self.cache_read_input_tokens = u.get('cache_read_input_tokens', 0) or 0
        self.cache_creation_input_tokens = u.get('cache_creation_input_tokens', 0) or 0

    def content(self, partial=False):
        # The assistant message's blocks.  partial (interrupted): what is
        # whole of them; thinking without its signature and tool calls
        # without their input are left out
        out = []
        for b in self.blocks:
            blk = b[0]
            if len(b) < 3:
                if not partial:
                    finish_block(b)
                elif blk['type'] == 'text':
                    blk['text'] = ''.join(b[1])
                    b.append(None)
                else:
                    continue
            if blk['type'] == 'text' and not blk['text']:
                continue
            if partial and blk['type'] in ('thinking', 'redacted_thinking', 'tool_use'):
                continue
            out.append(blk)
        return out

    def tool_calls(self):
        return [(b[0], b[2] if len(b) > 2 else None) for b in self.blocks
                if b[0]['type'] == 'tool_use']

    def text(self):
        return ''.join(b[0].get('text', '') for b in self.blocks if b[0]['type'] == 'text')

    def total_input(self):
        return self.input_tokens + self.cache_read_input_tokens + self.cache_creation_input_tokens


# ---------------------------------------------------------------------------
# OpenAI: Chat Completions

class OpenAI(Provider):
    name = 'openai'

    def context_window(self):
        return 128000

    def headers(self):
        h = 'Content-Type: application/json\r\nAccept: text/event-stream\r\n'
        h += 'User-Agent: fpc-agent/0.1\r\n'
        if self.key:
            h += 'Authorization: Bearer ' + self.key + '\r\n'
        return h

    def convert(self, system, messages):
        out = [{'role': 'system', 'content': system}]
        for m in messages:
            content = m['content']
            if isinstance(content, str):
                out.append({'role': m['role'], 'content': content})
                continue
            if m['role'] == 'assistant':
                text = []
                calls = []
                for b in content:
                    if b['type'] == 'text':
                        text.append(b['text'])
                    elif b['type'] == 'tool_use':
                        calls.append({'id': b['id'], 'type': 'function',
                                      'function': {'name': b['name'],
                                                   'arguments': dumps(b['input'])}})
                msg = {'role': 'assistant', 'content': ''.join(text) or None}
                if calls:
                    msg['tool_calls'] = calls
                out.append(msg)
            else:
                text = []
                for b in content:
                    if b['type'] == 'tool_result':
                        c = b.get('content', '')
                        if isinstance(c, list):
                            c = ''.join(x.get('text', '') for x in c)
                        if b.get('is_error'):
                            c = 'Error: ' + c
                        out.append({'role': 'tool', 'tool_call_id': b['tool_use_id'], 'content': c})
                    elif b['type'] == 'text':
                        text.append(b['text'])
                if text:
                    out.append({'role': 'user', 'content': '\n\n'.join(text)})
        return out

    def stream(self, system, tools, messages, ev, res, max_tokens=0, no_tools=False):
        s = self.s
        if not self.model:
            raise ApiError('no model set for the openai provider (/model NAME)')
        body = {'model': self.model, 'stream': True,
                'messages': self.convert(system, messages)}
        param = s.get('max_tokens_param') or \
            ('max_completion_tokens' if 'openai.com' in self.host else 'max_tokens')
        body[param] = max_tokens or s.get('max_tokens', 8192)
        if s.get('stream_usage', True):
            body['stream_options'] = {'include_usage': True}
        if tools:
            body['tools'] = [{'type': 'function',
                              'function': {'name': t['name'], 'description': t['description'],
                                           'parameters': t['schema']}} for t in tools]
            if no_tools:
                body['tool_choice'] = 'none'

        c = self.post('/chat/completions', self.headers(), body, ev)
        body = None
        text = None                   # [block, parts]
        calls = {}                    # index: [block, parts]
        for name, data in self.events(c, ev):
            if data.strip() == '[DONE]':
                res.done = True
                break
            try:
                d = json.loads(data)
            except ValueError:
                continue
            if d.get('error'):
                e = d['error']
                raise ApiError(e.get('message', data) if isinstance(e, dict) else str(e))
            u = d.get('usage')
            if u:
                details = u.get('prompt_tokens_details') or {}
                cached = details.get('cached_tokens', 0) or 0
                res.input_tokens = (u.get('prompt_tokens', 0) or 0) - cached
                res.cache_read_input_tokens = cached
                res.output_tokens = u.get('completion_tokens', 0) or 0
            res.model = d.get('model', res.model)
            for ch in d.get('choices') or ():
                delta = ch.get('delta') or {}
                r = delta.get('reasoning_content') or delta.get('reasoning')
                if r:
                    ev('thinking', r)
                t = delta.get('content')
                if t:
                    if text is None:
                        text = [{'type': 'text', 'text': ''}, []]
                        res.blocks.append(text)
                        ev('text_start', None)
                    text[1].append(t)
                    ev('text', t)
                for tc in delta.get('tool_calls') or ():
                    i = tc.get('index', 0)
                    b = calls.get(i)
                    f = tc.get('function') or {}
                    if b is None:
                        blk = {'type': 'tool_use', 'id': tc.get('id') or ('call_%d' % i),
                               'name': f.get('name', ''), 'input': {}}
                        b = calls[i] = [blk, []]
                        res.blocks.append(b)
                        ev('tool_start', blk)
                    elif f.get('name'):
                        b[0]['name'] += f['name']
                    if f.get('arguments'):
                        b[1].append(f['arguments'])
                        ev('tool_delta', b)
                fr = ch.get('finish_reason')
                if fr:
                    res.stop = {'tool_calls': 'tool_use', 'function_call': 'tool_use',
                                'length': 'max_tokens', 'stop': 'end_turn'}.get(fr, fr)
        if not res.done and res.stop is None:
            self.pool.drop(self.tls, self.host, self.port)
            raise ApiError('the response ended early', 0, True)
        res.done = True
        for b in res.blocks:
            if len(b) < 3:
                finish_block(b)
                if b[0]['type'] == 'tool_use':
                    ev('tool_ready', b[0])
        if calls and res.stop not in ('max_tokens',):
            res.stop = 'tool_use'
        return res
