"""tardis-net — a one-user, TOTP-gated remote power switch."""

from __future__ import annotations

from fastapi import Depends, FastAPI, Request
from fastapi.openapi.docs import get_swagger_ui_html
from fastapi.openapi.utils import get_openapi
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles

from ..core.config import BASE_DIR, settings
from .dependencies import require_session
from .routers import api, pages

app = FastAPI(
    title="tardis-net",
    description=f"TOTP-protected power control for {settings.machine_name}.",
    version="0.1.0",
    # Served by hand below so the schema needs the operator's session too.
    docs_url=None,
    openapi_url=None,
    redoc_url=None,
)

app.mount("/static", StaticFiles(directory=str(BASE_DIR / "static")), name="static")
app.include_router(api.router)
app.include_router(pages.router)


@app.get("/api/openapi.json", include_in_schema=False)
def openapi_schema(_: dict = Depends(require_session)) -> JSONResponse:
    return JSONResponse(
        get_openapi(title=app.title, version=app.version, description=app.description, routes=app.routes)
    )


@app.get("/api/docs", include_in_schema=False)
def swagger_ui(_: dict = Depends(require_session)) -> HTMLResponse:
    return get_swagger_ui_html(openapi_url="/api/openapi.json", title=f"{app.title} — API")


@app.exception_handler(401)
async def unauthorized_handler(request: Request, exc) -> JSONResponse | RedirectResponse:
    """Send browsers to the login page; keep JSON for API and htmx callers."""
    detail = getattr(exc, "detail", "Authentication required.")
    if request.url.path.startswith("/api") or request.headers.get("HX-Request") == "true":
        return JSONResponse({"detail": detail}, status_code=401, headers=_hx_login(request))
    return RedirectResponse("/login", status_code=303)


def _hx_login(request: Request) -> dict[str, str]:
    return {"HX-Redirect": "/login"} if request.headers.get("HX-Request") == "true" else {}
