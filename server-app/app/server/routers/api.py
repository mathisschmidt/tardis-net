"""JSON API under /api.

Everything the web UI does is available here too, so a script or a phone
shortcut can drive the machine without going through the pages.
"""

from __future__ import annotations

import time

from fastapi import APIRouter, Depends, HTTPException, Response, status

from ..dependencies import (
    current_session,
    get_auth,
    get_machine,
    get_store,
    require_pairing_key,
    require_session,
)
from ...core.auth import AuthError, AuthService
from ...core.config import settings
from ...core.machine import MachineController, MachineError
from ...core.schemas import (
    AuthStatusOut,
    CodeIn,
    MachineOut,
    PowerIn,
    PreferencesIn,
    PreferencesOut,
    StateOut,
)
from ...core.storage import StateStore

router = APIRouter(prefix="/api", tags=["api"])


def set_session_cookie(response: Response, token: str) -> None:
    response.set_cookie(
        settings.session_cookie,
        token,
        max_age=settings.session_max_age,
        httponly=True,
        samesite="lax",
        secure=settings.cookie_secure,
        path="/",
    )


def clear_session_cookie(response: Response) -> None:
    response.delete_cookie(settings.session_cookie, path="/")


@router.get("/health", summary="Liveness probe")
def health(auth: AuthService = Depends(get_auth)) -> dict:
    return {"status": "ok", "enrolled": auth.is_enrolled, "server_time": time.time()}


# ----------------------------------------------------------------------- auth


@router.get("/auth/status", response_model=AuthStatusOut)
def auth_status(
    session: dict | None = Depends(current_session), auth: AuthService = Depends(get_auth)
) -> AuthStatusOut:
    return AuthStatusOut(
        enrolled=auth.is_enrolled,
        authenticated=session is not None,
        locked_for=auth.lockout_remaining(),
    )


@router.post("/auth/enroll", response_model=AuthStatusOut, summary="Confirm the first pairing")
def enroll(
    payload: CodeIn, response: Response, auth: AuthService = Depends(get_auth)
) -> AuthStatusOut:
    try:
        auth.confirm_enrolment(payload.code)
    except AuthError as exc:
        raise _auth_http_error(exc) from exc
    set_session_cookie(response, auth.issue_session())
    return AuthStatusOut(enrolled=True, authenticated=True)


@router.post("/auth/login", response_model=AuthStatusOut)
def login(
    payload: CodeIn, response: Response, auth: AuthService = Depends(get_auth)
) -> AuthStatusOut:
    try:
        auth.verify_login(payload.code)
    except AuthError as exc:
        raise _auth_http_error(exc) from exc
    set_session_cookie(response, auth.issue_session())
    return AuthStatusOut(enrolled=True, authenticated=True)


@router.post("/auth/logout", status_code=status.HTTP_204_NO_CONTENT)
def logout(response: Response) -> Response:
    clear_session_cookie(response)
    response.status_code = status.HTTP_204_NO_CONTENT
    return response


# -------------------------------------------------------------------- machine


@router.get("/state", response_model=StateOut)
def read_state(
    _: dict = Depends(require_session),
    machine: MachineController = Depends(get_machine),
    store: StateStore = Depends(get_store),
) -> StateOut:
    return StateOut(
        machine=MachineOut(**machine.state()),
        preferences=PreferencesOut(**store.get("preferences")),
        server_time=time.time(),
    )


@router.get("/machine", response_model=MachineOut)
def read_machine(
    _: dict = Depends(require_session), machine: MachineController = Depends(get_machine)
) -> MachineOut:
    return MachineOut(**machine.state())


@router.post("/machine/power", response_model=MachineOut)
def set_power(
    payload: PowerIn,
    _: dict = Depends(require_session),
    machine: MachineController = Depends(get_machine),
) -> MachineOut:
    try:
        if payload.action == "on":
            state = machine.power_on()
        elif payload.action == "off":
            state = machine.power_off()
        else:
            state = machine.toggle()
    except MachineError as exc:
        raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=str(exc)) from exc
    return MachineOut(**state)


# -------------------------------------------------------------------- hardware


@router.post(
    "/hardware/poll",
    response_model=MachineOut,
    summary="Hardware heartbeat — authenticated with the pairing key, not a session",
)
def hardware_poll(
    _: None = Depends(require_pairing_key),
    machine: MachineController = Depends(get_machine),
) -> MachineOut:
    machine.record_hardware_poll()
    return MachineOut(**machine.state())


# ---------------------------------------------------------------- preferences


@router.get("/preferences", response_model=PreferencesOut)
def read_preferences(
    _: dict = Depends(require_session), store: StateStore = Depends(get_store)
) -> PreferencesOut:
    return PreferencesOut(**store.get("preferences"))


@router.put("/preferences", response_model=PreferencesOut)
def write_preferences(
    payload: PreferencesIn,
    _: dict = Depends(require_session),
    store: StateStore = Depends(get_store),
) -> PreferencesOut:
    changes = payload.changes()
    prefs = store.update("preferences", changes) if changes else store.get("preferences")
    return PreferencesOut(**prefs)


def _auth_http_error(exc: AuthError) -> HTTPException:
    code = (
        status.HTTP_429_TOO_MANY_REQUESTS
        if exc.retry_after
        else status.HTTP_401_UNAUTHORIZED
    )
    headers = {"Retry-After": str(exc.retry_after)} if exc.retry_after else None
    return HTTPException(status_code=code, detail=exc.message, headers=headers)
