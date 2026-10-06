# agent: the command (agent_main.c starts MicroPython with run()).
#
#   agent                       the conversation in the terminal
#   agent -p PROMPT [--yes]     one request, the answer printed, then exit
#   agent --tool NAME [JSON | KEY=VALUE...]  run one tool, print its result
#   agent --setup               the first-run questions again
#   options: --provider NAME, --model NAME, --yes (no questions: tools that
#   change things run), --version, -h

import json
import sys

import _agent

from . import VERSION
from . import config
from . import session
from . import ui
from . import util

HELP = """Commands:
  /help              this
  /model [NAME]      the model (a number picks from the list)
  /provider [NAME]   anthropic, or openai (any OpenAI-compatible server)
  /clear             a new conversation
  /compact [FOCUS]   replace the conversation with a summary of it
  /cost              tokens, cost, context and memory used
  /tools             the tools
  /todo              the task list
  /auto              tools that change things run without asking (toggle)
  /config            the configuration (~/.agent.json)
  /setup             the first-run questions again
  /exit              leave (also Ctrl-D, or Ctrl-C twice)
Keys: Enter sends; a line ending in \\ or Alt-Enter goes on to a new line;
Up/Down the history; Tab completes a command; Esc or Ctrl-C interrupts
the agent while it works."""

COMMANDS = ['/help', '/model', '/provider', '/clear', '/compact', '/cost', '/tools',
            '/todo', '/auto', '/config', '/setup', '/exit', '/quit']


def usage():
    print('usage: agent [-p PROMPT] [--yes] [--provider NAME] [--model NAME]\n'
          '             [--tool NAME [JSON | KEY=VALUE...]] [--setup] [--version]')


# ---------------------------------------------------------------------------
# The first-run questions

def setup(cfg):
    ui.line(ui.bold('Setting up the agent') + ui.dim(' (saved in %s)' % config.path()))
    ui.line('Which API?  1) Anthropic (Claude)   2) OpenAI-compatible (OpenAI, OpenRouter,')
    ui.line('            Ollama, llama.cpp, LM Studio ...)')
    k = ui.ask_line('[1] ', '1')
    name = 'openai' if k.startswith('2') or k.startswith('o') else 'anthropic'
    cfg['provider'] = name
    s = cfg[name]
    if name == 'anthropic':
        models = config.ANTHROPIC_MODELS
        for i, m in enumerate(models):
            ui.line('  %d) %s%s' % (i + 1, m, ' (default)' if i == 0 else ''))
        k = ui.ask_line('Model [1]: ', '1')
        s['model'] = pick(k, models) or s['model']
    else:
        s['base_url'] = ui.ask_line('Base URL [%s]: ' % s['base_url'], s['base_url']).rstrip('/')
        while True:
            s['model'] = ui.ask_line('Model (e.g. gpt-5, llama3.2)%s: ' %
                                     (' [%s]' % s['model'] if s['model'] else ''), s['model'])
            if s['model']:
                break
    env = s.get('api_key_env', '')
    key = ui.ask_line('API key (input hidden; empty: from $%s%s): ' %
                      (env, ', or none' if name == 'openai' else ''), '', mask=True)
    if key:
        s['api_key'] = key
    config.save(cfg)
    ui.line(ui.dim('Saved. /setup asks again; /config shows the file.'))


def pick(k, items):
    k = k.strip()
    if not k:
        return None
    if k.isdigit():
        i = int(k) - 1
        return items[i] if 0 <= i < len(items) else None
    return k


# ---------------------------------------------------------------------------
# History of what was typed

def history_path():
    return util.join(util.home(), '.agent_history')


def load_history():
    h = []
    try:
        with open(history_path()) as f:
            for ln in f:
                try:
                    h.append(json.loads(ln))
                except ValueError:
                    pass
    except OSError:
        pass
    return h[-100:]


def save_history(h, text):
    if h and h[-1] == text:
        return
    h.append(text)
    del h[:-100]
    try:
        with open(history_path(), 'a') as f:
            f.write(json.dumps(text) + '\n')
    except OSError:
        pass


# ---------------------------------------------------------------------------

def banner(s, cfg):
    p = s.provider
    ui.line(ui.accent(ui.MARK) + ' ' + ui.bold('fpc agent ' + VERSION) + '  ' +
            (p.model or '(no model)') + ui.dim(' (%s)' % p.name))
    import os
    ui.line(ui.dim('  %s - /help for commands - Esc or Ctrl-C interrupts' % os.getcwd()))
    state = _agent.netstate()
    if state != 'ready':
        ui.line(ui.yellow('  No network: ') + ui.dim(
            'no network coprocessor answers (the local tools still work).'
            if state in ('none', 'nodaemon') else 'the network coprocessor is starting.'))
    elif not config.api_key(cfg) and p.name == 'anthropic':
        ui.line(ui.yellow('  No API key: ') + ui.dim('/setup, or set ANTHROPIC_API_KEY.'))
    if cfg.get('auto_approve'):
        ui.line(ui.yellow('  Auto-approve is on: ') + ui.dim('tools run without asking.'))
    ui.line()


def config_text(cfg):
    lines = ['%s:' % config.path()]
    for k in config.DEFAULTS:
        if k in ('anthropic', 'openai'):
            continue
        lines.append('  %s: %s' % (k, json.dumps(cfg.get(k))))
    for name in ('anthropic', 'openai'):
        s = cfg[name]
        mark = ' (in use)' if cfg['provider'] == name else ''
        lines.append('  %s%s:' % (name, mark))
        for k in s:
            if k == 'api_key':
                continue
            lines.append('    %s: %s' % (k, json.dumps(s[k])))
        lines.append('    api key: %s' % config.key_source(cfg, name))
    return '\n'.join(lines)


def command(s, cfg, text):
    # A slash command; False to leave
    words = text.split(None, 1)
    cmd = words[0].lower()
    arg = words[1].strip() if len(words) > 1 else ''
    if cmd in ('/exit', '/quit'):
        return False
    if cmd == '/help':
        ui.line(HELP)
    elif cmd == '/clear':
        s.clear()
        ui.note('(A new conversation.)')
    elif cmd == '/compact':
        s.compact(arg)
    elif cmd == '/cost':
        ui.line(s.cost_text())
    elif cmd == '/tools':
        for d in s.tools.defs:
            kind = {'read': '', 'write': ' (asks)', 'exec': ' (asks)', 'net': ' (asks)'}[d['kind']]
            ui.line(ui.bold(session.NAMES.get(d['name'], d['name'])) + ui.dim(
                ' %s%s: ' % (d['name'], kind)) + d['description'].split('. ')[0])
    elif cmd == '/todo':
        ui.line(s.tools.todo_text() or '(No tasks.)')
    elif cmd == '/auto':
        cfg['auto_approve'] = not cfg.get('auto_approve')
        ui.note('(Auto-approve %s for this session.)' % ('on' if cfg['auto_approve'] else 'off'))
    elif cmd == '/config':
        ui.line(config_text(cfg))
    elif cmd == '/setup':
        setup(cfg)
        s.set_provider()
    elif cmd == '/provider':
        if not arg:
            ui.line('Provider: %s (anthropic, openai)' % cfg['provider'])
        elif arg not in ('anthropic', 'openai'):
            ui.error('anthropic or openai')
        else:
            cfg['provider'] = arg
            s.set_provider()
            save_setting(cfg)
            ui.note('(Provider %s, model %s.)' % (arg, s.provider.model or 'not set: /model NAME'))
    elif cmd == '/model':
        sec = config.section(cfg)
        models = config.ANTHROPIC_MODELS if cfg['provider'] == 'anthropic' else []
        if not arg:
            ui.line('Model: %s' % (sec.get('model') or '(not set)'))
            for i, m in enumerate(models):
                ui.line('  %d) %s' % (i + 1, m))
        else:
            m = pick(arg, models)
            if not m:
                ui.error('no such choice')
            else:
                sec['model'] = m
                s.set_provider()
                save_setting(cfg)
                ui.note('(Model %s.)' % m)
    else:
        ui.error('unknown command %s (/help)' % cmd)
    return True


def save_setting(cfg):
    # The provider and model changed: into the file (the rest as it is)
    try:
        cur = config.load() if config.exists() else config.merge(config.DEFAULTS, {})
        cur['provider'] = cfg['provider']
        cur[cfg['provider']]['model'] = cfg[cfg['provider']]['model']
        config.save(cur)
    except OSError as e:
        ui.error('could not save %s: %s' % (config.path(), e))


def repl(s, cfg):
    banner(s, cfg)
    hist = load_history()
    interrupts = 0
    while True:
        try:
            if ui.TTY:
                text = _agent.readline(ui.accent('> ') if ui.COLOR else '> ', hist,
                                       cont='  ', completions=COMMANDS)
            else:
                ui.out('> ')
                text = sys.stdin.readline()
                if not text:
                    raise EOFError
                ui.out(text)
        except KeyboardInterrupt:
            interrupts += 1
            if interrupts >= 2:
                break
            ui.note('(Ctrl-C again, Ctrl-D or /exit to leave.)')
            continue
        except EOFError:
            break
        interrupts = 0
        text = text.strip()
        if not text:
            continue
        save_history(hist, text)
        if text.startswith('/'):
            try:
                if not command(s, cfg, text):
                    break
            except KeyboardInterrupt:
                _agent.flush_input()
                ui.out(ui.yellow('Interrupted') + '\n')
            continue
        s.ask(text)
        ui.line()
    s.pool.close()


def tool_args(words):
    # JSON, or KEY=VALUE words (NSH's quoting keeps no double quotes)
    if len(words) == 1 and words[0].lstrip().startswith('{'):
        return json.loads(words[0])
    a = {}
    for w in words:
        k, eq, v = w.partition('=')
        if not eq:
            raise ValueError('%s: KEY=VALUE or JSON' % w)
        if v in ('true', 'false'):
            v = v == 'true'
        elif v.isdigit():
            v = int(v)
        a[k] = v
    return a


def run_tool(s, name, words):
    try:
        a = tool_args(words)
    except ValueError as e:
        print('agent: %s' % e)
        return 2
    if name not in s.tools.by_name:
        print('agent: no tool %s (%s)' % (name, ', '.join(d['name'] for d in s.tools.defs)))
        return 2
    try:
        text, summary = s.tools.run(name, a)
    except Exception as e:
        print('error: %s' % (e.args[-1] if isinstance(e, OSError) and e.args else e))
        return 1
    print(text)
    return 0


def run():
    args = sys.argv[1:]
    prompt = None
    tool = None
    tool_arg = None
    want_setup = False
    over = {}
    yes = False
    i = 0
    while i < len(args):
        a = args[i]
        nxt = args[i + 1] if i + 1 < len(args) else None
        if a in ('-h', '--help'):
            usage()
            return
        if a == '--version':
            print('agent ' + VERSION)
            return
        if a in ('-p', '--print') and nxt is not None:
            prompt = nxt
            i += 2
            continue
        if a == '--tool' and nxt is not None:
            tool = nxt
            tool_arg = args[i + 2:]
            break
        if a in ('--provider', '--model') and nxt is not None:
            over[a[2:]] = nxt
            i += 2
            continue
        if a in ('-y', '--yes'):
            yes = True
            i += 1
            continue
        if a == '--setup':
            want_setup = True
            i += 1
            continue
        if prompt is None and not a.startswith('-'):
            prompt = ' '.join(args[i:])
            break
        usage()
        sys.exit(2)

    cfg = config.load()
    ui.setup(cfg.get('color', True))
    if 'provider' in over:
        cfg['provider'] = over['provider']
    if 'model' in over:
        cfg[cfg['provider']]['model'] = over['model']
    if yes:
        cfg['auto_approve'] = True

    interactive = prompt is None and tool is None
    if want_setup or (interactive and ui.TTY and not config.exists() and not config.api_key(cfg)):
        try:
            setup(cfg)
        except (KeyboardInterrupt, EOFError):
            ui.line()
            return

    try:
        s = session.Session(cfg, interactive)
    except ValueError as e:
        print('agent: %s' % e)
        sys.exit(2)

    if tool is not None:
        sys.exit(run_tool(s, tool, tool_arg))

    if prompt is not None:
        ok = s.ask(prompt)
        s.pool.close()
        if not ok:
            sys.exit(1)
        return

    repl(s, cfg)
