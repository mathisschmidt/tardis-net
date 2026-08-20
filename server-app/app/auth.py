"""TOTP enrolment, verification and signed session cookies.

There is exactly one user. Until they enrol, the login page hands out a secret
(QR code + base32 key) and asks for a confirmation code. Once enrolled, only the
six-digit code is ever asked for again.
"""

from __future__ import annotations

import base64
import secrets
import time
from dataclasses import dataclass

import pyotp
import qrcode
from itsdangerous import BadSignature, SignatureExpired, URLSafeTimedSerializer

from .config import Settings
from .storage import StateStore

TOTP_INTERVAL = 30
# Accept the neighbouring step in each direction to tolerate clock drift.
TOTP_VALID_WINDOW = 1


class AuthError(Exception):
    """Raised when a login or enrolment attempt is rejected."""

    def __init__(self, message: str, *, retry_after: int | None = None) -> None:
        super().__init__(message)
        self.message = message
        self.retry_after = retry_after


@dataclass(frozen=True)
class Enrolment:
    """Everything the enrolment screen needs to render."""

    secret: str
    uri: str
    qr_data_uri: str

    @property
    def formatted_secret(self) -> str:
        """The base32 secret in four-character groups, for manual entry."""
        return " ".join(self.secret[i : i + 4] for i in range(0, len(self.secret), 4))


def _now() -> int:
    return int(time.time())


class AuthService:
    def __init__(self, store: StateStore, settings: Settings) -> None:
        self._store = store
        self._settings = settings
        self._serializer = URLSafeTimedSerializer(
            self._resolve_secret_key(), salt="tardis-net.session"
        )

    # ------------------------------------------------------------------ setup

    def _resolve_secret_key(self) -> str:
        """Use the configured key, else generate one and keep it on disk.

        Persisting matters: a key that changed on every boot would silently log
        the user out each time the server restarts.
        """
        if self._settings.secret_key:
            return self._settings.secret_key
        stored = self._store.raw("secret_key")
        if not stored:
            stored = secrets.token_urlsafe(48)
            self._store.set("secret_key", stored)
        return stored

    # ------------------------------------------------------------------- user

    @property
    def is_enrolled(self) -> bool:
        user = self._store.raw("user")
        return bool(user and user.get("totp_secret"))

    def enrolment(self) -> Enrolment:
        """Return the pending enrolment, creating it on first visit.

        The secret is stored as ``pending_secret`` so refreshing the page keeps
        showing the same QR code the authenticator app already scanned.
        """
        if self.is_enrolled:
            raise AuthError("A user is already enrolled.")

        secret = self._store.raw("pending_secret")
        if not secret:
            secret = pyotp.random_base32()
            self._store.set("pending_secret", secret)

        uri = pyotp.TOTP(secret, interval=TOTP_INTERVAL).provisioning_uri(
            name=self._settings.account_name, issuer_name=self._settings.issuer
        )
        return Enrolment(secret=secret, uri=uri, qr_data_uri=_qr_data_uri(uri))

    def rotate_enrolment(self) -> Enrolment:
        """Throw away the pending secret and offer a fresh one."""
        if self.is_enrolled:
            raise AuthError("A user is already enrolled.")
        self._store.set("pending_secret", None)
        return self.enrolment()

    def confirm_enrolment(self, code: str) -> None:
        """Turn the pending secret into the enrolled user once a code checks out."""
        if self.is_enrolled:
            raise AuthError("A user is already enrolled.")
        secret = self._store.raw("pending_secret")
        if not secret:
            raise AuthError("Enrolment expired — reload the page to get a new key.")

        self._guard_lockout()
        counter = _verify(secret, code)
        if counter is None:
            self._register_failure()
            raise AuthError("That code did not match. Check your authenticator and try again.")

        self._store.set(
            "user",
            {
                "totp_secret": secret,
                "created_at": _now(),
                "last_login": _now(),
                "last_counter": counter,
            },
        )
        self._store.set("pending_secret", None)
        self._reset_failures()

    def verify_login(self, code: str) -> None:
        user = self._store.raw("user")
        if not user or not user.get("totp_secret"):
            raise AuthError("No user is enrolled yet.")

        self._guard_lockout()
        counter = _verify(user["totp_secret"], code)
        if counter is None:
            self._register_failure()
            raise AuthError("Invalid code.")
        if user.get("last_counter") is not None and counter <= int(user["last_counter"]):
            # Each 30-second code is single-use, so a replayed one is refused.
            self._register_failure()
            raise AuthError("That code was already used. Wait for the next one.")

        self._store.set(
            "user", {**user, "last_login": _now(), "last_counter": counter}
        )
        self._reset_failures()

    def reset_user(self) -> None:
        """Forget the enrolled user so the app can be re-paired."""
        self._store.set("user", None)
        self._store.set("pending_secret", None)
        self._reset_failures()

    # -------------------------------------------------------------- lockout

    def lockout_remaining(self) -> int:
        auth = self._store.get("auth")
        locked_until = auth.get("locked_until")
        if not locked_until:
            return 0
        return max(0, int(locked_until) - _now())

    def _guard_lockout(self) -> None:
        remaining = self.lockout_remaining()
        if remaining > 0:
            raise AuthError(
                f"Too many attempts. Try again in {remaining} seconds.", retry_after=remaining
            )

    def _register_failure(self) -> None:
        auth = self._store.get("auth")
        attempts = int(auth.get("failed_attempts") or 0) + 1
        locked_until = None
        if attempts >= self._settings.max_failed_attempts:
            locked_until = _now() + self._settings.lockout_seconds
            attempts = 0
        self._store.update("auth", {"failed_attempts": attempts, "locked_until": locked_until})

    def _reset_failures(self) -> None:
        self._store.update("auth", {"failed_attempts": 0, "locked_until": None})

    # -------------------------------------------------------------- sessions

    def issue_session(self) -> str:
        return self._serializer.dumps({"sub": self._settings.account_name, "iat": _now()})

    def read_session(self, token: str | None) -> dict | None:
        if not token:
            return None
        try:
            data = self._serializer.loads(token, max_age=self._settings.session_max_age)
        except (BadSignature, SignatureExpired):
            return None
        if not self.is_enrolled:
            # The user was reset: old cookies must stop working.
            return None
        return data if isinstance(data, dict) else None


def _verify(secret: str, code: str) -> int | None:
    """Return the timestep the code belongs to, or ``None`` when it is invalid."""
    cleaned = "".join(ch for ch in (code or "") if ch.isdigit())
    if len(cleaned) != 6:
        return None
    totp = pyotp.TOTP(secret, interval=TOTP_INTERVAL)
    now = _now()
    for offset in range(-TOTP_VALID_WINDOW, TOTP_VALID_WINDOW + 1):
        at = now + offset * TOTP_INTERVAL
        if secrets.compare_digest(totp.at(at), cleaned):
            return at // TOTP_INTERVAL
    return None


def _qr_data_uri(uri: str, *, border: int = 2) -> str:
    """Render the otpauth URI as an inline SVG.

    Drawing the module matrix by hand keeps the QR crisp at any size and avoids
    pulling in Pillow just to produce one image.
    """
    qr = qrcode.QRCode(version=None, border=border)
    qr.add_data(uri)
    qr.make(fit=True)
    matrix = qr.get_matrix()
    size = len(matrix)

    path = "".join(
        f"M{x} {y}h1v1h-1z"
        for y, row in enumerate(matrix)
        for x, is_dark in enumerate(row)
        if is_dark
    )
    svg = (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {size} {size}" '
        'shape-rendering="crispEdges">'
        f'<rect width="{size}" height="{size}" fill="#e8f0ff"/>'
        f'<path d="{path}" fill="#050914"/>'
        "</svg>"
    )
    encoded = base64.b64encode(svg.encode("utf-8")).decode("ascii")
    return f"data:image/svg+xml;base64,{encoded}"
