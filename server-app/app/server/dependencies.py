"""Shared singletons and FastAPI dependencies."""

from __future__ import annotations

from fastapi import Depends, HTTPException, Request, status

from .auth import AuthService
from .config import settings
from .machine import MachineController
from .storage import StateStore

store = StateStore(settings.state_file)
auth_service = AuthService(store, settings)
machine = MachineController(store, settings)


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
