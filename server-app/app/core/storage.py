"""Tiny JSON-file store.

The whole app has a single user and a single machine, so a document on disk is
plenty. Writes go through a temp file + ``os.replace`` so a crash mid-write can
never leave a truncated state file behind.
"""

from __future__ import annotations

import json
import os
import threading
from pathlib import Path
from typing import Any

DEFAULT_STATE: dict[str, Any] = {
    "version": 1,
    "secret_key": None,
    "pairing_key": None,  # hardware auth key for the poll/ack handshake, see machine.py
    "hardware": {"last_seen": None},  # last authenticated poll from the hardware side
    "user": None,  # {"totp_secret", "created_at", "last_login", "last_counter"}
    # Secret offered on the enrolment screen, kept until the code confirms it.
    "pending_secret": None,
    "auth": {"failed_attempts": 0, "locked_until": None},
    "machine": {
        # idle states: "online" | "offline"; transient: "booting" | "shutting_down"
        "status": "offline",
        "changed_at": None,
        "target": None,
        "last_action": None,
        "boot_count": 0,
    },
    "preferences": {
        "confirm_before_power_off": True,
        "animations": True,
        "poll_interval": 3,
    },
}


def _deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    """Merge ``override`` onto a copy of ``base``, recursing into nested dicts."""
    merged = dict(base)
    for key, value in override.items():
        current = merged.get(key)
        if isinstance(current, dict) and isinstance(value, dict):
            merged[key] = _deep_merge(current, value)
        else:
            merged[key] = value
    return merged


class StateStore:
    """Thread-safe reader/writer for the state document."""

    def __init__(self, path: Path) -> None:
        self._path = path
        self._lock = threading.RLock()
        self._state: dict[str, Any] = self._load()

    @property
    def path(self) -> Path:
        return self._path

    def _load(self) -> dict[str, Any]:
        if not self._path.exists():
            return json.loads(json.dumps(DEFAULT_STATE))
        try:
            raw = json.loads(self._path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            # A damaged file should not brick the app: start over from defaults.
            return json.loads(json.dumps(DEFAULT_STATE))
        if not isinstance(raw, dict):
            return json.loads(json.dumps(DEFAULT_STATE))
        # Merge so state files written by an older version pick up new keys.
        return _deep_merge(DEFAULT_STATE, raw)

    def _flush(self) -> None:
        self._path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self._path.with_suffix(self._path.suffix + ".tmp")
        tmp.write_text(json.dumps(self._state, indent=2, sort_keys=True), encoding="utf-8")
        os.replace(tmp, self._path)
        try:
            os.chmod(self._path, 0o600)
        except OSError:
            # Best effort — some filesystems (bind mounts, Windows) refuse chmod.
            pass

    def snapshot(self) -> dict[str, Any]:
        """Return a deep copy of the current state."""
        with self._lock:
            return json.loads(json.dumps(self._state))

    def get(self, section: str) -> dict[str, Any]:
        with self._lock:
            return json.loads(json.dumps(self._state.get(section) or {}))

    def update(self, section: str, values: dict[str, Any]) -> dict[str, Any]:
        """Shallow-merge ``values`` into a top-level section and persist."""
        with self._lock:
            current = self._state.get(section)
            self._state[section] = {**current, **values} if isinstance(current, dict) else values
            self._flush()
            return json.loads(json.dumps(self._state[section]))

    def set(self, key: str, value: Any) -> None:
        with self._lock:
            self._state[key] = value
            self._flush()

    def raw(self, key: str, default: Any = None) -> Any:
        with self._lock:
            value = self._state.get(key, default)
            return json.loads(json.dumps(value)) if isinstance(value, (dict, list)) else value

    def reset(self) -> None:
        with self._lock:
            self._state = json.loads(json.dumps(DEFAULT_STATE))
            self._flush()
