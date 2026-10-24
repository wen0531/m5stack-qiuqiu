#!/usr/bin/env python3
"""For every language: set it, reopen Settings, capture a screenshot."""
import subprocess, sys, time, os

HERE = os.path.dirname(os.path.abspath(__file__))
CTL = os.path.join(HERE, "simctl")
OUT = os.path.join(HERE, "..", "out")
LANGS = ["en", "zh-Hans", "zh-Hant", "ja", "ko", "fr", "de", "es", "ru", "la"]
APPS = ["AlarmClock", "WatchFace", "Stopwatch", "Qiuqiu", "Settings"]

def ctl(*args):
    return subprocess.run([sys.executable, CTL, *args], capture_output=True, text=True).stdout.strip()

def launcher_page():
    out = ctl("dump")
    for name in APPS:
        if name in out:
            return name
    return None

def go_home():
    ctl("hold", "600"); time.sleep(1.4)

def open_settings():
    go_home()
    for _ in range(10):
        page = launcher_page()
        if page == "Settings":
            ctl("tap", "233", "233"); time.sleep(1.8); return True
        ctl("tap", "430", "233"); time.sleep(1.2)
    return False

def main():
    for lang in LANGS:
        ctl("lang", lang); time.sleep(0.4)
        if not open_settings():
            print(f"{lang}: cannot open Settings"); continue
        time.sleep(0.6)
        ctl("shot", os.path.join(OUT, f"settings_{lang}.png"))
        print(f"  {lang}: ok")
    return 0

if __name__ == "__main__":
    sys.exit(main())
