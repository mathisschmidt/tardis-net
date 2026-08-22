"""Run the device portal on your laptop, without an ESP32.

Serves ``web/portal.html`` and re-implements the firmware's own HTTP API
(``src/portal.cpp``) in Python: the same routes, the same status codes, the
same claim-then-login rule. Useful for working on the UI, and it is what the
portal's browser test drives.

    python tools/mock_device.py --port 8090
    python tools/mock_device.py --port 8090 --claimed   # pretend it is set up

It talks to a real tardis-net server when you press "Test connection", so it
also doubles as a way to check a pairing key by hand.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import secrets
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PORTAL_HTML = ROOT / "web" / "portal.html"

FIRMWARE = "1.0.0"
SESSION_SECONDS = 30 * 60
MAX_ATTEMPTS = 5
LOCKOUT_SECONDS = 300
STARTED = time.time()


class Device:
    """The bits of device state the portal can see."""

    def __init__(self, claimed: bool = False) -> None:
        self.password_salt = ""
        self.password_hash = ""
        self.session: str | None = None
        self.session_expires = 0.0
        self.failed_attempts = 0
        self.locked_until = 0.0
        self.ap_password = secrets.token_hex(5)
        self.config = {
            "wifi_ssid": "",
            "wifi_password": "",
            "server_url": "",
            "api_key": "",
            "poll_seconds": 5,
            "switch_pin": 26,
            "switch_active_high": True,
            "sense_pin": -1,
            "sense_active_high": True,
        }
        self.link = {
            "polled": False,
            "linked": False,
            "http_status": 0,
            "poll_interval": 5,
            "machine_status": "",
            "last_action": "",
            "commands_ok": 0,
            "commands_failed": 0,
            "last_error": "",
            "seconds_since_poll": 0,
            "busy": False,
        }
        if claimed:
            self.set_password("test-password")
            self.config.update(
                wifi_ssid="home-network",
                wifi_password="stored-secret",
                server_url="http://192.168.1.10:8000",
                api_key="xCVEMxCqquaIsROK",
            )

    # The firmware iterates SHA-256; the mock only needs to be consistent.
    def _hash(self, password: str) -> str:
        return hashlib.sha256((self.password_salt + password).encode()).hexdigest()

    @property
    def claimed(self) -> bool:
        return bool(self.password_hash)

    def set_password(self, password: str) -> None:
        self.password_salt = secrets.token_hex(16)
        self.password_hash = self._hash(password)

    def check_password(self, password: str) -> bool:
        return bool(self.password_hash) and secrets.compare_digest(
            self.password_hash, self._hash(password)
        )

    @property
    def configured(self) -> bool:
        config = self.config
        return bool(config["wifi_ssid"] and config["server_url"] and config["api_key"])

    def locked_for(self) -> int:
        return max(0, int(self.locked_until - time.time()))

    def register_failure(self) -> None:
        self.failed_attempts += 1
        if self.failed_attempts >= MAX_ATTEMPTS:
            self.failed_attempts = 0
            self.locked_until = time.time() + LOCKOUT_SECONDS

    def start_session(self) -> str:
        self.session = secrets.token_hex(16)
        self.session_expires = time.time() + SESSION_SECONDS
        self.failed_attempts = 0
        self.locked_until = 0
        return self.session

    def valid_session(self, token: str | None) -> bool:
        if not token or not self.session:
            return False
        if time.time() > self.session_expires:
            return False
        return secrets.compare_digest(token, self.session)


def validate_config(config: dict) -> str | None:
    """Same rules as include/device_config.h."""
    if not config["wifi_ssid"]:
        return "Wi-Fi network is required."
    if config["wifi_password"] and len(config["wifi_password"]) < 8:
        return "Wi-Fi password must be at least 8 characters (or empty for an open network)."
    url = config["server_url"]
    if not url:
        return "Server address is required."
    if not url.startswith(("http://", "https://")):
        return "Server address must start with http:// or https://."
    if not config["api_key"]:
        return "API key is required — copy it from the console's dashboard."
    if not 1 <= config["poll_seconds"] <= 3600:
        return "Poll interval must be between 1 and 3600 seconds."
    pin = config["switch_pin"]
    if not (0 <= pin <= 33) or 6 <= pin <= 11:
        return "Switch GPIO must be an output-capable pin (0-33, not 6-11)."
    sense = config["sense_pin"]
    if sense >= 0:
        if not (0 <= sense <= 39) or 6 <= sense <= 11:
            return "Sense GPIO must be a usable input pin (0-39, not 6-11)."
        if sense == pin:
            return "Sense GPIO must differ from the switch GPIO."
    return None


def mask_hint(secret: str) -> str:
    if not secret:
        return ""
    return "••••" if len(secret) <= 4 else "••••••••" + secret[-4:]


class Handler(BaseHTTPRequestHandler):
    device: Device

    def log_message(self, fmt, *args):  # quieter than the default
        if self.path != "/api/status":
            print(f"[mock] {self.command} {self.path} -> {args[1]}")

    # ------------------------------------------------------------- plumbing

    def _json(self, code: int, payload: dict, cookie: str | None = None) -> None:
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if cookie is not None:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        self.wfile.write(body)

    def _error(self, code: int, detail: str) -> None:
        self._json(code, {"ok": False, "detail": detail})

    def _body(self) -> dict:
        length = int(self.headers.get("Content-Length") or 0)
        if not length:
            return {}
        try:
            return json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            return {}

    def _session_token(self) -> str | None:
        for part in (self.headers.get("Cookie") or "").split(";"):
            name, _, value = part.strip().partition("=")
            if name == "tardis_portal":
                return value
        return None

    def _authed(self) -> bool:
        return self.device.valid_session(self._session_token())

    def _require_auth(self) -> bool:
        if self._authed():
            return True
        self._error(401, "Sign in first.")
        return False

    # --------------------------------------------------------------- routes

    def do_GET(self) -> None:
        if self.path == "/" or self.path.startswith("/index"):
            html = PORTAL_HTML.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.end_headers()
            self.wfile.write(html)
        elif self.path == "/api/status":
            self._status()
        elif self.path == "/api/config":
            if self._require_auth():
                self._get_config()
        else:
            self.send_response(302)
            self.send_header("Location", "/")
            self.end_headers()

    def do_POST(self) -> None:
        routes = {
            "/api/claim": self._claim,
            "/api/login": self._login,
            "/api/logout": self._logout,
            "/api/config": self._save_config,
            "/api/test": self._test,
            "/api/reboot": self._reboot,
        }
        handler = routes.get(self.path)
        if handler is None:
            self._error(404, "Unknown endpoint.")
            return
        handler()

    def _status(self) -> None:
        device = self.device
        signed_in = self._authed()
        payload = {
            "claimed": device.claimed,
            "authenticated": signed_in,
            "configured": device.configured,
            "locked_for": device.locked_for(),
            "firmware": FIRMWARE,
            "ap_mode": not device.configured,
            "wifi": {
                "connected": device.configured,
                "ssid": device.config["wifi_ssid"],
                "ip": "192.168.1.50" if device.configured else "192.168.4.1",
                "rssi": -57,
            },
            "uptime_s": int(time.time() - STARTED),
        }
        if signed_in:
            payload["link"] = device.link
        self._json(200, payload)

    def _claim(self) -> None:
        device = self.device
        if device.claimed:
            self._error(409, "This device already has a portal password.")
            return
        body = self._body()
        password = body.get("password") or ""
        confirm = body.get("confirm") or ""
        if len(password) < 8:
            self._error(400, "Password must be at least 8 characters.")
            return
        if password != confirm:
            self._error(400, "The two passwords do not match.")
            return
        device.set_password(password)
        token = device.start_session()
        self._json(200, {"ok": True}, cookie=f"tardis_portal={token}; Path=/; HttpOnly; SameSite=Lax")

    def _login(self) -> None:
        device = self.device
        if not device.claimed:
            self._error(409, "This device has no password yet — set one first.")
            return
        if device.locked_for():
            self._error(429, f"Too many attempts. Try again in {device.locked_for()}s.")
            return
        if not device.check_password(self._body().get("password") or ""):
            device.register_failure()
            self._error(401, "Wrong password.")
            return
        token = device.start_session()
        self._json(200, {"ok": True}, cookie=f"tardis_portal={token}; Path=/; HttpOnly; SameSite=Lax")

    def _logout(self) -> None:
        self.device.session = None
        self._json(200, {"ok": True}, cookie="tardis_portal=; Path=/; HttpOnly; Max-Age=0")

    def _get_config(self) -> None:
        config = self.device.config
        self._json(
            200,
            {
                "wifi_ssid": config["wifi_ssid"],
                "wifi_password_set": bool(config["wifi_password"]),
                "server_url": config["server_url"],
                "api_key_hint": mask_hint(config["api_key"]),
                "api_key_set": bool(config["api_key"]),
                "poll_seconds": config["poll_seconds"],
                "switch_pin": config["switch_pin"],
                "switch_active_high": config["switch_active_high"],
                "sense_pin": config["sense_pin"],
                "sense_active_high": config["sense_active_high"],
                "ap_password": self.device.ap_password,
            },
        )

    def _save_config(self) -> None:
        if not self._require_auth():
            return
        body = self._body()
        config = dict(self.device.config)
        config["wifi_ssid"] = body.get("wifi_ssid", config["wifi_ssid"])
        config["server_url"] = (body.get("server_url") or config["server_url"]).rstrip("/").strip()
        config["poll_seconds"] = int(body.get("poll_seconds", config["poll_seconds"]))
        config["switch_pin"] = int(body.get("switch_pin", config["switch_pin"]))
        config["switch_active_high"] = bool(body.get("switch_active_high", config["switch_active_high"]))
        config["sense_pin"] = int(body.get("sense_pin", config["sense_pin"]))
        config["sense_active_high"] = bool(body.get("sense_active_high", config["sense_active_high"]))
        # Blank secret fields keep whatever is stored — same rule as the firmware.
        if body.get("wifi_password"):
            config["wifi_password"] = body["wifi_password"]
        if body.get("api_key"):
            config["api_key"] = body["api_key"]

        problem = validate_config(config)
        if problem:
            self._error(400, problem)
            return
        wifi_changed = (
            config["wifi_ssid"] != self.device.config["wifi_ssid"]
            or config["wifi_password"] != self.device.config["wifi_password"]
        )
        self.device.config = config
        self._json(200, {"ok": True, "wifi_changed": wifi_changed})

    def _test(self) -> None:
        if not self._require_auth():
            return
        config = self.device.config
        if not self.device.configured:
            self._json(502, {"ok": False, "message": "Fill in the server address and API key first."})
            return
        request = urllib.request.Request(
            config["server_url"] + "/api/hardware/poll",
            data=json.dumps({"firmware": FIRMWARE, "uptime_s": int(time.time() - STARTED)}).encode(),
            headers={"Content-Type": "application/json", "X-Tardis-Key": config["api_key"]},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=8) as response:
                payload = json.loads(response.read())
            status = payload.get("machine", {}).get("status", "no status")
            self.device.link.update(polled=True, linked=True, http_status=200, machine_status=status)
            self._json(200, {"ok": True, "message": f"Connected. {config['server_url']} reports {status}."})
        except urllib.error.HTTPError as exc:
            message = ("The server rejected this API key." if exc.code == 401
                       else f"The server replied {exc.code}.")
            self.device.link.update(polled=True, linked=False, http_status=exc.code, last_error=message)
            self._json(502, {"ok": False, "message": message})
        except Exception as exc:  # noqa: BLE001 — any transport failure reads the same
            message = f"Could not reach {config['server_url']}. ({exc})"
            self.device.link.update(polled=True, linked=False, last_error=message)
            self._json(502, {"ok": False, "message": message})

    def _reboot(self) -> None:
        if not self._require_auth():
            return
        print("[mock] reboot requested (no-op)")
        self._json(200, {"ok": True})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument("--claimed", action="store_true", help="start already claimed + configured")
    args = parser.parse_args()

    Handler.device = Device(claimed=args.claimed)
    server = HTTPServer(("127.0.0.1", args.port), Handler)
    print(f"device portal mock on http://127.0.0.1:{args.port}/")
    if not args.claimed:
        print("unclaimed — the portal will ask you to set a password")
    server.serve_forever()


if __name__ == "__main__":
    main()
