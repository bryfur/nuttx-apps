# The agent's configuration: ~/.agent.json (docs/agent.md)
#
# A provider section each (anthropic, openai), the one in use named by
# "provider".  An API key comes from the file, else from the environment
# (ANTHROPIC_API_KEY, OPENAI_API_KEY, or the variable api_key_env names).
# Keys are never printed.

import json
import os

from . import util

ANTHROPIC_MODELS = ['claude-sonnet-5-5', 'claude-opus-5-5',
                    'claude-haiku-4-5-20251001']

DEFAULTS = {
    'provider': 'anthropic',
    'anthropic': {
        'model': 'claude-sonnet-5-5',
        'base_url': 'https://api.anthropic.com',
        'api_key': '',
        'api_key_env': 'ANTHROPIC_API_KEY',
        'max_tokens': 16000,
        'effort': '',             # low, medium, high, xhigh, max ('': the model's)
        'thinking': '',           # 'summarized': show the model's reasoning
        'cache': True,            # prompt caching (cache_control)
        'eager_tools': True,      # tool input streamed as it is written
    },
    'openai': {
        'model': '',
        'base_url': 'https://api.openai.com/v1',
        'api_key': '',
        'api_key_env': 'OPENAI_API_KEY',
        'max_tokens': 8192,
        'max_tokens_param': '',   # '' : max_completion_tokens at OpenAI, else max_tokens
        'stream_usage': True,     # stream_options.include_usage
    },
    'auto_approve': False,        # tools that change things run without asking
    'allow': [],                  # always allowed: "edit", "write", "shell:ls", "web_fetch:host"
    'compact_tokens': 100000,     # compact the conversation past this many input tokens
    'max_history_kb': 384,        # ... or when the request is this big
    'timeout': 300,               # seconds without a byte from the server
    'shell_timeout': 120,         # seconds for an NSH command
    'verify_tls': True,
    'color': True,
}


# The file's keys in this order (MicroPython's dicts keep none)
ORDER = ['provider', 'anthropic', 'openai', 'model', 'base_url', 'api_key', 'api_key_env',
         'max_tokens', 'max_tokens_param', 'effort', 'thinking', 'cache', 'eager_tools',
         'stream_usage', 'auto_approve', 'allow', 'compact_tokens', 'max_history_kb',
         'timeout', 'shell_timeout', 'verify_tls', 'color']


def path():
    return util.join(util.home(), '.agent.json')


def merge(base, over):
    out = {}
    for k in base:
        v = base[k]
        if k in over:
            o = over[k]
            if isinstance(v, dict) and isinstance(o, dict):
                v = merge(v, o)
            else:
                v = o
        out[k] = v
    for k in over:
        if k not in base:
            out[k] = over[k]
    return out


def exists():
    return util.exists(path())


def load():
    cfg = DEFAULTS
    try:
        with open(path()) as f:
            cfg = merge(DEFAULTS, json.loads(f.read()))
    except OSError:
        pass
    except ValueError as e:
        print('agent: %s: bad JSON (%s); using the defaults' % (path(), e))
    return cfg


def save(cfg):
    with open(path(), 'w') as f:
        f.write(pretty(cfg) + '\n')


def pretty(v, ind=''):
    # JSON with indentation (MicroPython's json has none), keys in order
    if isinstance(v, dict):
        if not v:
            return '{}'
        keys = list(v.keys())
        seen = []
        for k in ORDER + sorted(keys):
            if k in v and k not in seen:
                seen.append(k)
        inner = ind + '  '
        return '{\n' + ',\n'.join(inner + json.dumps(k) + ': ' +
                                  pretty(v[k], inner) for k in seen) + \
            '\n' + ind + '}'
    return json.dumps(v)


def section(cfg, name=None):
    return cfg[name or cfg['provider']]


def api_key(cfg, name=None):
    s = section(cfg, name)
    if s.get('api_key'):
        return s['api_key']
    env = s.get('api_key_env')
    if env:
        return os.getenv(env) or ''
    return ''


def key_source(cfg, name=None):
    # Where the key comes from, for display: never the key
    s = section(cfg, name)
    if s.get('api_key'):
        return 'set in ' + path()
    env = s.get('api_key_env')
    if env and os.getenv(env):
        return 'from $' + env
    return 'not set'
