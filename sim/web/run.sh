#!/usr/bin/env bash
# Start the simulator and its local web UI, then print the URL.
# Everything stays on 127.0.0.1 - this is a developer tool.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"

if ! [ -x "$ROOT/build/stopwatch_sim" ]; then
    echo "building simulator..."
    cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null
fi

if ! pgrep -f "build/stopwatch_sim" >/dev/null; then
    echo "starting simulator..."
    (cd "$ROOT" && setsid nohup ./build/stopwatch_sim > /tmp/stopwatch_sim.log 2>&1 < /dev/null &)
    sleep 3
fi

PORT="${SIM_WEB_PORT:-8099}"
if ! curl -s -o /dev/null "http://127.0.0.1:$PORT/"; then
    echo "starting web UI..."
    setsid nohup python3 "$HERE/serve.py" > /tmp/stopwatch_web.log 2>&1 < /dev/null &
    sleep 2
fi

echo
echo "  模拟器网页： http://127.0.0.1:$PORT/"
echo "  停止：       pkill -f serve.py; pkill -f build/stopwatch_sim"
echo
