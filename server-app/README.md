# server-app

A two-page web console for powering a remote machine (`tardis`) on and off,
protected by a single TOTP-enrolled operator.

- **`/login`** — pairs an authenticator app on first run (QR code + setup key +
  confirmation code), and asks only for the six-digit code from then on.
- **`/`** — the console: one animated power button, live status, and a couple of
  preferences that live on the server.
- **`/api/*`** — the same capabilities as JSON, documented at `/api/docs`.

Built with FastAPI + Jinja templates, htmx for every network call, Alpine for
rendering and animation, and Tailwind CSS v4 for the nocturne styling. No build
step is needed to run it — the compiled stylesheet and both JS libraries are
vendored under `app/static/`.

## Run it

```bash
cd server-app
python -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt
uvicorn app.main:app --reload --port 8000
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

## Security notes

- One user, one TOTP secret. Codes are accepted within ±30 s of clock drift and
  each 30-second code is single-use, so a captured code cannot be replayed.
- Five failed codes lock the form for five minutes (both configurable).
- The session cookie is signed (`itsdangerous`), `HttpOnly` and `SameSite=Lax`.
- `data/state.json` holds the TOTP secret and the cookie signing key. It is
  written `0600` and is git-ignored — keep it that way.

## Maintenance

```bash
python -m app.cli status          # machine state as JSON
python -m app.cli state           # whole state file, secrets redacted
python -m app.cli reset-pairing   # forget the user, show a fresh QR next visit
```

## Wiring it to real hardware

Power commands are simulated: `on` parks the machine in `booting` for
`TARDIS_TRANSITION_SECONDS` before it settles on `online`. Replace
`MachineController._dispatch` in [`app/machine.py`](app/machine.py) with the
real command — a Wake-on-LAN magic packet, an SSH `shutdown`, a smart-plug call
— and have `state()` read the machine's true status (e.g. a TCP ping). Nothing
else in the app needs to change.

## Development

```bash
pip install -r requirements-dev.txt
pytest                   # 19 tests covering enrolment, login, API and fragments

npm install              # only needed to change the styling
npm run build            # rebuild app/static/css/app.css
npm run watch            # …or rebuild on save
```

`app/static/css/app.css` is committed on purpose, so deploying never requires
Node. Rebuild and commit it whenever you touch templates or
`app/static/css/tailwind.css`.
