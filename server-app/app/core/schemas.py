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
    """``action`` accepts the wire names and the short aliases in
    ``machine.ACTION_ALIASES`` (``on``, ``off``, ``hard_off``, …)."""

    action: Literal[
        "toggle",
        "on",
        "power_on",
        "off",
        "shutdown",
        "graceful_shutdown",
        "hard_off",
        "force_off",
        "hard_power_off",
    ] = "toggle"


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
    linked: bool
    transitioning: bool
    awaiting_ack: bool
    pending_action: str | None = None
    progress: int
    eta_seconds: int
    changed_at: float | None = None
    since_seconds: int
    last_action: dict[str, Any] | None = None
    last_error: dict[str, Any] | None = None
    boot_count: int
    can_power_on: bool
    can_power_off: bool
    can_hard_power_off: bool


class HardwareOut(BaseModel):
    """What the console shows about the device on the other end of the key."""

    linked: bool
    last_seen: float | None = None
    since_seconds: int | None = None
    firmware: str | None = None
    ip: str | None = None
    rssi: int | None = None
    uptime_s: int | None = None
    commands_ok: int = 0
    commands_failed: int = 0
    poll_interval: int
    link_timeout: int


class HardwarePollIn(BaseModel):
    """Everything the device tells us about itself on a poll. All optional —
    a device that only wants work can post an empty body."""

    firmware: str | None = Field(default=None, max_length=32)
    ip: str | None = Field(default=None, max_length=45)
    rssi: int | None = Field(default=None, ge=-120, le=0)
    uptime_s: int | None = Field(default=None, ge=0)
    # Only sent by a device wired to a power-sense line; ground truth when present.
    power_sense: bool | None = None


class CommandOut(BaseModel):
    """A pulse the device should deliver, repeated on every poll until acked."""

    id: str
    action: Literal["power_on", "graceful_shutdown", "hard_power_off"]
    pulse_ms: int
    requested_at: float
    expires_in: int


class HardwarePollOut(BaseModel):
    server_time: float
    poll_interval: int
    machine: MachineOut
    command: CommandOut | None = None


class HardwareAckIn(BaseModel):
    id: str = Field(..., max_length=64)
    status: Literal["completed", "failed"] = "completed"
    detail: str | None = Field(default=None, max_length=200)


class HardwareAckOut(BaseModel):
    accepted: bool
    machine: MachineOut


class PreferencesOut(BaseModel):
    confirm_before_power_off: bool
    animations: bool
    poll_interval: int


class StateOut(BaseModel):
    machine: MachineOut
    hardware: HardwareOut
    preferences: PreferencesOut
    server_time: float
