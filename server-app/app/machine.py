"""Power state for the remote machine.

The transitions are simulated: asking for power-on parks the machine in
``booting`` for ``transition_seconds`` before it settles on ``online``. Swap
``_dispatch`` for a Wake-on-LAN packet or an SSH shutdown when you wire this up
to real hardware — the rest of the app only ever reads the state below.
"""

from __future__ import annotations

import time
from typing import Any, Literal

from .config import Settings
from .storage import StateStore

Status = Literal["offline", "booting", "online", "shutting_down"]
TRANSIENT: dict[str, Status] = {"booting": "online", "shutting_down": "offline"}

LABELS: dict[str, str] = {
    "offline": "Offline",
    "booting": "Materialising",
    "online": "Online",
    "shutting_down": "Dematerialising",
}
DESCRIPTIONS: dict[str, str] = {
    "offline": "The console room is dark. Power is cut.",
    "booting": "Time rotor spinning up — boot sequence in progress.",
    "online": "All systems nominal. Ready when you are.",
    "shutting_down": "Powering down — flushing the console.",
}


class MachineError(Exception):
    """Raised when a power command cannot be honoured right now."""


class MachineController:
    def __init__(self, store: StateStore, settings: Settings) -> None:
        self._store = store
        self._settings = settings

    # ------------------------------------------------------------------ read

    def state(self) -> dict[str, Any]:
        """Current machine state, resolving any transition that has elapsed."""
        machine = self._store.get("machine")
        status = str(machine.get("status") or "offline")
        changed_at = machine.get("changed_at")

        if status in TRANSIENT and changed_at is not None:
            elapsed = time.time() - float(changed_at)
            if elapsed >= self._settings.transition_seconds:
                machine = self._store.update(
                    "machine",
                    {
                        "status": TRANSIENT[status],
                        "changed_at": time.time(),
                        "target": None,
                    },
                )
                status = str(machine["status"])

        return self._decorate(machine)

    def _decorate(self, machine: dict[str, Any]) -> dict[str, Any]:
        status = str(machine.get("status") or "offline")
        changed_at = machine.get("changed_at")
        elapsed = time.time() - float(changed_at) if changed_at else 0.0
        transitioning = status in TRANSIENT

        remaining = 0
        progress = 100
        if transitioning:
            total = max(1, self._settings.transition_seconds)
            remaining = max(0, round(total - elapsed))
            progress = max(0, min(100, round(elapsed / total * 100)))

        return {
            "name": self._settings.machine_name,
            "status": status,
            "label": LABELS.get(status, status.title()),
            "description": DESCRIPTIONS.get(status, ""),
            "is_on": status == "online",
            "transitioning": transitioning,
            "progress": progress,
            "eta_seconds": remaining,
            "changed_at": changed_at,
            "since_seconds": int(elapsed),
            "last_action": machine.get("last_action"),
            "boot_count": machine.get("boot_count", 0),
            "can_power_on": status == "offline",
            "can_power_off": status == "online",
        }

    # ----------------------------------------------------------------- write

    def power_on(self) -> dict[str, Any]:
        return self._command("on")

    def power_off(self) -> dict[str, Any]:
        return self._command("off")

    def toggle(self) -> dict[str, Any]:
        current = self.state()
        if current["transitioning"]:
            raise MachineError(f"{current['name']} is already {current['label'].lower()}.")
        return self._command("off" if current["is_on"] else "on")

    def _command(self, action: str) -> dict[str, Any]:
        current = self.state()
        if current["transitioning"]:
            raise MachineError(f"{current['name']} is already {current['label'].lower()}.")
        if action == "on" and current["is_on"]:
            raise MachineError(f"{current['name']} is already online.")
        if action == "off" and not current["is_on"]:
            raise MachineError(f"{current['name']} is already offline.")

        self._dispatch(action)

        updates: dict[str, Any] = {
            "status": "booting" if action == "on" else "shutting_down",
            "changed_at": time.time(),
            "target": "online" if action == "on" else "offline",
            "last_action": {"action": action, "at": time.time()},
        }
        if action == "on":
            updates["boot_count"] = int(current.get("boot_count") or 0) + 1

        return self._decorate(self._store.update("machine", updates))

    def _dispatch(self, action: str) -> None:
        """Hook for the real power command (Wake-on-LAN, SSH, PDU, ...).

        Simulated for now — the state machine above is the whole behaviour.
        """
