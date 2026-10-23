#!/usr/bin/env python3
"""Turn the watch battery-log CSV into a runtime estimate.

Usage: python3 battery_report.py <csv> [--capacity 450]
"""
import sys
from pathlib import Path

CAPACITY_MAH = 450


def load(path):
    rows = []
    for line in Path(path).read_text(errors='ignore').splitlines():
        parts = line.strip().split(',')
        if len(parts) != 3:
            continue
        try:
            t, mv, pct = int(parts[0]), int(parts[1]), int(parts[2])
        except ValueError:
            continue
        rows.append((t, mv, pct))
    return rows


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    cap = CAPACITY_MAH
    if '--capacity' in sys.argv:
        cap = int(sys.argv[sys.argv.index('--capacity') + 1])
    rows = load(sys.argv[1])
    if len(rows) < 2:
        print(f'样本不足（{len(rows)} 条），无法估算')
        return

    # split into runs: a reboot resets uptime, so a decrease starts a new run
    runs, cur = [], [rows[0]]
    for prev, now in zip(rows, rows[1:]):
        if now[0] < prev[0]:
            runs.append(cur)
            cur = []
        cur.append(now)
    runs.append(cur)

    total_s = 0
    for i, run in enumerate(runs, 1):
        (t0, _, p0), (t1, _, p1) = run[0], run[-1]
        dur = t1 - t0
        total_s += dur
        d_pct = p0 - p1
        mv = sum(r[1] for r in run) / len(run)
        print(f'段 {i}: {dur/3600:5.2f} h  电量 {p0}% -> {p1}%  (Δ{d_pct:+.1f}%)  平均 {mv:.0f} mV')

    (t0, mv0, p0), (t1, mv1, p1) = rows[0], rows[-1]
    if total_s <= 0:
        print('时间跨度为 0，样本太少')
        return
    hours = total_s / 3600
    used_pct = p0 - p1
    print(f'\n总计: {hours:.2f} h, 电量 {p0}% -> {p1}%, 掉电 {used_pct}%')
    if used_pct > 0:
        used_mah = cap * used_pct / 100.0
        avg_ma = used_mah / hours * 1000
        full_h = cap / avg_ma
        print(f'平均电流: {avg_ma:.1f} mA')
        print(f'满电续航: {full_h:.1f} h ({full_h*60:.0f} 分钟)')
    else:
        print('电量没有下降（可能还在充电或样本太短）')
    print(f'电压: {mv0} mV -> {mv1} mV')


if __name__ == '__main__':
    main()
