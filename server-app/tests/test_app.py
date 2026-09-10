"""End-to-end checks over the real ASGI app with an isolated state file."""

from __future__ import annotations

import importlib
import time

import pyotp
import pytest
from fastapi.testclient import TestClient


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("TARDIS_STATE_FILE", str(tmp_path / "state.json"))
    monkeypatch.setenv("TARDIS_ACK_TIMEOUT_SECONDS", "60")

    # The modules read settings at import time, so reload them per test.
    from app.core import config, machine, runtime, storage
    from app.server import dependencies, templating
    from app.server.routers import api, pages

    for module in (config, storage, machine):
        importlib.reload(module)
    importlib.reload(runtime)
    for module in (templating, dependencies, api, pages):
        importlib.reload(module)
    server_app = importlib.reload(importlib.import_module("app.server.app"))

    with TestClient(server_app.app) as test_client:
        test_client.deps = dependencies  # type: ignore[attr-defined]
        yield test_client


def code_for(secret: str, at: float | None = None) -> str:
    return pyotp.TOTP(secret).at(at or time.time())


def shift_clock(monkeypatch, seconds: int) -> float:
    """Move the server's clock forward so a later TOTP step is the current one."""
    from app.core import auth as auth_module

    target = time.time() + seconds
    monkeypatch.setattr(auth_module, "_now", lambda: int(target))
    return target


def key_headers(client) -> dict[str, str]:
    """Headers a paired device sends — the pairing key, never a session cookie."""
    return {"X-Tardis-Key": client.deps.machine.pairing_key}


def poll(client, **report) -> dict:
    """One hardware heartbeat, returning the decoded poll response."""
    response = client.post("/api/hardware/poll", json=report, headers=key_headers(client))
    assert response.status_code == 200, response.text
    return response.json()


def ack(client, command_id: str, *, status: str = "completed", detail: str | None = None) -> dict:
    response = client.post(
        "/api/hardware/ack",
        json={"id": command_id, "status": status, "detail": detail},
        headers=key_headers(client),
    )
    assert response.status_code == 200, response.text
    return response.json()


def enrol(client) -> str:
    """Walk the enrolment flow and return the shared secret."""
    response = client.get("/login")
    assert response.status_code == 200
    secret = client.deps.store.raw("pending_secret")
    assert secret

    response = client.post("/login", data={"code": code_for(secret)})
    assert response.status_code == 204
    assert response.headers["HX-Redirect"] == "/"
    return secret


# ------------------------------------------------------------------ pages


def test_root_redirects_to_login_when_anonymous(client):
    response = client.get("/", follow_redirects=False)
    assert response.status_code == 303
    assert response.headers["location"] == "/login"


def test_login_page_offers_enrolment_first(client):
    body = client.get("/login").text
    assert "Scan with your authenticator" in body
    assert "data:image/svg+xml;base64," in body


def test_login_page_hides_qr_once_enrolled(client):
    enrol(client)
    client.cookies.clear()
    body = client.get("/login").text
    assert "Scan with your authenticator" not in body
    assert "Access code" in body


def test_dashboard_renders_after_login(client):
    enrol(client)
    body = client.get("/").text
    assert "remote machine" in body
    assert "power-btn" in body


def test_wrong_code_is_rejected_with_the_form(client):
    client.get("/login")
    response = client.post("/login", data={"code": "000000"})
    assert response.status_code == 401
    assert "did not match" in response.text


def test_second_login_uses_only_the_code(client, monkeypatch):
    secret = enrol(client)
    client.cookies.clear()
    # A later timestep, so the enrolment code is not simply replayed.
    later = shift_clock(monkeypatch, 60)
    response = client.post("/login", data={"code": code_for(secret, later)})
    assert response.status_code == 204
    assert client.get("/", follow_redirects=False).status_code == 200


def test_code_cannot_be_replayed(client, monkeypatch):
    secret = enrol(client)
    client.cookies.clear()
    later = shift_clock(monkeypatch, 60)
    used = code_for(secret, later)
    assert client.post("/login", data={"code": used}).status_code == 204
    client.cookies.clear()
    response = client.post("/login", data={"code": used})
    assert response.status_code == 401
    assert "already used" in response.text


def test_lockout_after_repeated_failures(client):
    client.get("/login")
    for _ in range(5):
        client.post("/login", data={"code": "000000"})
    response = client.post("/login", data={"code": "000000"})
    assert "Too many attempts" in response.text


def test_logout_clears_the_session(client):
    enrol(client)
    client.post("/logout", headers={"HX-Request": "true"})
    assert client.get("/", follow_redirects=False).status_code == 303


# -------------------------------------------------------------------- api


def test_api_requires_authentication(client):
    assert client.get("/api/state").status_code == 401


def test_health_is_public(client):
    payload = client.get("/api/health").json()
    assert payload["status"] == "ok"
    assert payload["enrolled"] is False


def test_power_cycle_needs_the_hardware_to_ack(client):
    enrol(client)
    assert client.get("/api/machine").json()["status"] == "unknown"
    poll(client, power_sense="off")
    assert client.get("/api/machine").json()["status"] == "offline"

    booting = client.post("/api/machine/power", json={"action": "on"}).json()
    assert booting["status"] == "booting"
    assert booting["transitioning"] is True
    assert booting["pending_action"] == "power_on"

    # A second command while one is in flight is refused.
    assert client.post("/api/machine/power", json={"action": "on"}).status_code == 409

    # Nothing moves until the device pulses the switch and says so.
    assert client.get("/api/machine").json()["status"] == "booting"
    command = poll(client)["command"]
    assert command["action"] == "power_on"
    assert command["pulse_ms"] == 500

    # An ack only retires the command — it does not assert the new state.
    ack(client, command["id"])
    assert client.get("/api/machine").json()["status"] == "offline"
    poll(client, power_sense="on")
    assert client.get("/api/machine").json()["status"] == "online"

    off = client.post("/api/machine/power", json={"action": "toggle"}).json()
    assert off["status"] == "shutting_down"
    assert off["pending_action"] == "graceful_shutdown"
    ack(client, poll(client)["command"]["id"])
    assert client.get("/api/machine").json()["is_on"] is True  # still waiting for the poll
    poll(client, power_sense="off")
    assert client.get("/api/machine").json()["is_on"] is False


def test_preferences_round_trip(client):
    enrol(client)
    updated = client.put("/api/preferences", json={"poll_interval": 9}).json()
    assert updated["poll_interval"] == 9
    assert client.get("/api/state").json()["preferences"]["poll_interval"] == 9


def test_preferences_reject_out_of_range_interval(client):
    enrol(client)
    assert client.put("/api/preferences", json={"poll_interval": 999}).status_code == 422


def test_state_survives_a_restart(client, tmp_path):
    enrol(client)
    client.put("/api/preferences", json={"poll_interval": 7})
    reloaded = client.deps.StateStore(tmp_path / "state.json")
    assert reloaded.get("preferences")["poll_interval"] == 7
    assert reloaded.raw("user")["totp_secret"]


# ------------------------------------------------------------- htmx fragments


def test_state_fragment_carries_an_hx_trigger(client):
    enrol(client)
    response = client.get("/partials/state", headers={"HX-Request": "true"})
    assert response.status_code == 204
    assert "tardis:state" in response.headers["HX-Trigger"]


def test_power_fragment_reports_conflicts_in_the_event(client):
    enrol(client)
    client.post("/partials/power", data={"action": "on"}, headers={"HX-Request": "true"})
    response = client.post(
        "/partials/power", data={"action": "on"}, headers={"HX-Request": "true"}
    )
    assert "already" in response.headers["HX-Trigger"]


def test_fragments_redirect_anonymous_htmx_callers(client):
    response = client.get("/partials/state", headers={"HX-Request": "true"})
    assert response.headers["HX-Redirect"] == "/login"


def test_preferences_fragment_saves_unchecked_boxes_as_false(client):
    enrol(client)
    client.post(
        "/partials/preferences",
        data={"poll_interval": 5},  # both switches off
        headers={"HX-Request": "true"},
    )
    prefs = client.get("/api/preferences").json()
    assert prefs["confirm_before_power_off"] is False
    assert prefs["animations"] is False
    assert prefs["poll_interval"] == 5


# ---------------------------------------------------------------- hardware link


def test_hardware_endpoints_reject_a_missing_or_wrong_key(client):
    enrol(client)
    assert client.post("/api/hardware/poll", json={}).status_code == 401
    assert (
        client.post("/api/hardware/poll", json={}, headers={"X-Tardis-Key": "nope"}).status_code
        == 401
    )
    assert (
        client.post(
            "/api/hardware/ack", json={"id": "x"}, headers={"X-Tardis-Key": "nope"}
        ).status_code
        == 401
    )


def test_hardware_endpoints_do_not_accept_an_operator_session(client):
    """A logged-in browser is not hardware: the key is the only way in."""
    enrol(client)  # leaves the session cookie on the client
    assert client.post("/api/hardware/poll", json={}).status_code == 401


def test_operator_endpoints_do_not_accept_the_pairing_key(client):
    """And the reverse: the device's key must not unlock the console's API."""
    enrol(client)
    headers = key_headers(client)
    client.cookies.clear()
    assert client.get("/api/state", headers=headers).status_code == 401
    assert client.get("/api/machine", headers=headers).status_code == 401
    assert client.get("/api/hardware", headers=headers).status_code == 401
    assert (
        client.post("/api/machine/power", json={"action": "on"}, headers=headers).status_code
        == 401
    )


def test_poll_reports_device_details_and_cadence(client):
    enrol(client)
    payload = poll(client, firmware="1.0.0", ip="192.168.1.50", rssi=-57, uptime_s=99)
    assert payload["command"] is None
    assert payload["poll_interval"] == client.deps.settings.hardware_poll_seconds

    hardware = client.get("/api/hardware").json()
    assert hardware["linked"] is True
    assert hardware["firmware"] == "1.0.0"
    assert hardware["ip"] == "192.168.1.50"
    assert hardware["rssi"] == -57


def test_console_shows_unlinked_until_the_device_polls(client):
    enrol(client)
    assert client.get("/api/machine").json()["linked"] is False
    poll(client)
    assert client.get("/api/machine").json()["linked"] is True


def test_command_is_repeated_until_acked(client):
    enrol(client)
    client.post("/api/machine/power", json={"action": "on"})
    first = poll(client)["command"]
    second = poll(client)["command"]
    assert first["id"] == second["id"]  # a dropped poll must not lose the command
    ack(client, first["id"])
    assert poll(client)["command"] is None


def test_hard_power_off_asks_for_a_five_second_hold(client):
    enrol(client)
    client.post("/api/machine/power", json={"action": "on"})
    ack(client, poll(client)["command"]["id"])
    poll(client, power_sense="on")

    state = client.post("/api/machine/power", json={"action": "hard_off"}).json()
    assert state["pending_action"] == "hard_power_off"
    command = poll(client)["command"]
    assert command["action"] == "hard_power_off"
    assert command["pulse_ms"] == 5000
    ack(client, command["id"])
    assert client.get("/api/machine").json()["status"] == "online"  # ack alone doesn't settle it
    poll(client, power_sense="off")
    assert client.get("/api/machine").json()["status"] == "offline"


def test_hard_power_off_overrides_a_stuck_boot(client):
    """The 5 s hold is the escape hatch — allowed even mid-transition."""
    enrol(client)
    client.post("/api/machine/power", json={"action": "on"})
    poll(client)  # boot command delivered, never acked

    forced = client.post("/api/machine/power", json={"action": "hard_power_off"})
    assert forced.status_code == 200
    command = poll(client)["command"]
    assert command["action"] == "hard_power_off"
    ack(client, command["id"])
    assert client.get("/api/machine").json()["transitioning"] is False
    poll(client, power_sense="off")
    assert client.get("/api/machine").json()["status"] == "offline"


def test_failed_ack_reverts_and_records_the_error(client):
    enrol(client)
    poll(client, power_sense="off")
    client.post("/api/machine/power", json={"action": "on"})
    command = poll(client)["command"]
    ack(client, command["id"], status="failed", detail="gpio busy")

    state = client.get("/api/machine").json()
    assert state["status"] == "offline"
    assert state["transitioning"] is False
    assert "gpio busy" in state["last_error"]["message"]
    assert client.get("/api/hardware").json()["commands_failed"] == 1


def test_unacked_command_expires_and_reverts(client, monkeypatch):
    enrol(client)
    poll(client, power_sense="off")
    client.post("/api/machine/power", json={"action": "on"})
    assert client.get("/api/machine").json()["status"] == "booting"

    # Jump past the ack deadline.
    from app.core import machine as machine_module

    real_time = time.time
    monkeypatch.setattr(
        machine_module.time, "time", lambda: real_time() + client.deps.settings.ack_timeout_seconds + 1
    )

    state = client.get("/api/machine").json()
    assert state["status"] == "offline"
    assert state["transitioning"] is False
    assert "never confirmed" in state["last_error"]["message"]
    assert poll(client)["command"] is None


def test_ack_for_an_unknown_command_is_refused(client):
    enrol(client)
    response = client.post(
        "/api/hardware/ack", json={"id": "deadbeef"}, headers=key_headers(client)
    )
    assert response.status_code == 409


def test_power_sense_report_is_ground_truth(client):
    """A device wired to a sense line settles the state, even without a command."""
    enrol(client)
    poll(client, power_sense="on")
    assert client.get("/api/machine").json()["status"] == "online"
    poll(client, power_sense="off")
    assert client.get("/api/machine").json()["status"] == "offline"


def test_power_sense_confirms_a_pending_command(client):
    enrol(client)
    client.post("/api/machine/power", json={"action": "on"})
    poll(client, power_sense="on")
    state = client.get("/api/machine").json()
    assert state["status"] == "online"
    assert state["transitioning"] is False


def test_no_sense_pin_reports_unknown_and_ack_cannot_settle_it(client):
    """A device with no sense pin fitted always reports "unknown" — the ack
    that follows a pulse never gets to assert online/offline on its own."""
    enrol(client)
    payload = poll(client, power_sense="unknown")
    assert payload["machine"]["status"] == "unknown"

    client.post("/api/machine/power", json={"action": "on"})
    ack(client, poll(client, power_sense="unknown")["command"]["id"])
    state = client.get("/api/machine").json()
    assert state["status"] == "unknown"
    assert state["state_known"] is False


def test_unknown_state_offers_every_action_and_no_side_can_be_ruled_out(client):
    enrol(client)
    poll(client, power_sense="unknown")
    state = client.get("/api/machine").json()
    assert state["can_power_on"] is True
    assert state["can_power_off"] is True
    assert state["can_hard_power_off"] is True

    # Neither direction can be refused as "already there" while unknown.
    assert client.post("/api/machine/power", json={"action": "on"}).status_code == 200
    ack(client, poll(client, power_sense="unknown")["command"]["id"])
    assert client.post("/api/machine/power", json={"action": "off"}).status_code == 200


def test_an_explicit_unknown_report_overrides_a_stale_status(client):
    """The device losing its sense wire (or being reconfigured) must not leave
    a confirmed-looking status stuck from before."""
    enrol(client)
    poll(client, power_sense="on")
    assert client.get("/api/machine").json()["status"] == "online"
    poll(client, power_sense="unknown")
    assert client.get("/api/machine").json()["status"] == "unknown"


# ------------------------------------------------------- auth coverage audit


PUBLIC_PATHS = {
    ("GET", "/api/health"),  # liveness probe, no data
    ("GET", "/login"),
    ("POST", "/login"),
    ("POST", "/login/new-key"),
    ("POST", "/logout"),  # clearing a cookie needs no cookie
    ("POST", "/api/auth/enroll"),
    ("POST", "/api/auth/login"),
    ("POST", "/api/auth/logout"),
    ("GET", "/api/auth/status"),  # boolean flags only, drives the login page
}
HARDWARE_PATHS = {("POST", "/api/hardware/poll"), ("POST", "/api/hardware/ack")}


def test_every_route_is_guarded_by_a_session_or_the_pairing_key(client):
    """No route may be reachable by an anonymous caller unless it is on the list.

    This is the alignment check between the two flows: the console's routes take
    the operator's session, the device's routes take the pairing key, and
    nothing accepts both.
    """
    enrol(client)
    client.cookies.clear()

    unguarded: list[str] = []
    for route in client.app.routes:
        path = getattr(route, "path", "")
        if not path or path.startswith("/static") or "{" in path:
            continue
        for method in sorted(getattr(route, "methods", set()) - {"HEAD", "OPTIONS"}):
            if (method, path) in PUBLIC_PATHS:
                continue
            response = client.request(method, path, json={}, follow_redirects=False)
            if (method, path) in HARDWARE_PATHS:
                if response.status_code != 401:
                    unguarded.append(f"{method} {path} -> {response.status_code} (needs key)")
                continue
            # Session routes: 401 for the API, a redirect to /login for pages.
            guarded = response.status_code == 401 or (
                response.status_code in (303, 307) and "/login" in response.headers.get("location", "")
            )
            if not guarded:
                unguarded.append(f"{method} {path} -> {response.status_code}")

    assert not unguarded, "unguarded routes: " + ", ".join(unguarded)


def test_a_bare_poll_or_ack_does_not_blank_device_details(client):
    enrol(client)
    poll(client, firmware="1.0.0", ip="192.168.1.50", rssi=-57)
    poll(client)  # empty heartbeat
    client.post("/api/machine/power", json={"action": "on"})
    ack(client, poll(client)["command"]["id"])

    hardware = client.get("/api/hardware").json()
    assert hardware["firmware"] == "1.0.0"
    assert hardware["ip"] == "192.168.1.50"
    assert hardware["rssi"] == -57
