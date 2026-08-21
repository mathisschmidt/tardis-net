"""FastAPI dependency wiring around the shared runtime singletons."""

from __future__ import annotations

import secrets

from fastapi import Depends, Header, HTTPException, Request, status

from ..core.auth import AuthService
from ..core.config import settings
from ..core.machine import MachineController
from ..core.runtime import auth_service, machine, store
from ..core.storage import StateStore


def get_store() -> StateStore:
    return store


def get_auth() -> AuthService:
    return auth_service


def get_machine() -> MachineController:
    return machine


def current_session(request: Request, auth: AuthService = Depends(get_auth)) -> dict | None:
    return auth.read_session(request.cookies.get(settings.session_cookie))


def require_session(session: dict | None = Depends(current_session)) -> dict:
    """Guard for the API. Page routes redirect instead — see ``routers.pages``."""
    if session is None:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED, detail="Authentication required."
        )
    return session


def require_pairing_key(
    x_tardis_key: str | None = Header(default=None, alias="X-Tardis-Key"),
    machine: MachineController = Depends(get_machine),
) -> None:
    """Guard for hardware endpoints — the pairing key stands in for a session."""
    if not x_tardis_key or not secrets.compare_digest(x_tardis_key, machine.pairing_key):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED, detail="Invalid pairing key."
        )
