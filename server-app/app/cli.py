"""Small maintenance CLI: ``python -m app.cli <command>``."""

from __future__ import annotations

import argparse
import json

from .dependencies import auth_service, machine, store


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m app.cli", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status", help="Print the machine state as JSON.")
    sub.add_parser("state", help="Dump the whole state file (secrets redacted).")
    sub.add_parser(
        "reset-pairing",
        help="Forget the enrolled user so the next visit shows a fresh QR code.",
    )

    args = parser.parse_args(argv)

    if args.command == "status":
        print(json.dumps(machine.state(), indent=2, default=str))
    elif args.command == "state":
        snapshot = store.snapshot()
        if snapshot.get("secret_key"):
            snapshot["secret_key"] = "***"
        if snapshot.get("user"):
            snapshot["user"]["totp_secret"] = "***"
        if snapshot.get("pending_secret"):
            snapshot["pending_secret"] = "***"
        print(json.dumps(snapshot, indent=2, sort_keys=True))
    elif args.command == "reset-pairing":
        auth_service.reset_user()
        print(f"Pairing cleared. Open the console to enrol again ({store.path}).")

    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
