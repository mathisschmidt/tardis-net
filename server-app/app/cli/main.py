"""Typer app backing ``python -m app.cli`` and the installed ``tardis`` command."""

from __future__ import annotations

import json

import typer
from rich.console import Console
from rich.panel import Panel
from rich.table import Table

from ..core.runtime import auth_service, machine, store

app = typer.Typer(add_completion=False, no_args_is_help=True, help=__doc__)
console = Console()


@app.command()
def run(
    host: str = typer.Option(
        "0.0.0.0",
        help="Interface to bind. The default answers the LAN, which is what the "
        "phone and the hardware agent need; pass 127.0.0.1 to keep it local.",
    ),
    port: int = typer.Option(8000, help="Port to listen on."),
    reload: bool = typer.Option(False, help="Reload on code changes (development only)."),
) -> None:
    """Start the web server."""
    import uvicorn

    # Loopback-only is the one binding the ESP32 can never reach, so say so
    # rather than letting the device fail with a bare "cannot reach the server".
    if host in ("127.0.0.1", "localhost", "::1"):
        console.print(
            f"[yellow]Bound to {host} — reachable from this machine only. "
            "The hardware agent will not be able to poll; use --host 0.0.0.0 for that."
        )
    else:
        console.print(
            f"Console on http://{_lan_address()}:{port}/ — "
            "that is the address to enter in the device's portal."
        )

    uvicorn.run("app.server.app:app", host=host, port=port, reload=reload)


def _lan_address() -> str:
    """Best guess at the address other devices should use to reach us."""
    import socket

    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # No packets are sent; this just asks the routing table which local
        # address would be used to reach the outside world.
        probe.connect(("192.0.2.1", 53))
        return str(probe.getsockname()[0])
    except OSError:
        return socket.gethostbyname(socket.gethostname())
    finally:
        probe.close()


@app.command()
def status() -> None:
    """Show the machine state."""
    data = machine.state()

    table = Table(show_header=False, box=None, padding=(0, 2))
    table.add_row("Status", data["status"])
    table.add_row("Online", "yes" if data["is_on"] else "no")
    if data["transitioning"]:
        table.add_row("Progress", f"{data['progress']}% ({data['eta_seconds']}s remaining)")
    table.add_row("Boot count", str(data["boot_count"]))
    console.print(Panel(table, title=f"{data['name']} — {data['label']}", title_align="left"))

    console.print_json(json.dumps(data, default=str))


@app.command("reset-pairing")
def reset_pairing() -> None:
    """Forget the enrolled user so the next visit shows a fresh QR code."""
    auth_service.reset_user()
    print(f"Pairing cleared. Open the console to enrol again ({store.path}).")


def main() -> None:
    app()


if __name__ == "__main__":  # pragma: no cover
    main()
