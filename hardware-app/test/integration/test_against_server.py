"""End-to-end check that the firmware's flow and the console's flow line up.

Runs the real server-app in-process and drives it with a Python device that
follows exactly what ``src/agent.cpp`` does — poll, hold the switch for
``pulse_ms``, ack — asserting the hold times from the table in both READMEs:

    power on            500 ms
    graceful shutdown   500 ms
    hard power off      5000 ms

    pytest hardware-app/test/integration        # needs server-app's dev deps

If the two sides ever drift — a renamed action, a moved header, a changed
status code — this is what fails.
"""

from __future__ import annotations

import importlib

import pytest

# conftest.py puts server-app on sys.path before this import runs.
pytest.importorskip("fastapi", reason="server-app dependencies are not installed")
pyotp = pytest.importorskip("pyotp")
from fastapi.testclient import TestClient

# Mirrors include/protocol.h
EXPECTED_PULSE_MS = {
    "power_on": 500,
    "graceful_shutdown": 500,
    "hard_power_off": 5000,
}
KEY_HEADER = "X-Tardis-Key"
POLL_PATH = "/api/hardware/poll"
ACK_PATH = "/api/hardware/ack"
FIRMWARE = "1.0.0"


class FakeDevice:
    """The firmware's agent loop, minus the GPIO and the network."""

    def __init__(self, client: TestClient, key: str) -> None:
        self.client = client
        self.key = key
        self.switch_closed = False
        self.pulses: list[tuple[str, int]] = []   # (action, hold in ms)
        self.commands_ok = 0

    def _headers(self) -> dict[str, str]:
        return {KEY_HEADER: self.key}

    def poll(self, power_sense: bool | None = None) -> dict:
        # Mirrors buildPollBody: always sent, "unknown" from a device with no
        # sense pin fitted rather than the field being left out.
        wire_sense = "unknown" if power_sense is None else ("on" if power_sense else "off")
        body = {
            "firmware": FIRMWARE,
            "ip": "192.168.1.50",
            "rssi": -57,
            "uptime_s": 42,
            "power_sense": wire_sense,
        }
        response = self.client.post(POLL_PATH, json=body, headers=self._headers())
        assert response.status_code == 200, response.text
        return response.json()

    def run_once(self, power_sense: bool | None = None) -> str | None:
        """One iteration of loop(): poll, pulse if told to, ack. Returns the action."""
        payload = self.poll(power_sense)
        command = payload.get("command")
        if not command:
            return None

        action = command["action"]
        assert action in EXPECTED_PULSE_MS, f"unknown action {action}"
        assert command["pulse_ms"] == EXPECTED_PULSE_MS[action]

        # Hold the switch for exactly as long as the server asked.
        self.switch_closed = True
        self.pulses.append((action, command["pulse_ms"]))
        self.switch_closed = False

        ack = self.client.post(
            ACK_PATH,
            json={"id": command["id"], "status": "completed"},
            headers=self._headers(),
        )
        assert ack.status_code == 200, ack.text
        self.commands_ok += 1
        return action


@pytest.fixture()
def stack(tmp_path, monkeypatch):
    """A fresh console with an enrolled operator, plus its paired device."""
    monkeypatch.setenv("TARDIS_STATE_FILE", str(tmp_path / "state.json"))
    monkeypatch.setenv("TARDIS_ACK_TIMEOUT_SECONDS", "60")

    from app.core import config, machine, runtime, storage
    from app.server import dependencies, templating
    from app.server.routers import api, pages

    for module in (config, storage, machine):
        importlib.reload(module)
    importlib.reload(runtime)
    for module in (templating, dependencies, api, pages):
        importlib.reload(module)
    server = importlib.reload(importlib.import_module("app.server.app"))

    with TestClient(server.app) as client:
        # Enrol the operator the way the login page does.
        client.get("/login")
        secret = dependencies.store.raw("pending_secret")
        client.post("/login", data={"code": pyotp.TOTP(secret).now()})
        device = FakeDevice(client, dependencies.machine.pairing_key)
        yield client, device


def test_full_power_cycle_over_the_real_api(stack):
    console, device = stack

    # Nothing to do while the operator is idle.
    assert device.run_once() is None

    console.post("/api/machine/power", json={"action": "on"})
    assert device.run_once() == "power_on"
    # The ack alone never confirms it — only a sense-line poll does.
    assert console.get("/api/machine").json()["status"] != "online"
    device.poll(power_sense=True)
    assert console.get("/api/machine").json()["status"] == "online"

    console.post("/api/machine/power", json={"action": "off"})
    assert device.run_once() == "graceful_shutdown"
    device.poll(power_sense=False)
    assert console.get("/api/machine").json()["status"] == "offline"

    console.post("/api/machine/power", json={"action": "on"})
    device.run_once()
    device.poll(power_sense=True)
    console.post("/api/machine/power", json={"action": "hard_off"})
    assert device.run_once() == "hard_power_off"
    device.poll(power_sense=False)
    assert console.get("/api/machine").json()["status"] == "offline"

    assert device.pulses == [
        ("power_on", 500),
        ("graceful_shutdown", 500),
        ("power_on", 500),
        ("hard_power_off", 5000),
    ]
    assert not device.switch_closed
    assert console.get("/api/hardware").json()["commands_ok"] == 4


def test_device_details_reach_the_console(stack):
    console, device = stack
    device.poll()
    hardware = console.get("/api/hardware").json()
    assert hardware["linked"] is True
    assert hardware["firmware"] == FIRMWARE
    assert hardware["ip"] == "192.168.1.50"


def test_console_waits_for_the_device(stack):
    """Between the press and the ack, the console must not claim success — and
    the ack itself is not the last word either; only the sense line is."""
    console, device = stack
    console.post("/api/machine/power", json={"action": "on"})

    state = console.get("/api/machine").json()
    assert state["status"] == "booting"
    assert state["transitioning"] is True
    assert state["is_on"] is False

    device.poll()  # command delivered, not yet acked
    assert console.get("/api/machine").json()["is_on"] is False

    device.run_once()
    assert console.get("/api/machine").json()["is_on"] is False  # ack still isn't enough

    device.poll(power_sense=True)
    assert console.get("/api/machine").json()["is_on"] is True


def test_a_device_with_the_wrong_key_gets_nowhere(stack):
    console, device = stack
    console.post("/api/machine/power", json={"action": "on"})

    impostor = FakeDevice(device.client, "not-the-pairing-key")
    assert impostor.client.post(POLL_PATH, json={}, headers={KEY_HEADER: impostor.key}).status_code == 401
    assert (
        impostor.client.post(
            ACK_PATH, json={"id": "x"}, headers={KEY_HEADER: impostor.key}
        ).status_code
        == 401
    )
    # The real command is untouched and still waiting.
    assert device.run_once() == "power_on"


def test_power_sense_keeps_the_console_honest(stack):
    """Someone presses the physical button; the console follows the sense line."""
    console, device = stack
    device.poll(power_sense=True)
    assert console.get("/api/machine").json()["status"] == "online"
    device.poll(power_sense=False)
    assert console.get("/api/machine").json()["status"] == "offline"


def test_the_device_never_sees_the_operator_api(stack):
    """The pairing key is for the two hardware routes and nothing else."""
    _, device = stack
    headers = {KEY_HEADER: device.key}
    device.client.cookies.clear()
    for method, path in [
        ("GET", "/api/state"),
        ("GET", "/api/machine"),
        ("GET", "/api/hardware"),
        ("POST", "/api/machine/power"),
        ("GET", "/api/preferences"),
    ]:
        response = device.client.request(method, path, json={}, headers=headers)
        assert response.status_code == 401, f"{method} {path} accepted the pairing key"
