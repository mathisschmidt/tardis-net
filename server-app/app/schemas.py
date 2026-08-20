"""Request and response models for the JSON API."""

from __future__ import annotations

from typing import Any, Literal

from pydantic import BaseModel, Field, field_validator


class CodeIn(BaseModel):
    code: str = Field(..., description="Six-digit TOTP code.")

    @field_validator("code")
    @classmethod
    def strip_separators(cls, value: str) -> str:
        return "".join(ch for ch in value if ch.isdigit())


class PowerIn(BaseModel):
    action: Literal["on", "off", "toggle"] = "toggle"


class PreferencesIn(BaseModel):
    confirm_before_power_off: bool | None = None
    animations: bool | None = None
    poll_interval: int | None = Field(default=None, ge=1, le=60)

    def changes(self) -> dict[str, Any]:
        return {k: v for k, v in self.model_dump().items() if v is not None}


class AuthStatusOut(BaseModel):
    enrolled: bool
    authenticated: bool
    locked_for: int = 0


class MachineOut(BaseModel):
    name: str
    status: Literal["offline", "booting", "online", "shutting_down"]
    label: str
    description: str
    is_on: bool
    transitioning: bool
    progress: int
    eta_seconds: int
    changed_at: float | None = None
    since_seconds: int
    last_action: dict[str, Any] | None = None
    boot_count: int
    can_power_on: bool
    can_power_off: bool


class PreferencesOut(BaseModel):
    confirm_before_power_off: bool
    animations: bool
    poll_interval: int


class StateOut(BaseModel):
    machine: MachineOut
    preferences: PreferencesOut
    server_time: float
