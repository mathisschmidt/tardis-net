"""Server-rendered pages and the htmx fragments they talk to."""

from __future__ import annotations

import json
import time

from fastapi import APIRouter, Depends, Form, Request, Response, status
from fastapi.responses import HTMLResponse, RedirectResponse

from ..auth import AuthError, AuthService
from ..config import settings
from ..dependencies import current_session, get_auth, get_machine, get_store
from ..machine import MachineController, MachineError
from ..storage import StateStore
from ..templating import templates
from .api import clear_session_cookie, set_session_cookie

router = APIRouter(tags=["pages"])


@router.get("/", response_class=HTMLResponse)
def index(
    request: Request,
    session: dict | None = Depends(current_session),
    machine: MachineController = Depends(get_machine),
    store: StateStore = Depends(get_store),
) -> Response:
    if session is None:
        return RedirectResponse("/login", status_code=status.HTTP_303_SEE_OTHER)
    return templates.TemplateResponse(
        request,
        "dashboard.html",
        {
            "machine": machine.state(),
            "preferences": store.get("preferences"),
            "account": settings.account_name,
        },
    )


@router.get("/login", response_class=HTMLResponse)
def login_page(
    request: Request,
    session: dict | None = Depends(current_session),
    auth: AuthService = Depends(get_auth),
) -> Response:
    if session is not None:
        return RedirectResponse("/", status_code=status.HTTP_303_SEE_OTHER)
    return templates.TemplateResponse(request, "login.html", _login_context(auth))


@router.post("/login", response_class=HTMLResponse)
def login_submit(
    request: Request,
    code: str = Form(default=""),
    auth: AuthService = Depends(get_auth),
) -> Response:
    """Handle both the first pairing and every later login.

    Answers htmx with the form fragment on failure, and with ``HX-Redirect`` on
    success so the browser performs a real navigation to the dashboard.
    """
    enrolling = not auth.is_enrolled
    try:
        if enrolling:
            auth.confirm_enrolment(code)
        else:
            auth.verify_login(code)
    except AuthError as exc:
        context = _login_context(auth) | {"error": exc.message}
        response = templates.TemplateResponse(
            request,
            "partials/login_form.html",
            context,
            status_code=status.HTTP_401_UNAUTHORIZED,
        )
        return response

    response = Response(status_code=status.HTTP_204_NO_CONTENT)
    response.headers["HX-Redirect"] = "/"
    set_session_cookie(response, auth.issue_session())
    return response


@router.post("/login/new-key", response_class=HTMLResponse)
def rotate_key(request: Request, auth: AuthService = Depends(get_auth)) -> Response:
    """Hand out a fresh secret when the QR code never made it into the app."""
    if auth.is_enrolled:
        return _hx_redirect("/login")
    auth.rotate_enrolment()
    return templates.TemplateResponse(request, "partials/login_form.html", _login_context(auth))


@router.post("/logout")
def logout(request: Request) -> Response:
    response = _hx_redirect("/login", request)
    clear_session_cookie(response)
    return response


# ------------------------------------------------------------ htmx fragments


@router.get("/partials/state")
def sync_state(
    session: dict | None = Depends(current_session),
    machine: MachineController = Depends(get_machine),
    store: StateStore = Depends(get_store),
) -> Response:
    """Polled by htmx. The payload rides on ``HX-Trigger`` rather than in the
    body, so the animated console is never swapped out mid-animation."""
    if session is None:
        return _hx_redirect("/login")
    return _state_event(machine, store)


@router.post("/partials/power")
def power(
    action: str = Form(default="toggle"),
    session: dict | None = Depends(current_session),
    machine: MachineController = Depends(get_machine),
    store: StateStore = Depends(get_store),
) -> Response:
    if session is None:
        return _hx_redirect("/login")
    error: str | None = None
    try:
        if action == "on":
            machine.power_on()
        elif action == "off":
            machine.power_off()
        else:
            machine.toggle()
    except MachineError as exc:
        error = str(exc)
    return _state_event(machine, store, error=error)


@router.post("/partials/preferences")
def save_preferences(
    # Unchecked boxes are simply absent from the form body, hence False defaults.
    confirm_before_power_off: bool = Form(default=False),
    animations: bool = Form(default=False),
    poll_interval: int = Form(default=3),
    session: dict | None = Depends(current_session),
    machine: MachineController = Depends(get_machine),
    store: StateStore = Depends(get_store),
) -> Response:
    if session is None:
        return _hx_redirect("/login")
    store.update(
        "preferences",
        {
            "confirm_before_power_off": confirm_before_power_off,
            "animations": animations,
            "poll_interval": max(1, min(60, poll_interval)),
        },
    )
    return _state_event(machine, store, toast="Preferences saved")


# ----------------------------------------------------------------- internals


def _login_context(auth: AuthService) -> dict:
    context: dict = {
        "enrolled": auth.is_enrolled,
        "locked_for": auth.lockout_remaining(),
        "issuer": settings.issuer,
        "account": settings.account_name,
        "enrolment": None,
        "error": None,
    }
    if not auth.is_enrolled:
        context["enrolment"] = auth.enrolment()
    return context


def _state_event(
    machine: MachineController,
    store: StateStore,
    *,
    error: str | None = None,
    toast: str | None = None,
) -> Response:
    """Empty body + an ``HX-Trigger`` event carrying the current state."""
    payload = {
        "machine": machine.state(),
        "preferences": store.get("preferences"),
        "server_time": time.time(),
        "error": error,
        "toast": toast,
    }
    response = Response(status_code=status.HTTP_204_NO_CONTENT)
    response.headers["HX-Trigger"] = json.dumps({"tardis:state": payload})
    return response


def _hx_redirect(target: str, request: Request | None = None) -> Response:
    """Redirect htmx via ``HX-Redirect``, and plain browsers the usual way."""
    if request is not None and request.headers.get("HX-Request") != "true":
        return RedirectResponse(target, status_code=status.HTTP_303_SEE_OTHER)
    response = Response(status_code=status.HTTP_204_NO_CONTENT)
    response.headers["HX-Redirect"] = target
    return response
