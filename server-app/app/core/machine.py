"""Power state for the remote machine, and the command queue the hardware drains.

The server never touches a wire itself. It holds at most one *pending command*
for the ESP32 sitting on ``tardis``'s front-panel switch; the device picks the
command up on its next poll, pulses the switch for ``pulse_ms``, and acks. An
unacked command expires after ``ack_timeout_seconds`` and the machine reverts
to where it was, with the failure surfaced to the console.

An ack only settles the *command* — it retires the pending pulse and counts it
as delivered or failed. It never asserts the machine's power state: an ack is
"we pulsed the switch", not "the machine is now on". The only thing that ever
moves ``status`` towards "online"/"offline" is a power-sense report on a poll,
which is why a device with no sense pin fitted reports "unknown" rather than
omitting it — the console would otherwise have no honest way to say it simply
doesn't know, and always waits for that report rather than the ack.
"""

from __future__ import annotations

import secrets
import string
import time
from typing import Any, Literal

from .config import Settings
from .storage import StateStore

Status = Literal["unknown", "offline", "booting", "online", "shutting_down"]
PowerSense = Literal["on", "off", "unknown"]
Action = Literal["power_on", "graceful_shutdown", "hard_power_off"]

TRANSIENT: dict[str, Status] = {"booting": "online", "shutting_down": "offline"}

# What the device does for each action: hold the front-panel switch this long.
# A short press boots an ATX machine or asks a running OS to shut down; a five
# second hold cuts power the way the physical button does.
PULSE_MS: dict[str, int] = {
    "power_on": 500,
    "graceful_shutdown": 500,
    "hard_power_off": 5000,
}
ACTION_TARGET: dict[str, Status] = {
    "power_on": "online",
    "graceful_shutdown": "offline",
    "hard_power_off": "offline",
}
ACTION_TRANSIENT: dict[str, Status] = {
    "power_on": "booting",
    "graceful_shutdown": "shutting_down",
    "hard_power_off": "shutting_down",
}
ACTION_LABELS: dict[str, str] = {
    "power_on": "Power on",
    "graceful_shutdown": "Graceful shutdown",
    "hard_power_off": "Hard power off",
}

# Names the API accepts for each action, so a phone shortcut can post {"action":
# "off"} without knowing the wire vocabulary.
ACTION_ALIASES: dict[str, Action] = {
    "on": "power_on",
    "power_on": "power_on",
    "off": "graceful_shutdown",
    "shutdown": "graceful_shutdown",
    "graceful_shutdown": "graceful_shutdown",
    "hard_off": "hard_power_off",
    "force_off": "hard_power_off",
    "hard_power_off": "hard_power_off",
}

PAIRING_KEY_LENGTH = 16
_PAIRING_KEY_ALPHABET = string.ascii_letters + string.digits

LABELS: dict[str, str] = {
    "unknown": "Unknown",
    "offline": "Offline",
    "booting": "Materialising",
    "online": "Online",
    "shutting_down": "Dematerialising",
}
DESCRIPTIONS: dict[str, str] = {
    "unknown": "No power-sense wire fitted — the console cannot confirm the real state.",
    "offline": "The console room is dark. Power is cut.",
    "booting": "Time rotor spinning up — boot sequence in progress.",
    "online": "All systems nominal. Ready when you are.",
    "shutting_down": "Powering down — flushing the console.",
}


class MachineError(Exception):
    """Raised when a power command cannot be honoured right now."""


def normalise_action(action: str) -> Action:
    """Map an API alias onto a wire action, or raise ``MachineError``."""
    try:
        return ACTION_ALIASES[(action or "").strip().lower()]
    except KeyError:
        raise MachineError(f"Unknown action {action!r}.") from None


class MachineController:
    def __init__(self, store: StateStore, settings: Settings) -> None:
        self._store = store
        self._settings = settings

    # --------------------------------------------------------------- pairing

    @property
    def pairing_key(self) -> str:
        """Key the hardware side authenticates its polls and acks with.

        Generated once on first access and persisted, the same lazy pattern
        ``AuthService`` uses for the cookie-signing secret.
        """
        stored = self._store.raw("pairing_key")
        if not stored:
            stored = "".join(
                secrets.choice(_PAIRING_KEY_ALPHABET) for _ in range(PAIRING_KEY_LENGTH)
            )
            self._store.set("pairing_key", stored)
        return stored

    @property
    def linked(self) -> bool:
        """Whether the hardware has polled within ``link_timeout_seconds``.

        Without a recent poll, any cached power state is unverified — the
        console should say so instead of asserting online/offline.
        """
        last_seen = self._store.get("hardware").get("last_seen")
        if last_seen is None:
            return False
        return (time.time() - float(last_seen)) < self._settings.link_timeout_seconds

    def hardware(self) -> dict[str, Any]:
        """What the console shows about the device itself."""
        info = self._store.get("hardware")
        last_seen = info.get("last_seen")
        return {
            **info,
            "linked": self.linked,
            "since_seconds": int(time.time() - float(last_seen)) if last_seen else None,
            "poll_interval": self._settings.hardware_poll_seconds,
            "link_timeout": self._settings.link_timeout_seconds,
        }

    def record_hardware_poll(
        self,
        *,
        firmware: str | None = None,
        ip: str | None = None,
        rssi: int | None = None,
        uptime_s: int | None = None,
        power_sense: PowerSense | None = None,
    ) -> None:
        """Record a heartbeat, and apply the power-sense report — the only thing
        that ever moves ``status``.

        Only the detail fields the device actually reported are written — an
        ack, or a poll with an empty body, must not blank out what an earlier
        poll told us. ``power_sense`` is different: ``None`` here means the
        caller (the ack route) is not reporting sense at all, not that the
        device is unsure, so it alone is left untouched in that case.
        """
        reported = {"firmware": firmware, "ip": ip, "rssi": rssi, "uptime_s": uptime_s}
        self._store.update(
            "hardware",
            {"last_seen": time.time(), **{k: v for k, v in reported.items() if v is not None}},
        )
        if power_sense == "on":
            self._apply_power_sense(True)
        elif power_sense == "off":
            self._apply_power_sense(False)
        elif power_sense == "unknown":
            self._apply_unknown_sense()

    def _apply_power_sense(self, powered: bool) -> None:
        """A sense line is ground truth — it also settles a pending transition."""
        machine = self._store.get("machine")
        observed: Status = "online" if powered else "offline"
        pending = machine.get("pending")

        # While a command is in flight the machine is expected to be in its old
        # state for a moment; only let the sense line settle it once it agrees
        # with where that command was heading.
        if pending and ACTION_TARGET[pending["action"]] != observed:
            return
        if machine.get("status") == observed:
            return

        updates: dict[str, Any] = {
            "status": observed,
            "changed_at": time.time(),
            "target": None,
            "pending": None,
            "last_error": None,
        }
        # A boot is a boot whether we asked for it or someone pressed the button.
        if observed == "online":
            updates["boot_count"] = int(machine.get("boot_count") or 0) + 1
        self._store.update("machine", updates)

    def _apply_unknown_sense(self) -> None:
        """The device has just told us, honestly, that it cannot tell.

        Leaves a transition the console requested alone — that guess is more
        useful than blanking it mid-flight — but otherwise "unknown" is as much
        ground truth as "on" or "off" and overrides any stale cached status.
        """
        machine = self._store.get("machine")
        if machine.get("pending"):
            return
        if machine.get("status") == "unknown":
            return
        self._store.update(
            "machine",
            {"status": "unknown", "changed_at": time.time(), "target": None, "last_error": None},
        )

    # ------------------------------------------------------------------ read

    def state(self) -> dict[str, Any]:
        """Current machine state, expiring a pending command that timed out."""
        machine = self._store.get("machine")
        pending = machine.get("pending")

        if pending and self._expired(pending):
            machine = self._store.update(
                "machine",
                {
                    "status": pending.get("previous_status") or "unknown",
                    "changed_at": time.time(),
                    "target": None,
                    "pending": None,
                    "last_error": {
                        "message": (
                            f"{ACTION_LABELS[pending['action']]} was never confirmed by the "
                            f"hardware — check that the device is powered and online."
                        ),
                        "at": time.time(),
                    },
                },
            )

        return self._decorate(machine)

    def _expired(self, pending: dict[str, Any]) -> bool:
        age = time.time() - float(pending["requested_at"])
        return age >= self._settings.ack_timeout_seconds

    def _decorate(self, machine: dict[str, Any]) -> dict[str, Any]:
        status = str(machine.get("status") or "offline")
        changed_at = machine.get("changed_at")
        elapsed = time.time() - float(changed_at) if changed_at else 0.0
        pending = machine.get("pending")
        transitioning = pending is not None

        remaining = 0
        progress = 100
        if pending:
            total = max(1, self._settings.ack_timeout_seconds)
            waited = time.time() - float(pending["requested_at"])
            remaining = max(0, round(total - waited))
            progress = max(0, min(100, round(waited / total * 100)))

        # Without a sense pin the console never learns the real state, so it
        # cannot rule either direction out — both the boot pulse and the
        # shutdown pulse stay available, same as the escape-hatch hold below.
        known = status != "unknown"

        return {
            "name": self._settings.machine_name,
            "status": status,
            "label": LABELS.get(status, status.title()),
            "description": DESCRIPTIONS.get(status, ""),
            "is_on": status == "online",
            "state_known": known,
            "linked": self.linked,
            "transitioning": transitioning,
            "awaiting_ack": pending is not None and pending.get("delivered_at") is not None,
            "pending_action": pending["action"] if pending else None,
            "progress": progress,
            "eta_seconds": remaining,
            "changed_at": changed_at,
            "since_seconds": int(elapsed),
            "last_action": machine.get("last_action"),
            "last_error": machine.get("last_error"),
            "boot_count": machine.get("boot_count", 0),
            "can_power_on": (status == "offline" or not known) and not transitioning,
            "can_power_off": (status == "online" or not known) and not transitioning,
            # The 5s hold is the escape hatch: allowed whenever power may be on,
            # including out of a boot that never finished or an unknown state.
            "can_hard_power_off": status != "offline",
        }

    # ----------------------------------------------------------------- write

    def request(self, action: str) -> dict[str, Any]:
        """Queue a command for the hardware and park the machine mid-transition."""
        wire_action = normalise_action(action)
        current = self.state()

        if current["transitioning"] and wire_action != "hard_power_off":
            raise MachineError(
                f"{current['name']} is already {current['label'].lower()} — "
                "wait for the hardware to confirm."
            )
        # These "already there" guards only fire on a *confirmed* status — an
        # unknown state (no sense pin, or no report yet) can't rule either
        # direction out, so both pulses stay available rather than guessing.
        if wire_action == "power_on" and current["status"] == "online":
            raise MachineError(f"{current['name']} is already online.")
        turning_off = wire_action in ("graceful_shutdown", "hard_power_off")
        if turning_off and current["status"] == "offline" and not current["transitioning"]:
            raise MachineError(f"{current['name']} is already offline.")

        machine = self._store.get("machine")
        now = time.time()
        pending = {
            "id": secrets.token_hex(8),
            "action": wire_action,
            "pulse_ms": PULSE_MS[wire_action],
            "requested_at": now,
            "delivered_at": None,
            "previous_status": machine.get("status") or "unknown",
        }

        updates: dict[str, Any] = {
            "status": ACTION_TRANSIENT[wire_action],
            "changed_at": now,
            "target": ACTION_TARGET[wire_action],
            "pending": pending,
            "last_action": {"action": wire_action, "at": now},
            "last_error": None,
        }
        return self._decorate(self._store.update("machine", updates))

    def toggle(self) -> dict[str, Any]:
        current = self.state()
        if current["transitioning"]:
            raise MachineError(
                f"{current['name']} is already {current['label'].lower()} — "
                "wait for the hardware to confirm."
            )
        return self.request("graceful_shutdown" if current["is_on"] else "power_on")

    # -------------------------------------------------------------- hardware

    def next_command(self) -> dict[str, Any] | None:
        """The command the device should pulse now, or ``None``.

        Marks the command delivered so the console can tell "queued" from "the
        device has it". Re-delivered on every poll until acked, which is what
        makes a poll dropped mid-flight harmless.
        """
        self.state()  # expire a timed-out command before handing one out
        pending = self._store.get("machine").get("pending")
        if not pending:
            return None

        if pending.get("delivered_at") is None:
            pending = {**pending, "delivered_at": time.time()}
            self._store.update("machine", {"pending": pending})

        waited = time.time() - float(pending["requested_at"])
        return {
            "id": pending["id"],
            "action": pending["action"],
            "pulse_ms": pending["pulse_ms"],
            "requested_at": pending["requested_at"],
            "expires_in": max(0, round(self._settings.ack_timeout_seconds - waited)),
        }

    def acknowledge(
        self, command_id: str, *, status: str = "completed", detail: str | None = None
    ) -> dict[str, Any]:
        """Settle a command the device has pulsed (or failed to pulse).

        Either way this only retires the *command* — it never asserts the
        machine's power state. Whether the pulse actually worked is for the
        next power-sense report to say; until then the status reverts to
        wherever it was before the command was requested.
        """
        machine = self._store.get("machine")
        pending = machine.get("pending")
        if not pending:
            raise MachineError("No command is pending.")
        if not secrets.compare_digest(str(command_id), str(pending["id"])):
            raise MachineError("That command is no longer pending.")

        counters = self._store.get("hardware")
        if status == "completed":
            self._store.update(
                "hardware", {"commands_ok": int(counters.get("commands_ok") or 0) + 1}
            )
            last_error = None
        else:
            self._store.update(
                "hardware", {"commands_failed": int(counters.get("commands_failed") or 0) + 1}
            )
            last_error = {
                "message": (
                    f"{ACTION_LABELS[pending['action']]} failed on the hardware"
                    + (f": {detail}" if detail else ".")
                ),
                "at": time.time(),
            }

        updates: dict[str, Any] = {
            "status": pending.get("previous_status") or "unknown",
            "changed_at": time.time(),
            "target": None,
            "pending": None,
            "last_error": last_error,
        }

        return self._decorate(self._store.update("machine", updates))

    def clear_error(self) -> None:
        self._store.update("machine", {"last_error": None})
