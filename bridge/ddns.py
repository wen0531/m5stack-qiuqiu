#!/usr/bin/env python3
"""Keep a Cloudflare AAAA record pointed at this host's public IPv6."""
import fcntl, json, os, subprocess, sys, urllib.request, urllib.error
from pathlib import Path

TOKEN_FILE = Path(os.environ.get('CF_TOKEN_FILE', os.path.expanduser('~/.config/qiuqiu/cf_dns_token.txt')))
ZONE = os.environ.get('DDNS_ZONE', 'example.com')
RECORD = os.environ.get('DDNS_RECORD', 'watch.example.com')
IFACE = os.environ.get('DDNS_IFACE', 'eth0')


def token():
    return TOKEN_FILE.read_text().strip()


def api(path, method='GET', body=None):
    req = urllib.request.Request(
        f'https://api.cloudflare.com/client/v4{path}',
        method=method,
        data=json.dumps(body).encode() if body else None,
        headers={'Authorization': f'Bearer {token()}', 'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read().decode())


def local_ipv6():
    """Pick the most durable global IPv6.

    Skips deprecated/tentative addresses (they are on their way out) and scores
    the rest by remaining valid lifetime, preferring /64 SLAAC addresses.
    """
    out = subprocess.check_output(['ip', '-6', '-o', 'addr', 'show', 'dev', IFACE, 'scope', 'global'],
                                  text=True)
    best, best_score = None, -1
    for line in out.splitlines():
        parts = line.split()
        if 'inet6' not in parts:
            continue
        addr, prefix = parts[parts.index('inet6') + 1].split('/')
        if not (addr.startswith('2') or addr.startswith('3')):
            continue
        if 'deprecated' in parts or 'tentative' in parts:
            continue
        valid = 0
        for i, p in enumerate(parts):
            if p == 'valid_lft' and i + 1 < len(parts):
                v = parts[i + 1].replace('sec', '')
                valid = int(v) if v.isdigit() else 0
        score = valid + (10 ** 9 if prefix == '64' else 0)   # prefer /64 SLAAC
        if score > best_score:
            best, best_score = addr, score
    if best is None:
        raise SystemExit('no usable global IPv6 on ' + IFACE)
    return best


def main():
    lock_path = '/tmp/stopwatch-ddns.lock'
    lock = open(lock_path, 'w')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print('another ddns run is in progress, skipping')
        return

    ip = local_ipv6()
    zones = api(f'/zones?name={ZONE}')
    zone_id = zones['result'][0]['id']
    recs = api(f'/zones/{zone_id}/dns_records?name={RECORD}')
    if recs['result']:
        rec = recs['result'][0]
        # repair content and also unexpected ttl/proxied/type drift
        if (rec['content'] == ip and rec.get('ttl') == 60
                and rec.get('proxied') is False and rec.get('type') == 'AAAA'):
            print(f'unchanged {RECORD} -> {ip}')
            return
        api(f"/zones/{zone_id}/dns_records/{rec['id']}", 'PUT', {
            'type': 'AAAA', 'name': RECORD, 'content': ip, 'ttl': 60, 'proxied': False})
        print(f'updated {RECORD} -> {ip}')
    else:
        api(f'/zones/{zone_id}/dns_records', 'POST', {
            'type': 'AAAA', 'name': RECORD, 'content': ip, 'ttl': 60, 'proxied': False})
        print(f'created {RECORD} -> {ip}')


if __name__ == '__main__':
    try:
        main()
    except urllib.error.HTTPError as e:
        print('http error:', e.code, e.read().decode()[:300], file=sys.stderr)
        sys.exit(1)
