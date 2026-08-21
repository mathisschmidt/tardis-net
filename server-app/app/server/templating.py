"""Jinja environment shared by the page routes."""

from __future__ import annotations

from fastapi.templating import Jinja2Templates

from .config import BASE_DIR, settings

templates = Jinja2Templates(directory=str(BASE_DIR / "templates"))
templates.env.globals.update(
    machine_name=settings.machine_name,
    issuer=settings.issuer,
    app_name="tardis-net",
)
