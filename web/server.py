#!/usr/bin/env python3
"""
OS concept: A read-only observer of ReliefOS over a local web page — IPC (the shared-memory
status table via reliefstat), process control (reliefctl -> signals), a threaded server — Units 1/2

web/server.py: the ReliefOS web dashboard. Python standard library only (http.server +
threading), no internet, bound to 127.0.0.1 only.

    python3 web/server.py [--port 8080] [--public]   (or: scripts/reliefctl web [--public])

--public: for sharing through a tunnel (e.g. cloudflared). Any Host is answered, but only a
request made on this machine (Host 127.0.0.1/localhost, no proxy headers) may control ReliefOS.
Everyone else gets a read-only view: POST /api/action is refused and the buttons are disabled.

Endpoints:
  GET  /                    web/index.html (one file, inline CSS/JS, no CDN)
  GET  /api/status          `build/reliefstat --json` (+ each camp's process state from /proc)
  GET  /api/events          Server-Sent Events: reliefd.log lines as JSON, the last 40 first,
                            then every new line as it is written
  POST /api/action          {"cmd": "start|stop|alert|dump|simulate", "camp": N|"all"}
                            runs scripts/reliefctl with a fixed argument list (never a shell);
                            simulate runs `reliefctl simulate flood` in the background
  GET  /api/log?pos=N      the same log lines by polling: {"pos": next N, "events": [...]}, for
                            viewers behind a quick tunnel (cloudflared quick tunnels drop SSE)
  GET  /api/job             the background simulation: running, return code, output tail
  GET  /api/experiments     the figures of docs/EXPERIMENTS.md with their captions
  GET  /figures/<path>.png  a PNG under docs/figures (nothing else on disk is served)

Safety: the server answers only Host 127.0.0.1:<port> or localhost:<port> (no DNS rebinding);
a POST must be JSON (a cross-site form cannot send that without a CORS preflight, which is
never granted) and, when the browser sends an Origin, come from this page.
Environment: RELIEF_HOME (default <repo>/run), RELIEF_BIN_DIR (default <repo>/build), RELIEF_WRAP
(a command prefix for reliefstat, as in reliefctl: e.g. valgrind, or setarch for the TSan build).
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent.parent
BIN = Path(os.environ.get("RELIEF_BIN_DIR") or ROOT / "build")
HOME = Path(os.environ.get("RELIEF_HOME") or ROOT / "run")
CTL = ROOT / "scripts" / "reliefctl"
WRAP = shlex.split(os.environ.get("RELIEF_WRAP", ""))
FIGURES = (ROOT / "docs" / "figures").resolve()
INDEX = ROOT / "web" / "index.html"
EXPERIMENTS_MD = ROOT / "docs" / "EXPERIMENTS.md"

ACTIONS = {"start", "stop", "alert", "dump", "simulate"}     # the allow-list
MAX_BODY = 2048
LOG_TAIL = 40
KV = re.compile(r'(\w+)=("([^"]*)"|\S*)')
IMG = re.compile(r"!\[([^\]]*)\]\((figures/[A-Za-z0-9_./-]+\.png)\)")


def parse_log_line(line):
    """'ts=... level=info comp=reliefd pid=1 event=grant camp=3 ...' -> dict."""
    fields = {}
    for m in KV.finditer(line):
        fields[m.group(1)] = m.group(3) if m.group(3) is not None else m.group(2)
    return {"ts": fields.pop("ts", ""), "level": fields.pop("level", ""), "event": fields.pop("event", ""),
            "comp": fields.pop("comp", ""), "pid": fields.pop("pid", ""), "fields": fields, "line": line}


def proc_state(pid):
    """Process state letter from /proc/<pid>/stat (R, S, D, T, Z), or '-'."""
    try:
        with open(f"/proc/{int(pid)}/stat", "rb") as f:
            data = f.read(512).decode("ascii", "replace")
        return data[data.rindex(")") + 2]
    except (OSError, ValueError, IndexError):
        return "-"


def experiments():
    """[{section, caption, src}] for every figure in EXPERIMENTS.md, in order."""
    out, section = [], ""
    try:
        text = EXPERIMENTS_MD.read_text(encoding="utf-8")
    except OSError:
        return out
    for line in text.splitlines():
        if line.startswith("## "):
            section = line[3:].strip()
        for m in IMG.finditer(line):
            path = (ROOT / "docs" / m.group(2)).resolve()
            if path.is_relative_to(FIGURES) and path.is_file():
                out.append({"section": section, "caption": m.group(1), "src": "/" + m.group(2)})
    return out


class Job:
    """The one background simulation (reliefctl simulate flood)."""
    lock = threading.Lock()
    state = {"running": False, "rc": None, "output": "", "started": 0.0, "finished": 0.0}

    @classmethod
    def start(cls):
        with cls.lock:
            if cls.state["running"]:
                return False
            cls.state = {"running": True, "rc": None, "output": "", "started": time.time(), "finished": 0.0}
        threading.Thread(target=cls._run, daemon=True).start()
        return True

    @classmethod
    def _run(cls):
        argv = [str(CTL), "simulate", "flood", "--time-scale", "10"]
        try:
            p = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=600)
            rc, out = p.returncode, p.stdout + p.stderr
        except subprocess.TimeoutExpired:
            rc, out = -1, "simulation timed out after 600 s"
        with cls.lock:
            cls.state.update(running=False, rc=rc, output=out[-6000:], finished=time.time())

    @classmethod
    def snapshot(cls):
        with cls.lock:
            return dict(cls.state)


class Handler(BaseHTTPRequestHandler):
    server_version = "ReliefOS-web/1"
    protocol_version = "HTTP/1.1"

    # ---- helpers -----------------------------------------------------------------------------
    def log_message(self, fmt, *args):      # quiet: errors only
        pass

    PROXY_HEADERS = ("Cf-Connecting-Ip", "Cf-Ray", "X-Forwarded-For", "X-Forwarded-Host", "Forwarded")

    def is_local(self):
        """A request made on this machine: a local Host and no header a tunnel or proxy adds."""
        port = self.server.server_address[1]
        if self.headers.get("Host", "") not in (f"127.0.0.1:{port}", f"localhost:{port}"):
            return False
        return not any(self.headers.get(h) for h in self.PROXY_HEADERS)

    def allowed_host(self):
        # --public: anyone may look (read-only); otherwise only this machine (no DNS rebinding).
        return self.server.public or self.is_local()

    def send(self, code, body, ctype="application/json", extra=None):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    # ---- GET ----------------------------------------------------------------------------------
    def do_GET(self):
        if not self.allowed_host():
            return self.send(403, {"error": "forbidden host"})
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            try:
                return self.send(200, INDEX.read_bytes(), "text/html; charset=utf-8",
                                 {"Content-Security-Policy": "default-src 'self'; style-src 'unsafe-inline'; "
                                  "script-src 'unsafe-inline'; img-src 'self'"})
            except OSError:
                return self.send(500, {"error": "web/index.html missing"})
        if path == "/api/status":
            return self.status()
        if path == "/api/events":
            return self.events()
        if path == "/api/log":
            return self.log_poll()
        if path == "/api/job":
            return self.send(200, Job.snapshot())
        if path == "/api/experiments":
            return self.send(200, experiments())
        if path.startswith("/figures/") and path.endswith(".png"):
            target = (FIGURES / path[len("/figures/"):]).resolve()
            if target.is_relative_to(FIGURES) and target.is_file():
                return self.send(200, target.read_bytes(), "image/png", {"Cache-Control": "max-age=60"})
        return self.send(404, {"error": "not found"})

    def status(self):
        try:
            p = subprocess.run(WRAP + [str(BIN / "reliefstat"), "--json"], stdin=subprocess.DEVNULL,
                               capture_output=True, text=True, timeout=30)
            data = json.loads(p.stdout or '{"running": false}')
        except FileNotFoundError:
            return self.send(500, {"running": False, "error": f"{BIN / 'reliefstat'} not built; run make"})
        except (subprocess.TimeoutExpired, json.JSONDecodeError) as e:
            return self.send(503, {"running": False, "error": str(e)})
        for camp in data.get("camps", []):
            camp["proc_state"] = proc_state(camp.get("pid", 0)) if camp.get("state") in ("RUNNING", "STARTING") else "-"
        data["job_running"] = Job.snapshot()["running"]
        data["readonly"] = not self.is_local()
        return self.send(200, data)

    def log_poll(self):
        """Polling twin of events(): the log lines written since byte offset pos."""
        log = HOME / "logs" / "reliefd.log"
        q = parse_qs(urlparse(self.path).query)
        try:
            pos = int(q["pos"][0]) if "pos" in q else -1
        except ValueError:
            return self.send(400, {"error": "pos must be an integer"})
        try:
            size = log.stat().st_size
        except OSError:
            return self.send(200, {"pos": 0, "events": []})
        if pos > size:                              # a new run truncated or replaced the log
            pos = 0
        first = pos < 0
        start = max(0, size - 16384) if first else pos
        with open(log, "rb") as f:
            f.seek(start)
            chunk = f.read(min(size - start, 65536))
        lines = chunk.split(b"\n")
        tail = lines.pop()                          # an unfinished last line waits for next time
        if first and start > 0:
            lines = lines[1:]                       # we started mid-line
        if first:
            lines = lines[-LOG_TAIL:]
        events = []
        for raw in lines:
            line = raw.decode("utf-8", "replace").rstrip("\r")
            if line:
                events.append(parse_log_line(line))
        return self.send(200, {"pos": start + len(chunk) - len(tail), "events": events})

    def events(self):
        """Server-Sent Events: tail reliefd.log, one JSON object per line."""
        log = HOME / "logs" / "reliefd.log"
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        pos, partial, last_beat = 0, b"", time.monotonic()

        def emit(raw):
            line = raw.decode("utf-8", "replace").rstrip("\r")
            if line:
                self.wfile.write(b"data: " + json.dumps(parse_log_line(line)).encode() + b"\n\n")

        try:
            try:
                size = log.stat().st_size
                with open(log, "rb") as f:
                    f.seek(max(0, size - 16384))
                    lines = f.read().split(b"\n")
                pos = size
                for raw in (lines[1:] if size > 16384 else lines)[-LOG_TAIL - 1:]:
                    emit(raw)
            except OSError:
                pos = 0
            self.wfile.write(b": connected\n\n")
            self.wfile.flush()
            while not self.server.stopping:
                time.sleep(0.25)
                try:
                    size = log.stat().st_size
                except OSError:
                    size = 0
                if size < pos:                      # a new run truncated or replaced the log
                    pos, partial = 0, b""
                if size > pos:
                    with open(log, "rb") as f:
                        f.seek(pos)
                        chunk = f.read(size - pos)
                    pos = size
                    *complete, partial = (partial + chunk).split(b"\n")
                    for raw in complete:
                        emit(raw)
                    self.wfile.flush()
                if time.monotonic() - last_beat > 15:
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
                    last_beat = time.monotonic()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass                                    # the browser went away

    # ---- POST ---------------------------------------------------------------------------------
    def do_POST(self):
        if not self.allowed_host():
            return self.send(403, {"error": "forbidden host"})
        if not self.is_local():
            return self.send(403, {"ok": False, "error": "view only: ReliefOS is controlled from the presenter's machine"})
        port = self.server.server_address[1]
        origin = self.headers.get("Origin")
        if origin is not None and origin not in (f"http://127.0.0.1:{port}", f"http://localhost:{port}"):
            return self.send(403, {"error": "forbidden origin"})
        if urlparse(self.path).path != "/api/action":
            return self.send(404, {"error": "not found"})
        if not self.headers.get("Content-Type", "").startswith("application/json"):
            return self.send(415, {"error": "send application/json"})
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if length < 0 or length > MAX_BODY:
            return self.send(413, {"error": "body too large"})
        try:
            req = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            return self.send(400, {"error": "invalid JSON"})
        if not isinstance(req, dict):
            return self.send(400, {"error": "expected an object"})
        cmd = req.get("cmd")
        if cmd not in ACTIONS:
            return self.send(400, {"error": f"cmd must be one of {sorted(ACTIONS)}"})

        if cmd == "simulate":
            if not Job.start():
                return self.send(409, {"ok": False, "error": "a simulation is already running"})
            return self.send(202, {"ok": True, "output": "flood simulation started (reliefctl simulate flood)"})

        argv = [str(CTL), cmd]
        if cmd in ("alert", "dump"):
            camp = req.get("camp")
            if camp == "all":
                argv.append("all")
            elif isinstance(camp, int) and not isinstance(camp, bool) and 1 <= camp <= RELIEF_MAX_CAMPS:
                argv.append(str(camp))
            else:
                return self.send(400, {"error": f"camp must be 1-{RELIEF_MAX_CAMPS} or \"all\""})
        try:
            p = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
        except subprocess.TimeoutExpired:
            return self.send(504, {"ok": False, "error": f"reliefctl {cmd} timed out"})
        out = (p.stdout + p.stderr).strip()
        return self.send(200, {"ok": p.returncode == 0, "rc": p.returncode, "output": out[-4000:]})


RELIEF_MAX_CAMPS = 64       # RELIEF_SHM_CAMPS in include/relief.h


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    stopping = False
    public = False


def main():
    ap = argparse.ArgumentParser(description="ReliefOS web dashboard (127.0.0.1 only).")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--public", action="store_true",
                    help="answer any Host (for a tunnel); remote viewers are read-only")
    args = ap.parse_args()
    if not 1 <= args.port <= 65535:
        ap.error("--port must be 1-65535")
    try:
        srv = Server(("127.0.0.1", args.port), Handler)
    except OSError as e:
        print(f"reliefos web: cannot listen on 127.0.0.1:{args.port}: {e.strerror}", file=sys.stderr)
        return 1
    srv.public = args.public
    print(f"ReliefOS web dashboard: http://127.0.0.1:{args.port}/   (Ctrl-C to stop)", flush=True)
    print(f"  RELIEF_HOME={HOME}  binaries={BIN}", flush=True)
    if args.public:
        print("  --public: any Host is served; remote viewers are read-only, control stays on this machine",
              flush=True)
    try:
        srv.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        srv.stopping = True
        srv.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
