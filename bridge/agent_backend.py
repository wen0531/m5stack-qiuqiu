"""Pluggable agent backends for the StopWatch relay.

Supported: HTTP (Hermes gateway, OpenAI-compatible) and CLI agents
(codex / claude / dsh / agy). The active backend lives in agents.json and can
be switched at runtime without restarting the bridge.
"""
import json, os, re, subprocess, urllib.request
from pathlib import Path

HERE = Path(__file__).parent
CONFIG = HERE / 'agents.json'
VOICE_SYSTEM = ('你是手表上的语音助手。回答必须口语化、简短（一到两句话），'
                '不要使用 Markdown、列表、代码块或表情符号。')

_ansi = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')

# Rolling conversation context so the watch keeps one continuous conversation
# instead of starting a fresh session for every utterance.
HISTORY = []
HISTORY_MAX_TURNS = 6
HISTORY_IDLE_RESET_S = 30 * 60
_last_turn_at = 0.0


def history_enabled():
    return os.environ.get('VOICE_HISTORY', '1') != '0'


def reset_history(reason=''):
    global HISTORY, _last_turn_at
    if HISTORY and reason:
        print(f'[agent] history cleared ({reason})', flush=True)
    HISTORY = []
    _last_turn_at = 0.0


def _config():
    return json.loads(CONFIG.read_text(encoding='utf-8'))


def current_agent():
    cfg = _config()
    return cfg.get('default') or next(iter(cfg.get('agents', {})), '')


def set_agent(name):
    cfg = _config()
    if name not in cfg.get('agents', {}):
        raise KeyError(name)
    cfg['default'] = name
    CONFIG.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding='utf-8')
    try:
        reset_history(f'switched to {name}')
    except NameError:
        pass
    return name


def _clean(text: str) -> str:
    text = _ansi.sub('', text)
    # drop common CLI chatter lines
    lines = []
    for line in text.splitlines():
        s = line.strip()
        if not s:
            continue
        if s.startswith(('>', '•', 'ℹ', '⚠', 'ERROR', 'WARN')):
            continue
        if 'MCP server started' in s or 'tools enabled' in s:
            continue
        lines.append(s)
    out = '\n'.join(lines).strip()
    return out


def _key_for(cfg):
    """Resolve the API key: environment variable first, then an optional env file.

    agents.json controls both:
      "api_key_env": "RELAY_API_KEY"                 -> read that env var
      "env_file": "~/.config/qiuqiu/agent.env"       -> or parse it
      "env_keys": ["MY_API_KEY"]                      -> which keys to take from the file
    """
    name = cfg.get('api_key_env')
    if name:
        val = os.environ.get(name, '')
        if val:
            return val
    path = cfg.get('env_file')
    keys = cfg.get('env_keys') or []
    if path and keys:
        try:
            for line in open(os.path.expanduser(path)):
                line = line.strip()
                if '=' in line and not line.startswith('#'):
                    k, v = line.split('=', 1)
                    if k.strip() in keys:
                        return v.strip().strip('"').strip("'")
        except OSError:
            pass
    return ''


def _history_messages(text):
    import time as _t
    global _last_turn_at
    if not history_enabled():
        return []
    now = _t.time()
    if _last_turn_at and (now - _last_turn_at) > HISTORY_IDLE_RESET_S:
        reset_history('idle timeout')
    msgs = list(HISTORY)
    msgs.append({'role': 'user', 'content': text})
    return msgs


def _remember(text, answer):
    global _last_turn_at
    if not history_enabled():
        return
    import time as _t
    HISTORY.append({'role': 'user', 'content': text})
    HISTORY.append({'role': 'assistant', 'content': answer})
    while len(HISTORY) > HISTORY_MAX_TURNS * 2:
        HISTORY.pop(0)
    _last_turn_at = _t.time()


def _ask_http(cfg, text):
    payload = {
        'model': cfg.get('model', 'default'),
        'messages': ([{'role': 'system', 'content': VOICE_SYSTEM}] + _history_messages(text)),
        'stream': False,
    }
    # the gateway silently ignores `model` unless `provider` is sent too
    if cfg.get('provider'):
        payload['provider'] = cfg['provider']
    body = json.dumps(payload).encode()
    headers = {'Content-Type': 'application/json'}
    key = _key_for(cfg)
    if key:
        headers['Authorization'] = f'Bearer {key}'
    req = urllib.request.Request(cfg['url'], data=body, headers=headers)
    with urllib.request.urlopen(req, timeout=cfg.get('timeout', 180)) as r:
        data = json.loads(r.read().decode())
    return (data.get('choices') or [{}])[0].get('message', {}).get('content', '').strip()


def _env_for(cfg):
    """Some CLIs need credentials exported (e.g. dsh needs DEEPSEEK_API_KEY)."""
    env = os.environ.copy()
    path = cfg.get('env_file')
    keys = cfg.get('env_keys') or []
    if path and keys:
        try:
            for line in open(path):
                line = line.strip()
                if '=' in line and not line.startswith('#'):
                    k, v = line.split('=', 1)
                    if k in keys:
                        env[k] = v.strip().strip('"').strip("'")
        except OSError:
            pass
    return env


def _ask_cli(cfg, text):
    context = ''
    if history_enabled() and HISTORY:
        recent = HISTORY[-4:]
        context = '\n\n（刚才的对话：' + ' / '.join(m['content'] for m in recent) + '）'
    prompt = VOICE_SYSTEM + context + '\n\n用户说：' + text
    cmd = list(cfg['cmd']) + [prompt]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=cfg.get('timeout', 180),
                          env=_env_for(cfg))
    out = proc.stdout or ''
    if not out.strip() and proc.stderr:
        out = proc.stderr
    return _clean(out)


_ERROR_MARKERS = ('HTTP_404', 'HTTP_500', 'MISSING_CREDENTIAL', 'no API key',
                  'request failed', 'Traceback (most recent call last)')


def _looks_like_error(text):
    """A CLI agent may print its failure instead of exiting non-zero."""
    low = text.lower()
    return any(m.lower() in low for m in _ERROR_MARKERS)


def _run_agent(name, agent, text):
    if agent.get('type') == 'http':
        return _ask_http(agent, text)
    return _ask_cli(agent, text)


def ask(text):
    """Send one utterance to the active agent, falling back to the others.

    A slow or broken primary (e.g. the Hermes gateway) must not leave the user
    with silence, so each configured fallback is tried until one answers.
    """
    cfg = _config()
    primary = cfg.get('default') or next(iter(cfg.get('agents', {})), '')
    order = [primary] + [f for f in cfg.get('fallback', []) if f != primary and f in cfg['agents']]
    last_err = None
    for name in order:
        agent = cfg['agents'].get(name)
        if not agent:
            continue
        try:
            out = _run_agent(name, agent, text)
            if out and out.strip():
                if name != primary:
                    print(f'[agent] fallback answered: {name}', flush=True)
                _remember(text, out)
                return out
            last_err = RuntimeError(f'{name} returned empty')
        except Exception as e:
            last_err = e
            print(f'[agent] {name} failed: {type(e).__name__}: {e}', flush=True)
    raise last_err or RuntimeError('no agent produced an answer')


if __name__ == '__main__':
    import sys
    print(ask(' '.join(sys.argv[1:]) or '你好'))
