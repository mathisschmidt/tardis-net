"""Run the real firmware against the real console, on this machine.

``src/*.cpp`` is compiled for the host against the shims in ``test/host/shim``
and started as a process; ``server-app`` is started as a second process. The
test then does what an operator does — claim the portal, type the console
address and key, test, save — and finally asserts that a command queued on the
console makes the firmware hold the switch for the documented time.

    make e2e          # or: pytest test/host -k firmware

Nothing here is a re-implementation: the portal, the config store and the agent
under test are the same translation units that get flashed.
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
REPO = ROOT.parent
SERVER_APP = REPO / "server-app"
BINARY = ROOT / ".build" / "firmware-host"

pytest.importorskip("fastapi", reason="server-app dependencies are not installed")
pyotp = pytest.importorskip("pyotp")

PORTAL_PASSWORD = "correct-horse"
PULSE_MS = {"power_on": 500, "graceful_shutdown": 500, "hard_power_off": 5000}


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def wait_for(predicate, timeout: float = 20.0, interval: float = 0.2, what: str = "condition"):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(interval)
    raise AssertionError(f"timed out waiting for {what}")


class Http:
    """Minimal cookie-keeping HTTP client (no third-party dependency)."""

    def __init__(self, base: str) -> None:
        import http.cookiejar
        import urllib.request

        self.base = base
        self.jar = http.cookiejar.CookieJar()
        # No proxy: everything here is on loopback, and a configured HTTP proxy
        # would swallow it.
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}), urllib.request.HTTPCookieProcessor(self.jar)
        )

    def request(self, method: str, path: str, payload=None, form=None, headers=None):
        import urllib.error
        import urllib.parse
        import urllib.request

        data, head = None, dict(headers or {})
        if payload is not None:
            data = json.dumps(payload).encode()
            head["Content-Type"] = "application/json"
        elif form is not None:
            data = urllib.parse.urlencode(form).encode()
            head["Content-Type"] = "application/x-www-form-urlencoded"

        request = urllib.request.Request(self.base + path, data=data, headers=head, method=method)
        try:
            with self.opener.open(request, timeout=20) as response:
                body = response.read().decode()
                return response.status, (json.loads(body) if body.strip().startswith(("{", "[")) else body)
        except urllib.error.HTTPError as error:
            body = error.read().decode()
            return error.code, (json.loads(body) if body.strip().startswith(("{", "[")) else body)
        except (urllib.error.URLError, OSError):
            # Not up yet — the callers poll, so this is a "not ready", not a crash.
            return 0, None

    def get(self, path, **kwargs):
        return self.request("GET", path, **kwargs)

    def post(self, path, **kwargs):
        return self.request("POST", path, **kwargs)


@pytest.fixture(scope="module")
def firmware_binary() -> Path:
    """Compile src/*.cpp for the host, exactly as `make host` does."""
    if not shutil.which("g++"):
        pytest.skip("g++ is needed to build the firmware for the host")
    subprocess.run(["python", "tools/build_web_assets.py"], cwd=ROOT, check=True)
    BINARY.parent.mkdir(parents=True, exist_ok=True)
    build = subprocess.run(  # noqa: PLW1510 — the assert below is the check
        ["g++", "-std=c++17", "-O1", "-Iinclude", "-Isrc", "-Itest/host/shim",
         "-o", str(BINARY), "test/host/shim/host_runtime.cpp",
         *[str(p) for p in sorted((ROOT / "src").glob("*.cpp"))]],
        cwd=ROOT, capture_output=True, text=True,
    )
    assert build.returncode == 0, f"host build failed:\n{build.stderr}"
    return BINARY


@pytest.fixture()
def console(tmp_path):
    """A real server-app process with an enrolled operator."""
    port = free_port()
    state = tmp_path / "state.json"
    environment = {
        **os.environ,
        "TARDIS_STATE_FILE": str(state),
        "TARDIS_HARDWARE_POLL_SECONDS": "1",
        "PYTHONPATH": str(SERVER_APP),
    }
    process = subprocess.Popen(
        [sys.executable, "-m", "uvicorn", "app.server.app:app",
         "--host", "127.0.0.1", "--port", str(port), "--log-level", "warning"],
        cwd=SERVER_APP, env=environment,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    client = Http(f"http://127.0.0.1:{port}")
    try:
        wait_for(lambda: client.get("/api/health")[0] == 200, what="the console to start")

        # Enrol the operator the way the login page does.
        client.get("/login")
        secret = json.loads(state.read_text())["pending_secret"]
        status, _ = client.post("/login", form={"code": pyotp.TOTP(secret).now()})
        assert status == 204

        client.get("/")  # rendering the dashboard mints the pairing key
        client.port = port
        client.state_file = state
        client.key = json.loads(state.read_text())["pairing_key"]
        yield client
    finally:
        process.terminate()
        process.wait(timeout=10)


@pytest.fixture()
def device(firmware_binary, tmp_path):
    """The real firmware, running as a local process."""
    port = free_port()
    home = tmp_path / "device"
    home.mkdir()
    log = home / "serial.log"
    handle = log.open("w")
    process = subprocess.Popen(
        [str(firmware_binary)],
        cwd=home,
        env={**os.environ, "TARDIS_HOST_STATE_DIR": str(home), "TARDIS_PORTAL_PORT": str(port)},
        stdout=handle, stderr=subprocess.STDOUT,
    )
    client = Http(f"http://127.0.0.1:{port}")
    try:
        wait_for(lambda: client.get("/api/status")[0] == 200, what="the device portal to start")
        client.log = log
        client.serial = lambda: log.read_text()
        yield client
    finally:
        process.terminate()
        process.wait(timeout=10)
        handle.close()


def pulses(serial_text: str) -> list[tuple[int, int, str]]:
    """(millis, pin, level) for every GPIO change the firmware made."""
    events = []
    for line in serial_text.splitlines():
        if not line.startswith("[gpio]"):
            continue
        parts = line.split()
        events.append((int(parts[1].rstrip("ms")), int(parts[3]), parts[5]))
    return events


def configure(device, console, *, save: bool = True):
    """Claim the portal and enter the console's details, as an operator would."""
    status, _ = device.post(
        "/api/claim", payload={"password": PORTAL_PASSWORD, "confirm": PORTAL_PASSWORD}
    )
    assert status == 200
    if save:
        status, body = device.post("/api/config", payload={
            "wifi_ssid": "home-network",
            "wifi_password": "hunter2hunter2",
            "server_url": console.base,
            "api_key": console.key,
            "poll_seconds": 1,
            "switch_pin": 26,
            "switch_active_high": True,
            "sense_pin": -1,
            "sense_active_high": True,
        })
        assert status == 200, body


def test_test_button_uses_what_is_typed_not_only_what_is_saved(device, console):
    """Regression: pressing Test before the first save must actually test.

    It used to answer "fill in the address and key first" while both were on
    screen, because it only ever looked at the stored config.
    """
    configure(device, console, save=False)

    status, body = device.post(
        "/api/test", payload={"server_url": console.base, "api_key": console.key}
    )
    assert status == 200, body
    assert "Connected" in body["message"], body

    # A wrong key must be reported as such, not as a connection failure.
    status, body = device.post(
        "/api/test", payload={"server_url": console.base, "api_key": "not-the-key"}
    )
    assert status == 502
    assert "rejected" in body["message"].lower(), body


def test_device_links_to_the_console_after_configuration(device, console):
    configure(device, console)

    link = wait_for(
        lambda: (device.get("/api/status")[1].get("link") or {}).get("linked"),
        what="the device to poll the console",
    )
    assert link is True

    hardware = console.get("/api/hardware")[1]
    assert hardware["linked"] is True
    assert hardware["firmware"] == "1.0.0"


@pytest.mark.parametrize(
    "action,wire",
    [("on", "power_on"), ("hard_off", "hard_power_off")],
)
def test_a_console_command_pulses_the_switch(device, console, action, wire):
    """The whole point: a press on the console holds the real pin, the real time."""
    configure(device, console)
    wait_for(lambda: (device.get("/api/status")[1].get("link") or {}).get("linked"),
             what="the device to link")

    if action == "hard_off":
        # hard_off is only offered while the machine is on; get it there first.
        console.post("/api/machine/power", payload={"action": "on"})
        wait_for(lambda: console.get("/api/machine")[1]["status"] == "online",
                 what="the machine to come online")

    before = len(pulses(device.serial()))

    def both_edges():
        seen = pulses(device.serial())
        return seen if len(seen) >= before + 2 else None

    status, _ = console.post("/api/machine/power", payload={"action": action})
    assert status == 200

    expected = PULSE_MS[wire]
    events = wait_for(both_edges, timeout=30, what="the switch to pulse")
    closed, released = events[before], events[before + 1]
    assert closed[2] == "HIGH" and released[2] == "LOW"
    assert closed[1] == 26
    held = released[0] - closed[0]
    assert abs(held - expected) < 250, f"held {held}ms, expected ~{expected}ms"

    # And the console only moves once the device has acked.
    settled = "online" if wire == "power_on" else "offline"
    wait_for(lambda: console.get("/api/machine")[1]["status"] == settled,
             what=f"the console to report {settled}")
    assert console.get("/api/hardware")[1]["commands_ok"] >= 1


def test_the_switch_is_released_at_boot_before_anything_else(device, console):
    """A reset must not look like a button press to the machine."""
    events = pulses(device.serial())
    assert events, "no GPIO activity logged at all"
    assert events[0][2] == "LOW", f"first GPIO write was {events[0]}"


def test_portal_requires_the_password_after_claiming(device, console):
    configure(device, console, save=False)
    anonymous = Http(device.base)
    assert anonymous.get("/api/config")[0] == 401
    assert anonymous.post("/api/test", payload={})[0] == 401
    assert anonymous.get("/api/wifi/scan")[0] == 401
    # The claim endpoint closes behind itself.
    assert anonymous.post(
        "/api/claim", payload={"password": "another-one", "confirm": "another-one"}
    )[0] == 409

    status, _ = anonymous.post("/api/login", payload={"password": PORTAL_PASSWORD})
    assert status == 200
    assert anonymous.get("/api/config")[0] == 200
