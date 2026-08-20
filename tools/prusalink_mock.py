#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mock PrusaLink server — a Core One stand-in for developing DragonBreath's `dc_prusa`
source WITHOUT a real printer (the Bambuddy pattern, for Prusa).

The response shapes and auth are taken verbatim from the Prusa-Firmware-Buddy source
(the Core One firmware):
  - GET /api/v1/status   -> lib/WUI/nhttp/status_renderer.cpp
  - GET /api/version     -> PrusaLink version block
  - Auth: X-Api-Key header on every /api/* request; the value is the printer's
    PrusaLink password. Missing/wrong -> 401. Plain HTTP, no TLS.
    (tests/integration/test_prusa_link.py, utils/gen-automata/http_server.py)

It also simulates a bed heat-soak: `temp_bed` ramps toward `target_bed` (and drifts to
ambient when the bed is off), so telemetry looks real. State + targets are drivable at
runtime through an unauthenticated control plane so you can script scenarios:

  # start it (port 80 needs sudo; use 8080 for quick local curl tests)
  python3 tools/prusalink_mock.py --api-key mysecret --port 8080

  # what dc_prusa will poll:
  curl -H 'X-Api-Key: mysecret' http://localhost:8080/api/v1/status
  curl http://localhost:8080/api/v1/status            # -> 401 (no key)

  # drive a print / heat-soak (control plane, no key needed):
  curl -X POST 'http://localhost:8080/mock/set?state=PRINTING&target_bed=60'
  curl -X POST 'http://localhost:8080/mock/set?state=FINISHED&target_bed=0'
  curl http://localhost:8080/mock                      # inspect sim state

HTTP/1.1 keep-alive is enabled so you can exercise dc_prusa's single-socket polling.
"""
import argparse
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# printer.state values, from src/state/printer_state.cpp printer_state::to_str
STATES = {"IDLE", "PRINTING", "PAUSED", "FINISHED", "STOPPED", "READY", "BUSY", "ATTENTION"}

# --- simulated printer state (guarded by a lock; ticked by a background thread) -------
_lock = threading.Lock()
sim = {
    "state": "IDLE",
    "temp_bed": 23.0,
    "target_bed": 0.0,
    "temp_nozzle": 23.0,
    "target_nozzle": 0.0,
    "ambient": 23.0,
    "axis_z": 5.0,
    # job fields (only meaningful while PRINTING/PAUSED)
    "job_id": 0,
    "progress": 0.0,
    "time_printing": 0,
    "time_remaining": 0,
}
API_KEY = "test-key"        # overridden by --api-key
BED_HEAT_RATE = 2.0         # C/s toward target when heating
NOZZLE_HEAT_RATE = 8.0      # C/s
COOL_RATE = 0.3             # C/s drift toward ambient when off
TICK_S = 0.5


def _approach(cur, target, rate, dt):
    """Move `cur` toward `target` by at most rate*dt."""
    step = rate * dt
    if cur < target:
        return min(cur + step, target)
    if cur > target:
        return max(cur - step, target)
    return cur


def _simulator():
    last = time.monotonic()
    while True:
        time.sleep(TICK_S)
        now = time.monotonic()
        dt = now - last
        last = now
        with _lock:
            amb = sim["ambient"]
            # bed
            if sim["target_bed"] > 0:
                sim["temp_bed"] = _approach(sim["temp_bed"], sim["target_bed"], BED_HEAT_RATE, dt)
            else:
                sim["temp_bed"] = _approach(sim["temp_bed"], amb, COOL_RATE, dt)
            # nozzle
            if sim["target_nozzle"] > 0:
                sim["temp_nozzle"] = _approach(sim["temp_nozzle"], sim["target_nozzle"], NOZZLE_HEAT_RATE, dt)
            else:
                sim["temp_nozzle"] = _approach(sim["temp_nozzle"], amb, COOL_RATE, dt)
            # job clock
            if sim["state"] in ("PRINTING",):
                sim["time_printing"] += int(round(dt))
                if sim["time_remaining"] > 0:
                    sim["time_remaining"] = max(0, sim["time_remaining"] - int(round(dt)))


def status_body():
    """Exact /api/v1/status shape from status_renderer.cpp (floats at 1 decimal)."""
    with _lock:
        printing = sim["state"] in ("PRINTING", "PAUSED")
        job = {}
        if printing:
            job = {
                "id": sim["job_id"],
                "progress": round(sim["progress"], 2),
                "time_remaining": sim["time_remaining"],
                "time_printing": sim["time_printing"],
            }
        return {
            "job": job,
            "storage": {"path": "/usb/", "name": "usb", "read_only": False},
            "transfer": {},
            "printer": {
                "state": sim["state"],
                "temp_bed": round(sim["temp_bed"], 1),
                "target_bed": round(sim["target_bed"], 1),
                "temp_nozzle": round(sim["temp_nozzle"], 1),
                "target_nozzle": round(sim["target_nozzle"], 1),
                "axis_z": round(sim["axis_z"], 1),
                "flow": 100,
                "speed": 100,
                "fan_hotend": 0,
                "fan_print": 0,
            },
        }


def version_body():
    # PrusaLink /api/version block (Core One-ish values; dc_prusa only needs it to exist)
    return {
        "api": "2.0.0",
        "server": "2.1.2",
        "nozzle_diameter": 0.40,
        "text": "PrusaLink",
        "hostname": "prusa-mock",
        "capabilities": {"upload-by-put": True},
    }


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"      # keep-alive, so dc_prusa can reuse one socket
    server_version = "PrusaLink-mock/1.0"

    def _send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _auth_ok(self):
        return self.headers.get("X-Api-Key") == API_KEY

    def _log(self, note):
        print(f"  {self.command} {self.path} -> {note}", flush=True)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/mock":                       # control plane: inspect (no auth)
            with _lock:
                self._send_json({"sim": dict(sim), "api_key": API_KEY})
            self._log("sim state")
            return
        if path.startswith("/api/"):
            if not self._auth_ok():
                self._send_json({"error": "Unauthorized"}, 401)
                self._log("401 (bad/missing X-Api-Key)")
                return
            if path == "/api/v1/status":
                self._send_json(status_body()); self._log("200 status"); return
            if path == "/api/version":
                self._send_json(version_body()); self._log("200 version"); return
            self._send_json({"error": "Not Found"}, 404); self._log("404"); return
        self._send_json({"error": "Not Found"}, 404); self._log("404")

    def do_POST(self):
        path = urlparse(self.path).path
        if path == "/mock/set":                   # control plane: drive the sim (no auth)
            q = parse_qs(urlparse(self.path).query)
            # also accept a JSON body
            n = int(self.headers.get("Content-Length", 0) or 0)
            if n:
                try:
                    q.update({k: [v] for k, v in json.loads(self.rfile.read(n)).items()})
                except Exception:
                    pass
            applied = {}
            with _lock:
                for key in ("state", "target_bed", "temp_bed", "target_nozzle",
                            "temp_nozzle", "ambient", "progress", "time_remaining",
                            "job_id", "axis_z"):
                    if key in q:
                        val = q[key][0]
                        if key == "state":
                            val = str(val).upper()
                            if val not in STATES:
                                self._send_json({"error": f"bad state; use {sorted(STATES)}"}, 400)
                                self._log("400 bad state"); return
                            sim[key] = val
                        elif key in ("time_remaining", "job_id"):
                            sim[key] = int(float(val))
                        else:
                            sim[key] = float(val)
                        applied[key] = sim[key]
            self._send_json({"ok": True, "applied": applied})
            self._log(f"set {applied}")
            return
        self._send_json({"error": "Not Found"}, 404); self._log("404")

    def log_message(self, *a):        # silence the default noisy logger; we print our own
        pass


def main():
    global API_KEY
    ap = argparse.ArgumentParser(description="Mock PrusaLink server (Core One stand-in).")
    ap.add_argument("--host", default="0.0.0.0", help="bind address (default: all interfaces)")
    ap.add_argument("--port", type=int, default=8080,
                    help="port (default 8080; real PrusaLink is 80 — needs sudo)")
    ap.add_argument("--api-key", default="test-key", help="X-Api-Key value clients must send")
    ap.add_argument("--state", default="IDLE", choices=sorted(STATES), help="initial state")
    ap.add_argument("--target-bed", type=float, default=0.0, help="initial bed setpoint")
    ap.add_argument("--ambient", type=float, default=23.0, help="ambient temp for cooldown")
    args = ap.parse_args()

    API_KEY = args.api_key
    with _lock:
        sim["state"] = args.state
        sim["target_bed"] = args.target_bed
        sim["ambient"] = args.ambient
        sim["temp_bed"] = args.ambient
        sim["temp_nozzle"] = args.ambient

    threading.Thread(target=_simulator, daemon=True).start()
    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Mock PrusaLink on http://{args.host}:{args.port}  (X-Api-Key: {API_KEY!r})")
    print( "  GET  /api/v1/status   (auth)   GET /api/version (auth)")
    print( "  POST /mock/set?state=PRINTING&target_bed=60   GET /mock")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    main()
