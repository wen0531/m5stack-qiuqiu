#!/usr/bin/env python3
"""StopWatch BLE relay bridge: serves the phone page and collects/sends audio."""
import asyncio, json, os, ssl, struct, subprocess, sys, time, urllib.request, wave
from pathlib import Path
from aiohttp import web, WSMsgType
from adpcm import Decoder, Encoder

import agent_backend
import opus as opus_codec

HERE = Path(__file__).parent
REC_DIR = Path('/tmp/stopwatch-relay')
ASR_URL = os.environ.get('ASR_URL', 'http://127.0.0.1:18767/v1/audio/transcriptions')
FFMPEG = '/usr/bin/ffmpeg'
PY_BIN = os.environ.get('RELAY_PY', sys.executable)  # use the running interpreter by default


def run_cmd(cmd, timeout=120):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def asr(path: Path) -> str:
    """Cloud ASR (Volcengine streaming API), falling back to the local model."""
    try:
        r = run_cmd([PY_BIN, str(Path(__file__).parent / 'volc_stream_asr.py'), '--file', str(path)], timeout=120)
        for line in r.stdout.splitlines():
            if line.startswith('TRANSCRIPT:'):
                text = line.split('TRANSCRIPT:', 1)[1].strip()
                if text:
                    return text
    except Exception as e:
        print('[relay] cloud asr failed:', e, flush=True)
    r = run_cmd(['curl', '-s', '--max-time', '120', '-X', 'POST', ASR_URL,
                 '-F', 'model=qwen3-asr', '-F', f'file=@{path};type=audio/wav'], timeout=150)
    try:
        import json as _j
        data = _j.loads(r.stdout)
        text = data.get('text', '')
        if '<asr_text>' in text:
            text = text.split('<asr_text>', 1)[1].split('<', 1)[0]
        return text.strip()
    except Exception:
        return ''


def _gateway_key():
    try:
        for line in open(os.environ.get('HERMES_ENV_FILE', os.path.expanduser('~/.hermes/.env'))):
            line = line.strip()
            if line.startswith('API_SERVER_KEY='):
                return line.split('=', 1)[1].strip()
    except Exception:
        pass
    return ''


import unicodedata

SWITCH_PREFIXES = ('换', '叫', '切到', '切换到', '用', '找', '换成')

RESET_PHRASES = ('新话题', '换个话题', '忘掉刚才', '忘记刚才', '重新开始', '清空对话', '重新说')


def _norm(text: str) -> str:
    """Lowercase, strip punctuation/spaces so ASR variance does not matter."""
    t = unicodedata.normalize('NFKC', text).lower()
    return ''.join(ch for ch in t if ch.isalnum())


def match_agent(text: str):
    """Detect a backend switch request.

    Returns (agent_name, remaining_text) or (None, text).
    - whole utterance equals an alias          -> switch only
    - alias + separator + question            -> switch and forward the question
    - 换/叫/切到/用 + alias                    -> switch only
    """
    cfg = agent_backend._config()
    raw = text.strip()
    norm = _norm(raw)
    if not norm:
        return None, text

    for name, meta in cfg.get('agents', {}).items():
        for alias in meta.get('aliases', []):
            a = _norm(alias)
            if not a:
                continue
            if norm == a:
                return name, ''
            for pre in SWITCH_PREFIXES:
                if norm == _norm(pre + alias):
                    return name, ''
            # alias + explicit separator + question ("小马，今天天气怎么样").
            # Without a separator ("小马是谁") it is an ordinary question.
            if raw.lower().startswith(alias.lower()) and len(raw) > len(alias):
                tail = raw[len(alias):]
                if tail[0] not in ' ,，。:：、!！?？~～\t':
                    continue
                rest = tail.lstrip(' ,，。:：、!！?？~～\t')
                if rest:
                    return name, rest
    return None, text


def ask_agent(text: str) -> str:
    """Send the utterance to whichever backend is selected in agents.json."""
    try:
        return agent_backend.ask(text)
    except Exception as e:
        print('[relay] agent failed:', type(e).__name__, e, flush=True)
        return ''


def hermes_chat(text: str, system_prompt: str = '') -> str:
    """Ask the local Hermes gateway (OpenAI-compatible API, same channel as its IM gateways)."""
    key = _gateway_key()
    if not key:
        return ''
    sys_p = system_prompt or ('你是 M5Stack 手表上的语音助手。回答必须口语化、简短（一到两句话），'
                              '不要用 Markdown、列表或表情符号。')
    body = json.dumps({
        'model': 'hermes',
        'messages': [{'role': 'system', 'content': sys_p},
                     {'role': 'user', 'content': text}],
        'stream': False,
    }).encode()
    req = urllib.request.Request('http://127.0.0.1:8642/v1/chat/completions', data=body,
                                 headers={'Content-Type': 'application/json',
                                          'Authorization': f'Bearer {key}'})
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            data = json.loads(r.read().decode())
        return (data.get('choices') or [{}])[0].get('message', {}).get('content', '').strip()
    except Exception as e:
        print('[relay] hermes failed:', e, flush=True)
        return ''


def tts_to_wav(text: str, out: Path) -> bool:
    mp3 = out.with_suffix('.mp3')
    r = run_cmd([PY_BIN, '-m', 'edge_tts', '--voice', 'zh-CN-XiaoxiaoNeural',
                 '--text', text, '--write-media', str(mp3)], timeout=120)
    if not mp3.exists():
        print('[relay] tts failed', r.stderr[-300:], flush=True)
        return False
    r = run_cmd([FFMPEG, '-y', '-v', 'error', '-i', str(mp3), '-ar', '16000', '-ac', '1', str(out)], timeout=120)
    return out.exists()


STOP_PLAY = {'flag': False}   # set by the watch's A-key interrupt


async def play_clip(ws, wav_path, tail_wait=1.3, label='reply'):
    """Send one clip in the background so the WS receive loop stays responsive
    (otherwise an interrupt from the watch would only be read after the whole
    clip had been pushed)."""
    try:
        await ws.send_json({'type': 'speaking'})
        await send_downlink(ws, wav_path)
        if STOP_PLAY['flag']:
            print('[relay] playback cancelled', flush=True)
            return
        await asyncio.sleep(tail_wait)
        await ws.send_json({'type': 'done'})
        if label == 'reply':
            print('[relay] playback done', flush=True)
    except Exception as e:
        print(f'[relay] play_clip failed: {type(e).__name__}: {e}', flush=True)


async def send_downlink(ws, wav_path: Path):
    """Stream one clip to the watch.

    Codec is chosen by DOWNLINK_CODEC (default: opus 32 kbps / 60 ms). ADPCM is
    kept as a fallback for A/B comparison.
    """
    STOP_PLAY['flag'] = False
    with wave.open(str(wav_path)) as w:
        pcm = list(struct.unpack('<%dh' % w.getnframes(), w.readframes(w.getnframes())))

    codec = os.environ.get('DOWNLINK_CODEC', 'opus').lower()
    if codec == 'adpcm':
        await ws.send_json({'type': 'codec', 'id': 1})
        enc = Encoder()
        chunk = 320
        for off in range(0, len(pcm), chunk):
            if STOP_PLAY['flag']:
                print('[relay] downlink cancelled by watch', flush=True)
                return
            piece = pcm[off:off + chunk]
            if len(piece) < chunk:
                piece += [0] * (chunk - len(piece))
            await ws.send_bytes(enc.encode(piece))
            await asyncio.sleep(0.02)
        return

    # Opus CBR 32 kbps, 60 ms frames (240 B payload -> 244 B packet)
    await ws.send_json({'type': 'codec', 'id': 2})
    bitrate = int(os.environ.get('OPUS_BITRATE', '32000'))
    enc = opus_codec.Encoder(fs=16000, channels=1, bitrate=bitrate, complexity=5, frame_ms=60)
    frame = enc.frame_size            # 960 samples at 16 kHz
    # push the first frames back-to-back so the watch has ~300 ms of slack,
    # then follow the media clock exactly
    HEAD_START_MS = int(os.environ.get('OPUS_HEAD_START_MS', '300'))
    sent_ms = 0
    t0 = time.monotonic()
    for off in range(0, len(pcm), frame):
        if STOP_PLAY['flag']:
            print('[relay] downlink cancelled by watch', flush=True)
            return
        piece = pcm[off:off + frame]
        if len(piece) < frame:
            piece += [0] * (frame - len(piece))
        await ws.send_bytes(enc.encode(piece))
        sent_ms += 60
        if sent_ms > HEAD_START_MS:
            delay = (t0 + sent_ms / 1000.0) - time.monotonic()
            if delay > 0:
                await asyncio.sleep(delay)

REC_DIR.mkdir(exist_ok=True)

APP_WS = None            # last connected relay app socket
BATTERY_FUT = None       # pending export future


class Session:
    def __init__(self):
        self.decoder = Decoder()
        self.pcm = []
        self.recording = False

    def reset(self):
        self.decoder = Decoder()
        self.pcm = []

    def feed(self, payload: bytes):
        self.pcm.extend(self.decoder.decode(payload))

    def save(self):
        if not self.pcm:
            return None
        ts = time.strftime('%Y%m%d-%H%M%S')
        path = REC_DIR / f'rec-{ts}.wav'
        with wave.open(str(path), 'wb') as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
            w.writeframes(b''.join(int(x).to_bytes(2, 'little', signed=True) for x in self.pcm))
        return path


async def index(request):
    return web.FileResponse(HERE / 'index.html')


async def battery_export(request):
    """Ask the phone app to pull the watch's battery log and return it as CSV."""
    global BATTERY_FUT
    if APP_WS is None:
        return web.json_response({'ok': False, 'error': 'no phone app connected'}, status=503)
    loop = asyncio.get_event_loop()
    BATTERY_FUT = loop.create_future()
    try:
        await APP_WS.send_json({'cmd': 'battery'})
    except Exception as e:
        return web.json_response({'ok': False, 'error': f'phone link lost: {e}'}, status=503)
    try:
        data = await asyncio.wait_for(BATTERY_FUT, timeout=32)
    except asyncio.TimeoutError:
        return web.json_response({'ok': False, 'error': 'timeout'}, status=504)
    finally:
        BATTERY_FUT = None
    path = REC_DIR / f'battery-{time.strftime("%Y%m%d-%H%M%S")}.csv'
    path.write_text(data)
    print(f'[relay] battery csv saved {path} ({len(data)} bytes)', flush=True)
    return web.json_response({'ok': True, 'file': str(path), 'csv': data[:2000]})


async def agent_info(request):
    cfg = agent_backend._config()
    return web.json_response({'active': cfg.get('default'), 'available': list(cfg['agents'])})


async def agent_switch(request):
    name = request.query.get('name') or (await request.json()).get('name', '')
    try:
        agent_backend.set_agent(name)
    except KeyError:
        return web.json_response({'ok': False, 'error': f'unknown agent {name}'}, status=400)
    print(f'[relay] agent switched to {name}', flush=True)
    return web.json_response({'ok': True, 'active': name})


async def ws_handler(request):
    ws = web.WebSocketResponse()
    await ws.prepare(request)
    sess = request.app['session']
    global APP_WS
    APP_WS = ws
    print('[relay] phone connected', flush=True)
    try:
        async for msg in ws:
            if msg.type == WSMsgType.BINARY:
                if sess.recording:
                    sess.feed(msg.data)
                    if len(sess.pcm) % 16000 < 320:
                        await ws.send_json({'type': 'level', 'samples': len(sess.pcm)})
            elif msg.type == WSMsgType.TEXT:
                try:
                    cmd = json.loads(msg.data)
                except Exception:
                    continue
                if cmd.get('type') == 'battery_csv':
                    global BATTERY_FUT
                    if BATTERY_FUT is not None and not BATTERY_FUT.done():
                        BATTERY_FUT.set_result(cmd.get('data', ''))
                    continue
                if cmd.get('cmd') == 'start':
                    sess.reset(); sess.recording = True
                    await ws.send_json({'type': 'recording', 'on': True})
                    print('[relay] recording start', flush=True)
                elif cmd.get('cmd') == 'stop':
                    sess.recording = False
                    path = sess.save()
                    await ws.send_json({'type': 'saved', 'file': str(path) if path else None,
                                        'seconds': round(len(sess.pcm) / 16000, 2)})
                    print('[relay] saved', path, flush=True)
                    if path is not None:
                        text = asr(path)
                        print('[relay] asr:', text, flush=True)
                        await ws.send_json({'type': 'asr', 'text': text})

                        norm_text = _norm(text) if text else ''
                        if norm_text and any(_norm(p) in norm_text for p in RESET_PHRASES):
                            agent_backend.reset_history('user asked')
                            greet = '好，我们重新开始'
                            wav = REC_DIR / 'reset.wav'
                            if tts_to_wav(greet, wav):
                                asyncio.create_task(play_clip(ws, wav, 1.0, 'greet'))
                            text = ''

                        target, rest = match_agent(text) if text else (None, text)
                        if target:
                            agent_backend.set_agent(target)
                            print(f'[relay] voice switch -> {target}', flush=True)
                            await ws.send_json({'type': 'agent', 'active': target,
                                                'color': meta.get('color', 0)})
                            meta = agent_backend._config()['agents'].get(target, {})
                            alias = (meta.get('aliases') or [target])[0]
                            greet = f'{alias}在'
                            wav = REC_DIR / 'switch.wav'
                            if tts_to_wav(greet, wav):
                                asyncio.create_task(play_clip(ws, wav, 1.0, 'greet'))
                            text = rest

                        if text:
                            if os.environ.get('REPLY_MODE', 'hermes') == 'echo':
                                reply = os.environ.get('REPLY_PREFIX', '我听到你说，') + text
                            else:
                                await ws.send_json({'type': 'thinking'})
                                reply = ask_agent(text)
                                if not reply:
                                    reply = '抱歉，我现在没法回答。'
                            await ws.send_json({'type': 'reply', 'text': reply})
                            wav = REC_DIR / 'reply.wav'
                            if tts_to_wav(reply, wav):
                                asyncio.create_task(play_clip(ws, wav))
                elif cmd.get('cmd') == 'agent_status':
                    await ws.send_json({'type': 'agent', 'active': agent_backend.current_agent(),
                                        'source': 'sync'})

                elif cmd.get('cmd') == 'stop_play':
                    STOP_PLAY['flag'] = True
                    print('[relay] stop_play requested by watch', flush=True)

                elif cmd.get('cmd') == 'agent_next':
                    cfg = agent_backend._config()
                    order = list(cfg.get('agents', {}).keys())
                    cur = agent_backend.current_agent()
                    nxt = order[(order.index(cur) + 1) % len(order)] if cur in order else order[0]
                    agent_backend.set_agent(nxt)
                    print(f'[relay] B-key cycle -> {nxt}', flush=True)
                    # firmware already changed the colour locally; do not resend it
                    await ws.send_json({'type': 'agent', 'active': nxt, 'source': 'bkey'})

                elif cmd.get('cmd') == 'agent':
                    name = cmd.get('name', '')
                    try:
                        agent_backend.set_agent(name)
                        print(f'[relay] agent switched to {name} (from app)', flush=True)
                        await ws.send_json({'type': 'agent', 'active': name})
                    except KeyError:
                        await ws.send_json({'type': 'agent', 'error': f'unknown {name}'})

                elif cmd.get('cmd') == 'play':
                    # send a WAV back to the phone -> watch (ADPCM downlink)
                    target = cmd.get('file') or ''
                    p = Path(target)
                    if p.exists():
                        with wave.open(str(p)) as w:
                            pcm = list(__import__('struct').unpack('<%dh' % w.getnframes(), w.readframes(w.getnframes())))
                        enc = Encoder()
                        chunk = 320
                        for off in range(0, len(pcm), chunk):
                            piece = pcm[off:off + chunk]
                            if len(piece) < chunk:
                                piece += [0] * (chunk - len(piece))
                            await ws.send_bytes(enc.encode(piece))
                            await asyncio.sleep(0.02)   # real-time pacing
                        await ws.send_json({'type': 'played', 'file': str(p)})
    finally:
        sess.recording = False
        print('[relay] phone disconnected', flush=True)
    return ws


async def serve():
    """HTTPS (phone page) + plain WS (native app) share one session object."""
    session = Session()

    app = web.Application()
    app['session'] = session
    app.router.add_get('/', index)
    app.router.add_get('/ws', ws_handler)
    app.router.add_get('/battery', battery_export)
    app.router.add_get('/agent', agent_info)
    app.router.add_post('/agent', agent_switch)

    plain = web.Application()
    plain['session'] = session
    plain.router.add_get('/ws', ws_handler)
    plain.router.add_get('/battery', battery_export)
    plain.router.add_get('/agent', agent_info)
    plain.router.add_post('/agent', agent_switch)

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(str(HERE / 'cert.pem'), str(HERE / 'key.pem'))

    tls_port = int(os.environ.get('RELAY_PORT', '8443'))
    plain_port = int(os.environ.get('RELAY_PLAIN_PORT', '8444'))

    tls_runner = web.AppRunner(app, prints=None)
    await tls_runner.setup()
    await web.TCPSite(tls_runner, None, tls_port, ssl_context=ctx).start()   # dual-stack

    plain_runner = web.AppRunner(plain, prints=None)
    await plain_runner.setup()
    await web.TCPSite(plain_runner, None, plain_port).start()   # dual-stack

    print(f'[relay] https://0.0.0.0:{tls_port} (phone page)', flush=True)
    print(f'[relay] ws://0.0.0.0:{plain_port}/ws (native app)', flush=True)
    while True:
        await asyncio.sleep(3600)


def main():
    asyncio.run(serve())


if __name__ == '__main__':
    main()
