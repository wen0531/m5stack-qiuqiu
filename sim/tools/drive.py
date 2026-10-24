#!/usr/bin/env python3
"""Drive the simulator: navigate the launcher and screenshot every language."""
import subprocess, sys, time, os

CTL = os.path.join(os.path.dirname(__file__), "simctl")

def ctl(*args, **kw):
    return subprocess.run([sys.executable, CTL, *args], capture_output=True, text=True, **kw).stdout.strip()

def dump():
    out = ctl("dump")
    rows = []
    for line in out.splitlines():
        if "\t" not in line:
            continue
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        coords, clickable, text = parts[0], parts[1], "\t".join(parts[2:])
        x1, y1, x2, y2 = map(int, coords.split(","))
        rows.append((text, x1, y1, x2, y2))
    return rows

def visible(name):
    return any(name in r[0] for r in dump())

def state():
    s = ctl("state")
    d = {}
    for kv in s.split():
        if "=" in kv:
            k, v = kv.split("=", 1)
            d[k] = v
    return d

def go_home():
    ctl("hold", "600"); time.sleep(1.2)

def open_app(name):
    go_home()
    for _ in range(8):
        if visible(name):
            ctl("tap", "233", "233"); time.sleep(1.5); return True
        ctl("tap", "430", "233"); time.sleep(1.1)
    return False

def main():
    if not open_app("Settings"):
        print("could not reach Settings"); return 1
    print("opened:", state().get("app"))
    outdir = os.path.join(os.path.dirname(__file__), "..", "out")
    seen = set()
    for _ in range(14):
        rows = dump()
        lang_row = [r for r in rows if "Language" in r[0] or "语言" in r[0] or "言語" in r[0]
                    or "언어" in r[0] or "Langue" in r[0] or "Sprache" in r[0] or "Idioma" in r[0]
                    or "Язык" in r[0] or "Lingua" in r[0] or "設定" in r[0]]
        if not lang_row:
            ctl("swipe", "233", "380", "233", "120", "--ms", "350")
            time.sleep(0.6)
            continue
        text, x1, y1, x2, y2 = lang_row[0]
        cur = state().get("lang", "?")
        subprocess.run([sys.executable, CTL, "shot", f"{outdir}/ui_{cur}.png"], capture_output=True)
        if cur in seen:
            print("cycle complete at", cur); break
        seen.add(cur)
        print("language:", cur, "-> tapping", repr(text))
        ctl("tap", str((x1 + x2) // 2), str((y1 + y2) // 2))
        time.sleep(1.4)
    print("captured:", sorted(seen))
    return 0

if __name__ == "__main__":
    sys.exit(main())
