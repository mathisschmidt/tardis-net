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
    host: str = typer.Option("127.0.0.1", help="Interface to bind."),
    port: int = typer.Option(8000, help="Port to listen on."),
    reload: bool = typer.Option(False, help="Reload on code changes (development only)."),
) -> None:
    """Start the web server."""
    import uvicorn

    uvicorn.run("app.server.app:app", host=host, port=port, reload=reload)


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
