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
| `TARDIS_TRANSITION_SECONDS` | Length of the simulated boot / shutdown sequence. |

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
| `POST` | `/api/machine/power` | `{"action": "on" \| "off" \| "toggle"}`. |
| `GET`/`PUT` | `/api/preferences` | Persisted UI preferences. |

Everything except `/api/health` and the auth endpoints requires the session
cookie. `409` means the machine is mid-transition; `429` means the code form is
locked out after too many failures.

`POST /api/hardware/poll` is separate: it's the hardware side's heartbeat, so
it authenticates with `X-Tardis-Key: <pairing key>` instead of the session
cookie. See [Wiring it to real hardware](#wiring-it-to-real-hardware).

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

Power commands are currently simulated: `on` parks the machine in `booting`
for `TARDIS_TRANSITION_SECONDS` before it settles on `online` on a timer, with
no real hardware involved. The target design replaces that timer with a
pairing key and an acknowledgement handshake, so the console only reports
`online`/`offline` once the physical machine has actually confirmed it:

1. **Pairing key.** The first time it's needed, the server generates a random
   16-character key (`MachineController.pairing_key` in
   [`app/core/machine.py`](app/core/machine.py)) and persists it in
   `data/state.json` next to the TOTP secret and cookie signing key. It's
   shown on the dashboard so the operator can copy it onto the hardware side
   once, out of band, when the device is set up — the server never sends it
   anywhere after that.
2. **Operator presses the power button.** The console records the requested
   action (`on`/`off`) as *pending* and starts a 60-second ack timer. At this
   point the machine's status is a waiting state — not yet `online`/`offline`
   — the console shows it as still transitioning.
3. **Hardware polls for status.** The device on the `tardis` machine calls
   `POST /api/hardware/poll`, authenticating with `X-Tardis-Key: <pairing
   key>` instead of the operator's session cookie. Each poll records the
   server's `last_seen` time for the hardware — the console considers it
   *linked* only while a poll has landed in the last 60 seconds, and shows
   "Not linked" instead of a possibly-stale online/offline state otherwise.
   Once the poll also carries the pending change, the device performs the
   real action (Wake-on-LAN packet, SSH `shutdown`, smart-plug toggle, …).
4. **Hardware acknowledges.** After acting, the device calls back into the
   server — again authenticated with the pairing key — to ack that specific
   change. Only this ack, from a caller that knows the key, is trusted to
   confirm the machine actually changed state.
5. **Server resolves the transition.** Once the ack for the pending change
   arrives, the server flips the machine to `online` (or `offline`) and the
   console reflects it.
6. **Timeout.** If no ack arrives within 60 seconds of the request, the
   server cancels the pending change, reverts the machine to its last known
   state, and the console surfaces an error instead of quietly sitting in
   `booting`/`shutting_down` forever.

So far, steps 1 and 3 are implemented as a heartbeat: the pairing key
(generation, persistence, dashboard display) and `POST /api/hardware/poll`
(records `last_seen`, drives the `linked` flag on `MachineOut` and the
"Not linked" state of the console's status pill). `MachineController._dispatch`
is still the timer-based stub, and nothing yet carries a pending change
through the poll or listens for an ack — building the rest means adding
that payload to the poll response, an ack endpoint, and replacing the
elapsed-time check in `MachineController.state()` with a
pending/ack/timeout state machine.

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
pytest                   # 19 tests covering enrolment, login, API and fragments
```

`app/static/css/app.css` is committed and hand-maintained — there is no build
step and no Node/npm dependency. Edit it directly when styling changes.
