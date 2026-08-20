"""Application settings, read once from the environment."""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
PROJECT_DIR = BASE_DIR.parent


def _env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


def _env_int(name: str, default: int) -> int:
    raw = os.getenv(name)
    if raw is None or not raw.strip():
        return default
    try:
        return int(raw)
    except ValueError:
        return default


@dataclass(frozen=True)
class Settings:
    # Identity shown in the authenticator app.
    issuer: str = field(default_factory=lambda: os.getenv("TARDIS_ISSUER", "tardis-net"))
    account_name: str = field(default_factory=lambda: os.getenv("TARDIS_ACCOUNT", "operator"))

    # Name of the machine we power on and off.
    machine_name: str = field(default_factory=lambda: os.getenv("TARDIS_MACHINE", "tardis"))

    # Where the persisted state lives.
    state_file: Path = field(
        default_factory=lambda: Path(
            os.getenv("TARDIS_STATE_FILE", str(PROJECT_DIR / "data" / "state.json"))
        ).expanduser()
    )

    # Session cookie.
    session_cookie: str = field(
        default_factory=lambda: os.getenv("TARDIS_SESSION_COOKIE", "tardis_session")
    )
    session_max_age: int = field(
        default_factory=lambda: _env_int("TARDIS_SESSION_MAX_AGE", 12 * 60 * 60)
    )
    cookie_secure: bool = field(default_factory=lambda: _env_bool("TARDIS_COOKIE_SECURE", False))

    # Sign the session cookie. Generated and persisted on first boot when unset.
    secret_key: str | None = field(default_factory=lambda: os.getenv("TARDIS_SECRET_KEY"))

    # Brute-force guard on the TOTP form.
    max_failed_attempts: int = field(default_factory=lambda: _env_int("TARDIS_MAX_ATTEMPTS", 5))
    lockout_seconds: int = field(default_factory=lambda: _env_int("TARDIS_LOCKOUT_SECONDS", 300))

    # How long the simulated boot / shutdown sequence takes.
    transition_seconds: int = field(
        default_factory=lambda: _env_int("TARDIS_TRANSITION_SECONDS", 8)
    )


settings = Settings()
