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
    monkeypatch.setenv("TARDIS_TRANSITION_SECONDS", "1")

    # The modules read settings at import time, so reload them per test.
    from app import config, dependencies, machine, storage, templating
    from app.routers import api, pages

    for module in (config, storage, machine, templating, dependencies, api, pages):
        importlib.reload(module)
    main = importlib.reload(importlib.import_module("app.main"))

    with TestClient(main.app) as test_client:
        test_client.deps = dependencies  # type: ignore[attr-defined]
        yield test_client


def code_for(secret: str, at: float | None = None) -> str:
    return pyotp.TOTP(secret).at(at or time.time())


def shift_clock(monkeypatch, seconds: int) -> float:
    """Move the server's clock forward so a later TOTP step is the current one."""
    from app import auth as auth_module

    target = time.time() + seconds
    monkeypatch.setattr(auth_module, "_now", lambda: int(target))
    return target


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


def test_power_cycle(client):
    enrol(client)
    assert client.get("/api/machine").json()["status"] == "offline"

    booting = client.post("/api/machine/power", json={"action": "on"}).json()
    assert booting["status"] == "booting"
    assert booting["transitioning"] is True

    # A second command while in transition is refused.
    assert client.post("/api/machine/power", json={"action": "on"}).status_code == 409

    time.sleep(1.1)
    assert client.get("/api/machine").json()["status"] == "online"

    off = client.post("/api/machine/power", json={"action": "toggle"}).json()
    assert off["status"] == "shutting_down"
    time.sleep(1.1)
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
