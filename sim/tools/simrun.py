#!/usr/bin/env python3
"""Supervisor for the simulator.

Runs the simulator as a child process, executes a list of commands and reports
crashes, hangs and the last good frame.  The supervisor must live outside the
measured process: once the simulator dies it can no longer answer for itself.
"""
import os, signal, socket, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, "..", "build", "stopwatch_sim")
CTL = os.path.join(HERE, "simctl")
PORT = 9123


def ctl(*args, timeout=10):
    r = subprocess.run([sys.executable, CTL, *args], capture_output=True, text=True, timeout=timeout)
    return r.stdout.strip()


def wait_port(timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", PORT), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def run(commands, shot_dir="out", stall_ms=3000):
    os.makedirs(shot_dir, exist_ok=True)
    log_path = os.path.join(shot_dir, "sim.log")
    log = open(log_path, "wb")
    proc = subprocess.Popen([SIM], stdout=log, stderr=subprocess.STDOUT)
    result = {"crashed": False, "stalled": False, "exit": None, "last_frame": None}

    if not wait_port():
        proc.kill()
        result["crashed"] = True
        result["reason"] = "control socket never came up"
        return result

    last_loops, last_ms = 0, 0
    try:
        for cmd in commands:
            if isinstance(cmd, (int, float)):
                time.sleep(cmd)
                continue
            out = ctl(*cmd.split(" ", 1)) if " " in cmd else ctl(cmd)
            if cmd.startswith("shot"):
                result["last_frame"] = cmd.split(" ", 1)[-1]
            # stall detection on the simulator heartbeat
            st = ctl("state")
            for kv in st.split():
                if kv.startswith("loops="):
                    loops = int(kv.split("=")[1])
                    if loops == last_loops:
                        time.sleep(stall_ms / 1000)
                        st2 = ctl("state")
                        loops2 = int([x for x in st2.split() if x.startswith("loops=")][0].split("=")[1])
                        if loops2 == loops:
                            result["stalled"] = True
                    last_loops = loops
            if proc.poll() is not None:
                break

        # final progress check: a frozen simulator keeps the socket alive but
        # stops advancing its loop counter
        if proc.poll() is None:
            def loops_now():
                st = ctl("state")
                for kv in st.split():
                    if kv.startswith("loops="):
                        return int(kv.split("=")[1])
                return -1
            a = loops_now()
            time.sleep(stall_ms / 1000)
            b = loops_now()
            if a >= 0 and a == b:
                result["stalled"] = True
    finally:
        if proc.poll() is None:
            try:
                ctl("quit", timeout=3)
            except Exception:
                pass
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()

    rc = proc.returncode
    result["exit"] = rc
    if rc is not None and rc < 0:
        result["crashed"] = True
        result["signal"] = -rc
        result["signal_name"] = signal.Signals(-rc).name
    elif rc not in (0, None):
        result["crashed"] = True

    if result["crashed"] or result["stalled"]:
        print("---- last 30 log lines ----")
        with open(log_path, "rb") as f:
            for line in f.read().decode(errors="replace").splitlines()[-30:]:
                print(line)
    return result


def main():
    argv = sys.argv[1:]
    cmds = argv if argv else ["state"]
    res = run(cmds)
    print(res)
    return 1 if (res["crashed"] or res["stalled"]) else 0


if __name__ == "__main__":
    sys.exit(main())
