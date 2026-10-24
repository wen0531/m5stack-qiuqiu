#!/usr/bin/env python3
"""Local web front end for the StopWatch simulator.

Binds to 127.0.0.1 only: this is a developer tool, not a remote service.
Serves a live MJPEG view of the 466x466 panel plus A/B key, touch and language
controls.
"""
import base64, json, os, socket, sys, threading, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"
PORT = int(os.environ.get("SIM_WEB_PORT", "8099"))
SIM_PORT = int(os.environ.get("SIM_CTRL_PORT", "9123"))
HERE = os.path.dirname(os.path.abspath(__file__))

_sim_lock = threading.Lock()

def sim(cmd: str, timeout: float = 10.0) -> bytes:
    """Send one command to the simulator control socket."""
    with _sim_lock:
        s = socket.create_connection(("127.0.0.1", SIM_PORT), timeout=timeout)
        try:
            s.sendall((cmd + "\n").encode())
            chunks = []
            while True:
                b = s.recv(65536)
                if not b:
                    break
                chunks.append(b)
                if b.endswith(b"\n") or b.endswith(b"\r\n"):
                    break
            return b"".join(chunks)
        finally:
            s.close()

def frame_png() -> bytes:
    raw = sim("frame")
    return base64.b64decode(raw.strip())

PAGE = open(os.path.join(HERE, "index.html"), "rb").read()

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, code, ctype, body, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)

        if u.path == "/":
            self._send(200, "text/html; charset=utf-8", PAGE)
        elif u.path == "/frame.png":
            try:
                self._send(200, "image/png", frame_png())
            except Exception as e:
                self._send(503, "text/plain", str(e).encode())
        elif u.path == "/stream":
            self.stream()
        elif u.path == "/state":
            self._send(200, "application/json", json.dumps(parse_state(sim("state"))).encode())
        elif u.path == "/log":
            self._send(200, "text/plain; charset=utf-8", sim("log"))
        elif u.path == "/cmd":
            c = q.get("c", [""])[0]
            try:
                self._send(200, "text/plain", sim(c) or b"ok\n")
            except Exception as e:
                self._send(503, "text/plain", str(e).encode())
        else:
            self._send(404, "text/plain", b"not found")

    def stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            while True:
                png = frame_png()
                self.wfile.write(b"--frame\r\nContent-Type: image/png\r\nContent-Length: ")
                self.wfile.write(str(len(png)).encode() + b"\r\n\r\n")
                self.wfile.write(png)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
                time.sleep(0.08)      # ~12 fps
        except (BrokenPipeError, ConnectionResetError, OSError):
            return

def parse_state(raw: bytes) -> dict:
    d = {}
    for kv in raw.decode(errors="replace").split():
        if "=" in kv:
            k, v = kv.split("=", 1)
            d[k] = v
    return d

def main():
    srv = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"simulator web UI: http://{HOST}:{PORT}")
    srv.serve_forever()

if __name__ == "__main__":
    main()
