"""Volcengine (豆包/火山) ASR client: submit file -> query -> text."""
import base64, json, os, time, uuid, urllib.request, urllib.error
from pathlib import Path

ENV_FILE = Path(os.environ.get('VOLC_ASR_ENV', os.path.expanduser('~/.config/qiuqiu/volc_asr.env')))
BASE = 'https://openspeech.bytedance.com/api/v3/auc/bigmodel'


def load_env():
    cfg = {}
    if ENV_FILE.exists():
        for line in ENV_FILE.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith('#') or '=' not in line:
                continue
            k, v = line.split('=', 1)
            cfg[k.strip()] = v.strip().strip('"').strip("'")
    return cfg


def _post(url, body, headers, timeout=60):
    data = json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, headers={**headers, 'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, dict(r.headers), r.read().decode(errors='replace')


def transcribe(wav_path, timeout=120):
    cfg = load_env()
    app = cfg.get('VOLCENGINE_ASR_APP_ID')
    token = cfg.get('VOLCENGINE_ASR_ACCESS_TOKEN')
    resource = cfg.get('VOLCENGINE_ASR_RESOURCE_ID', 'volc.bigasr.auc')
    if not (app and token):
        raise RuntimeError('volcengine credentials missing')

    req_id = str(uuid.uuid4())
    audio_b64 = base64.b64encode(Path(wav_path).read_bytes()).decode()
    body = {
        'user': {'uid': 'stopwatch-relay'},
        'audio': {'format': 'wav', 'rate': 16000, 'bits': 16, 'channel': 1, 'data': audio_b64},
        'request': {'model_name': 'bigmodel', 'enable_itn': True, 'enable_punc': True},
    }
    headers = {
        'X-Api-App-Key': app,
        'X-Api-Access-Key': token,
        'X-Api-Resource-Id': resource,
        'X-Api-Request-Id': req_id,
        'X-Api-Sequence': '-1',
    }
    status, hdr, txt = _post(f'{BASE}/submit', body, headers)
    code = hdr.get('X-Api-Status-Code') or hdr.get('x-api-status-code')
    if code not in (None, '20000000', '20000001'):
        raise RuntimeError(f'submit failed: {code} {hdr.get("X-Api-Message")} {txt[:200]}')
    task_id = None
    try:
        task_id = json.loads(txt).get('id') or json.loads(txt).get('task_id')
    except Exception:
        pass

    deadline = time.time() + timeout
    while time.time() < deadline:
        qbody = {'user': {'uid': 'stopwatch-relay'}, 'audio': {'format': 'wav', 'rate': 16000, 'bits': 16, 'channel': 1},
                 'request': {'model_name': 'bigmodel'}}
        if task_id:
            qbody['audio']['task_id'] = task_id
        qh = dict(headers)
        qh['X-Api-Request-Id'] = str(uuid.uuid4())
        st, wh, wt = _post(f'{BASE}/query', qbody, qh)
        qc = wh.get('X-Api-Status-Code') or wh.get('x-api-status-code')
        if qc == '20000000':
            try:
                j = json.loads(wt)
                res = j.get('result') or {}
                text = res.get('text') or j.get('text') or ''
                if text:
                    return text.strip()
            except Exception:
                pass
        elif qc and qc not in ('20000001', '20000002'):
            raise RuntimeError(f'query failed: {qc} {wh.get("X-Api-Message")} {wt[:200]}')
        time.sleep(1.0)
    raise TimeoutError('volcengine asr timeout')


if __name__ == '__main__':
    import sys
    print(transcribe(sys.argv[1]))
