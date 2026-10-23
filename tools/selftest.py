#!/usr/bin/env python3
"""One-shot health check for the StopWatch voice relay.

Run with the BLE venv:
  ~/.local/share/esp32-tools/ble-venv/bin/python selftest.py [--agents]
"""
import json, subprocess, sys, urllib.request
from pathlib import Path

HERE = Path(__file__).parent
RESULTS = []


def check(name, fn, hint=''):
    try:
        ok, detail = fn()
    except Exception as e:
        ok, detail = False, f'{type(e).__name__}: {e}'
    RESULTS.append((name, ok, detail, hint))
    print(f"{'✓' if ok else '✗'} {name}: {detail}")


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()


def unit_active(unit):
    def run():
        out = sh(f'systemctl --user is-active {unit}')
        return out == 'active', out or 'not running'
    return run


def port_open(port):
    def run():
        out = sh(f"ss -ltn | grep ':{port} '")
        return bool(out), 'listening' if out else 'closed'
    return run


def local_https():
    def run():
        code = sh("curl -sk -m 5 -o /dev/null -w '%{http_code}' https://127.0.0.1:8443/")
        return code == '200', f'HTTP {code}'
    return run


def ddns_matches():
    def run():
        sys.path.insert(0, str(HERE))
        import ddns
        local = ddns.local_ipv6()
        zone_id = ddns.api(f'/zones?name={ddns.ZONE}')['result'][0]['id']
        rec = ddns.api(f'/zones/{zone_id}/dns_records?name={ddns.RECORD}')['result'][0]
        same = rec['content'] == local
        extra = '' if (rec.get('ttl') == 60 and rec.get('proxied') is False) else ' (ttl/proxied 被改过)'
        return same, f"record={rec['content']} local={local}{extra}"
    return run


def agent_endpoint():
    def run():
        body = sh('curl -s -m 5 http://127.0.0.1:8444/agent')
        data = json.loads(body)
        return True, f"active={data.get('active')} available={len(data.get('available', []))}"
    return run


def phone_app():
    def run():
        body = sh('curl -s -m 20 http://127.0.0.1:8444/battery')
        if not body:
            return False, 'no response'
        data = json.loads(body)
        if data.get('ok'):
            csv = data.get('csv', '')
            lines = [l for l in csv.strip().splitlines() if l and not l.startswith('uptime') and l != 'batt,done']
            last = lines[-1] if lines else '(no samples)'
            return True, f'connected, last sample: {last}'
        return False, data.get('error', 'unknown')
    return run


def watch_advertising():
    def run():
        import asyncio
        from bleak import BleakScanner

        async def scan():
            devs = await BleakScanner.discover(timeout=6)
            return [d.name for d in devs if d.name and 'StopWatch' in d.name]

        names = asyncio.run(scan())
        return bool(names), names[0] if names else 'not advertising (watch off or already connected)'
    return run


def agents_alive():
    def run():
        sys.path.insert(0, str(HERE))
        import agent_backend as ab
        out = []
        for name in ('hermes', 'codex', 'dsh'):
            try:
                ab._run_agent(name, ab._config()['agents'][name], '回一个字：好')
                out.append(f'{name}=ok')
            except Exception as e:
                out.append(f'{name}=FAIL({type(e).__name__})')
        return all('ok' in o for o in out), ' '.join(out)
    return run


def main():
    print('=== StopWatch 中继自检 ===\n')
    check('桥服务 stopwatch-relay', unit_active('stopwatch-relay.service'))
    check('DDNS 定时器', unit_active('stopwatch-ddns.timer'))
    check('端口 8443', port_open(8443))
    check('端口 8444', port_open(8444))
    check('本机 HTTPS', local_https())
    check('DDNS 记录与本机一致', ddns_matches(), 'systemctl --user start stopwatch-ddns.service')
    check('agent 接口', agent_endpoint())
    check('手机 App 已连接', phone_app(), '手机打开「手表中继」，或检查 Wi-Fi/流量')
    app_ok = RESULTS[-2][1] if len(RESULTS) >= 2 else False
    if app_ok:
        print('✓ 手表在广播: 跳过（已通过 App 连接，此时不广播属正常）')
    else:
        check('手表在广播', watch_advertising(), '手表没电/已连接时属正常')
    if '--agents' in sys.argv:
        check('三个后台可用', agents_alive())

    bad = [r for r in RESULTS if not r[1]]
    print(f'\n=== {len(RESULTS) - len(bad)}/{len(RESULTS)} 项通过 ===')
    if bad:
        print('\n需要处理：')
        for name, _, detail, hint in bad:
            print(f'  - {name}: {detail}')
            if hint:
                print(f'    建议: {hint}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
