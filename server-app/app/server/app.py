"""tardis-net — a one-user, TOTP-gated remote power switch."""

from __future__ import annotations

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles

from ..core.config import BASE_DIR, settings
from .routers import api, pages

app = FastAPI(
    title="tardis-net",
    description=f"TOTP-protected power control for {settings.machine_name}.",
    version="0.1.0",
    docs_url="/api/docs",
    openapi_url="/api/openapi.json",
    redoc_url=None,
)

app.mount("/static", StaticFiles(directory=str(BASE_DIR / "static")), name="static")
app.include_router(api.router)
app.include_router(pages.router)


@app.exception_handler(401)
async def unauthorized_handler(request: Request, exc) -> JSONResponse | RedirectResponse:
    """Send browsers to the login page; keep JSON for API and htmx callers."""
    detail = getattr(exc, "detail", "Authentication required.")
    if request.url.path.startswith("/api") or request.headers.get("HX-Request") == "true":
        return JSONResponse({"detail": detail}, status_code=401, headers=_hx_login(request))
    return RedirectResponse("/login", status_code=303)


def _hx_login(request: Request) -> dict[str, str]:
    return {"HX-Redirect": "/login"} if request.headers.get("HX-Request") == "true" else {}
