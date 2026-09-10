# server-app

A two-page web console for powering a remote machine (`tardis`) on and off,
protected by a single TOTP-enrolled operator.

- **`/login`** — pairs an authenticator app on first run (QR code + setup key +
  confirmation code), and asks only for the six-digit code from then on.
- **`/`** — the console: one animated power button, live status, and a couple of
  preferences that live on the server.
- **`/api/*`** — the same capabilities as JSON, documented at `/api/docs`.

Built with FastAPI + Jinja templates, htmx for every network call, Alpine for
rendering and animation, and hand-maintained Tailwind-generated CSS for the
nocturne styling. No build step is needed to run it — the stylesheet and both
JS libraries are vendored under `app/static/`.

## Run it

```bash
cd server-app
python -m venv .venv && . .venv/bin/activate
pip install .
tardis run --reload --port 8000
```

`tardis run` binds `0.0.0.0` so the phone and the hardware agent on the LAN can
reach it; pass `--host 127.0.0.1` to keep it to this machine (the ESP32 cannot
poll a loopback-only console, and the command says so when you do).

Open <http://localhost:8000>. The first visit shows the enrolment QR code; scan
it with any TOTP app (Aegis, 1Password, Google Authenticator…), type the code,
and you are on the console. State is written to `data/state.json`.

## Configuration

Every setting is an environment variable — see [`.env.example`](.env.example)
for the full list and defaults. The ones worth setting in production:

| Variable | Purpose |
| --- | --- |
| `TARDIS_SECRET_KEY` | Signs the session cookie. Generated and persisted on first boot if unset. |
| `TARDIS_COOKIE_SECURE` | Set to `true` once the console is served over HTTPS. |
| `TARDIS_MACHINE` | Name of the machine shown throughout the UI. |
| `TARDIS_STATE_FILE` | Where the JSON state document lives. |
| `TARDIS_ACK_TIMEOUT_SECONDS` | How long the hardware has to pulse the switch and acknowledge (default 60). |
| `TARDIS_HARDWARE_POLL_SECONDS` | Cadence served to the device on every poll (default 5). |
| `TARDIS_LINK_TIMEOUT_SECONDS` | Older than this and the console stops trusting the cached power state (default 30). |

## API

| Method | Path | Notes |
| --- | --- | --- |
| `GET` | `/api/health` | Public liveness probe. |
| `GET` | `/api/auth/status` | Whether a user is enrolled / the caller is signed in. |
| `POST` | `/api/auth/enroll` | `{"code": "123456"}` — confirms the first pairing. |
| `POST` | `/api/auth/login` | `{"code": "123456"}` — sets the session cookie. |
| `POST` | `/api/auth/logout` | Clears the session cookie. |
| `GET` | `/api/state` | Machine state + preferences in one call. |
| `GET` | `/api/machine` | Machine state only. |
| `POST` | `/api/machine/power` | `{"action": "on" \| "off" \| "hard_off" \| "toggle"}` — queues a command for the hardware. |
| `GET` | `/api/hardware` | Device link status for the dashboard. |
| `GET`/`PUT` | `/api/preferences` | Persisted UI preferences. |

Everything except `/api/health` and the auth endpoints requires the session
cookie — including `/api/docs` and the OpenAPI schema. `409` means the machine
is mid-transition; `429` means the code form is locked out after too many
failures.

The two hardware endpoints are separate. They authenticate with
`X-Tardis-Key: <pairing key>` and **only** that — an operator's session cookie
does not open them, and the pairing key does not open anything else:

| Method | Path | Notes |
| --- | --- | --- |
| `POST` | `/api/hardware/poll` | Heartbeat + "what should I do?"; returns the pending command. |
| `POST` | `/api/hardware/ack` | "I pulsed the switch" — the only thing that settles a transition. |

See [Wiring it to real hardware](#wiring-it-to-real-hardware).

## Security notes

- One user, one TOTP secret. Codes are accepted within ±30 s of clock drift and
  each 30-second code is single-use, so a captured code cannot be replayed.
- Five failed codes lock the form for five minutes (both configurable).
- The session cookie is signed (`itsdangerous`), `HttpOnly` and `SameSite=Lax`.
- `data/state.json` holds the TOTP secret, the cookie signing key, and the
  hardware pairing key. It is written `0600` and is git-ignored — keep it
  that way.
- The pairing key is shown on the dashboard, behind the same session cookie
  as everything else — only the signed-in operator sees it.

## Maintenance

The `tardis` console script (installed with the package) is also reachable as
`python -m app.cli` without installing it.

```bash
tardis run                # start the server (wraps uvicorn)
tardis status             # machine state, rendered with rich
tardis reset-pairing      # forget the user, show a fresh QR next visit
```

## Wiring it to real hardware

Power commands are not simulated: the console holds a command until the device
in [`hardware-app`](../hardware-app) picks it up, pulses `tardis`'s front-panel
switch, and says so.

1. **Pairing key.** The server generates a random 16-character key on first use
   (`MachineController.pairing_key`) and persists it in `data/state.json`. It is
   shown on the dashboard so it can be copied into the device's config portal
   once, out of band.
2. **Operator presses the power button.** The console records the action as
   *pending* — `power_on`, `graceful_shutdown` or `hard_power_off` — and shows
   the machine as transitioning. It does **not** claim the machine is on.
3. **Hardware polls.** `POST /api/hardware/poll` returns the pending command
   with the hold time the device should use:

   | Action | Pulse |
   | --- | --- |
   | `power_on` | 500 ms |
   | `graceful_shutdown` | 500 ms |
   | `hard_power_off` | 5 s |

   The same command is repeated on every poll until it is acked, so a dropped
   response costs one cycle and nothing more.
4. **Hardware acknowledges.** `POST /api/hardware/ack` with the command id. This
   only retires the *command* — counted as delivered or failed — and the
   machine's status reverts to wherever it was before the command was
   requested. An ack never asserts `online` or `offline` on its own; only a
   power-sense report does that (see step 6).
5. **Timeout.** No ack within `TARDIS_ACK_TIMEOUT_SECONDS` and the command
   expires: the machine reverts to its previous state and the console shows what
   failed.
6. **Power sense — the only source of truth for `online`/`offline`.** Every
   poll carries `power_sense`: `"on"`, `"off"`, or `"unknown"` from a device
   with no sense pin fitted. A device that never reports `"on"`/`"off"` leaves
   the machine `unknown` forever, ack or no ack — the console would rather say
   "I don't know" than guess. When a sense line is wired up, its report wins
   over anything inferred from a command, so the console stays right even when
   someone presses the physical button.

Each poll also records `last_seen`; without one inside
`TARDIS_LINK_TIMEOUT_SECONDS` the console shows **Not linked** rather than a
stale online/offline. Commands are still accepted while unlinked — they simply
expire if the device never comes back.

## Layout

```
app/
  core/     config, storage, auth, machine, schemas — no FastAPI/CLI imports
  server/   FastAPI app, routers, dependency wiring, templates
  cli/      the `tardis` command (typer)
```

`app/core` is the shared layer: both `app/server` and `app/cli` depend on it
(via `app/core/runtime.py`, which builds the one store/auth/machine instance
each process uses), and never on each other.

## Development

```bash
pip install -e ".[dev]"
pytest                   # 34 tests: enrolment, login, API, fragments, hardware protocol
```

`app/static/css/app.css` is committed and hand-maintained — there is no build
step and no Node/npm dependency. Edit it directly when styling changes.
