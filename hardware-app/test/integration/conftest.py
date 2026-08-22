"""Put server-app on the import path so the integration test can drive it."""

from __future__ import annotations

import sys
from pathlib import Path

SERVER_APP = Path(__file__).resolve().parents[3] / "server-app"
if str(SERVER_APP) not in sys.path:
    sys.path.insert(0, str(SERVER_APP))
