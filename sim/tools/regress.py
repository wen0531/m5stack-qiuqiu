#!/usr/bin/env python3
"""Deterministic UI regression for the simulator.

Runs a scripted scenario in step mode (fixed clock, time only advances when the
test says so), captures screenshots and compares their hashes against a stored
baseline.

  python3 tools/regress.py            # check against baseline
  python3 tools/regress.py --update   # rewrite the baseline
"""
import hashlib, json, os, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, "..", "build", "stopwatch_sim")
CTL = os.path.join(HERE, "simctl")
OUT = os.path.join(HERE, "..", "out", "regress")
BASELINE = os.path.join(HERE, "..", "regress", "baseline.json")

SCENARIOS = {
    # Every scenario starts from a fresh, paused process and uses explicit
    # steps, so the same screenshots must come out on every run.
    "launcher": [
        ("clock date 2026-10-24", None),
        ("clock 09:41:00", None),
        ("step 1500", None),
        ("shot", "launcher.png"),
    ],
    "watchface": [
        ("open WatchFace", None),
        ("step 2500", None),
        ("shot", "watchface.png"),
    ],
    "settings": [
        ("open Settings", None),
        ("step 2500", None),
        ("shot", "settings.png"),
    ],
    "qiuqiu": [
        ("open Qiuqiu", None),
        ("step 4000", None),
        ("shot", "qiuqiu.png"),
    ],
    "launcher_ja": [
        ("lang ja", None),
        ("step 1200", None),
        ("shot", "launcher_ja.png"),
    ],
    "launcher_ru": [
        ("lang ru", None),
        ("step 1200", None),
        ("shot", "launcher_ru.png"),
    ],
    "settings_zh": [
        ("lang zh-Hans", None),
        ("step 800", None),
        ("open Settings", None),
        ("step 2500", None),
        ("shot", "settings_zh.png"),
    ],
}

def centre_label():
    """The launcher draws the current app name at the bottom of the screen."""
    out = ctl("dump")
    best = None
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        x1, y1, x2, y2 = (int(v) for v in parts[0].split(","))
        if 340 <= y1 <= 430:          # the big centred label
            best = parts[2]
    return best

def wait_for(text, tries=10):
    """Swipe right until the centred launcher label matches one of the names."""
    wanted = [t for t in text.split("|") if t]
    for _ in range(tries):
        cur = centre_label()
        if cur and any(w in cur for w in wanted):
            return True
        ctl("tap 430 233")
        ctl("step 1400")
    cur = centre_label()
    return bool(cur and any(w in cur for w in wanted))

PORT = [9123]

def port_open(port=None, timeout=0.3):
    port = port or PORT[0]
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False

def wait_port(want_open, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        if port_open() == want_open:
            return True
        time.sleep(0.2)
    return False

def ctl(cmd, timeout=60):
    """Send a raw control command (simctl raw) so every verb is available."""
    env = dict(os.environ, SIM_CTRL_PORT=str(PORT[0]))
    r = subprocess.run([sys.executable, CTL, "raw", cmd], capture_output=True,
                       text=True, timeout=timeout, env=env)
    out = (r.stdout or "").strip()
    if r.returncode != 0:
        out += f" [exit {r.returncode}] {r.stderr.strip()[:120]}"
    return out

def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()

def run(update=False):
    os.makedirs(OUT, exist_ok=True)
    baseline = {}
    if os.path.exists(BASELINE):
        baseline = json.load(open(BASELINE))

    failures = []
    for idx, (name, steps) in enumerate(SCENARIOS.items()):
        PORT[0] = 9123 + idx * 7          # a private port per scenario
        # every scenario starts from a fresh process so the launcher page,
        # language and RTC state are identical each run
        # the control socket is a single global port: make sure the previous
        # scenario's process is really gone before starting the next one
        wait_port(False, 10)
        env = dict(os.environ, SIM_START_PAUSED="1", SIM_CTRL_PORT=str(PORT[0]))
        proc = subprocess.Popen([SIM], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, env=env)
        if not wait_port(True, 15):
            print("  ! simulator did not open its control port")
        ctl("pause")
        try:
            for cmd, shot in steps:
                if cmd.startswith("waitfor:"):
                    target = cmd.split(":", 1)[1]
                    ok = wait_for(target)
                    print(f"  waitfor {target}: {'ok' if ok else 'NOT FOUND'}")
                    if not ok:
                        failures.append(f"{name}/waitfor-{target}")
                    continue
                if shot:
                    cmd = f"shot {os.path.join(OUT, shot)}"
                out = ctl(cmd)
                if shot:
                    path = os.path.join(OUT, shot)
                    digest = sha(path)
                    old = baseline.get(f"{name}/{shot}")
                    if update or old is None:
                        baseline[f"{name}/{shot}"] = digest
                        print(f"  {name}/{shot}: baseline {'updated' if update else 'created'}")
                    elif digest != old:
                        failures.append(f"{name}/{shot}")
                        print(f"  {name}/{shot}: MISMATCH")
                    else:
                        print(f"  {name}/{shot}: ok")
                if "not found" in out or "error" in out.lower():
                    print(f"  ! command failed: {cmd} -> {out}")
        finally:
            try:
                ctl("quit", timeout=3)
            except Exception:
                pass
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()

    if update or not os.path.exists(BASELINE):
        os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
        json.dump(baseline, open(BASELINE, "w"), indent=2, sort_keys=True)
        print(f"baseline written to {BASELINE}")

    if failures:
        print(f"FAILED: {len(failures)} screenshot(s) changed: {', '.join(failures)}")
        return 1
    print("PASS: all screenshots match the baseline")
    return 0

if __name__ == "__main__":
    sys.exit(run(update="--update" in sys.argv))
