"""Process-wide singletons shared by the CLI and the server.

Both entry points talk to the same store, auth service and machine controller —
this is the one place that constructs them, so neither side has to import
from the other.
"""

from __future__ import annotations

from .auth import AuthService
from .config import settings
from .machine import MachineController
from .storage import StateStore

store = StateStore(settings.state_file)
auth_service = AuthService(store, settings)
machine = MachineController(store, settings)
