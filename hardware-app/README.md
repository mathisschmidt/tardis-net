# hardware-app

An ESP32 wired across the front-panel power switch of the machine called
`tardis`. It asks [`server-app`](../server-app) what to do every few seconds and,
when told to, holds the switch closed for exactly as long as the action needs:

| Action | Pulse | What it does to an ATX machine |
| --- | --- | --- |
| `power_on` | ~500 ms | a short press starts the machine |
| `graceful_shutdown` | ~500 ms | the same press asks a running OS to shut down |
| `hard_power_off` | ~5 s | holding the button cuts power immediately |

Everything the device needs — Wi-Fi, the console's address, the API key — is
entered in its own web portal, which is password-protected from the very first
connection.

## First run

No computer or serial monitor is needed for any of this — a phone is enough.

1. **Flash it.** `pio run -t upload` (a monitor is only useful for
   watching the log, not required to set the device up).
2. **Join the setup network.** With nothing configured the device opens an
   open (no-password) access point called `tardis-setup-XXXX` — `XXXX` is a
   short id unique to that board. Join it from your phone's Wi-Fi settings and
   a **"Sign in to network" prompt should open the portal by itself** within a
   few seconds — the device answers every DNS lookup with its own address
   specifically to trigger that captive-portal prompt. If it does not appear,
   open a browser and go to <http://192.168.4.1/>.
3. **Claim the portal.** Nobody has set a password yet, so the first screen
   asks you to choose one. From then on that page always asks for it — the
   same shape as the console's "enrol once, then just the code".
4. **Configure it.** Fill in the Wi-Fi network — tap *Scan* to pick it from
   what the device can see, and *Test* to check the password before saving —
   the console address (`http://<host>:8000`) and the **pairing key** copied
   from the console's dashboard, then save and reboot. The device joins your
   network, the setup AP closes, and the portal stays reachable at the
   device's LAN address, **and also at `http://tardis-XXXX.local/`** (the same
   `XXXX` id from the setup network name) — no need to hunt for its IP on the
   router.
5. **Check the console.** The dashboard's *Hardware link* card turns to
   `Polling` within a few seconds, and the power button starts working.

Revisiting the portal later shows the current configuration and lets you change
it. Secrets are never sent back to the browser: the Wi-Fi password and API key
show as `•••••••• (stored)`, and leaving a field blank keeps what is saved.

## Wiring

The ESP32 does not switch mains. It closes the same two pins the case button
shorts — a dry contact across the motherboard's `PWR_SW` header.

```
   ESP32                       relay / opto             motherboard
   ┌──────────┐                ┌─────────┐              ┌────────────┐
   │ GPIO 26  ├───────────────►│ IN      │              │  PWR_SW  ○ │
   │          │                │      NO ├──────────────┤          │ │
   │ GND      ├───────────────►│ GND  COM├──────────────┤          ○ │
   │ 5V/3V3   ├───────────────►│ VCC     │              └────────────┘
   └──────────┘                └─────────┘
```

- **Any isolated dry-contact output works**: a small relay module, a solid-state
  relay, or an optocoupler (a 4N35 with a 220 Ω series resistor is plenty — the
  header is 3.3 V at a few mA). Isolation is the point: the ESP32 and the
  machine keep separate grounds.
- **Set the polarity in the portal.** Most relay boards close on `LOW`; the
  portal's *Closes on* selector covers both, and the firmware writes the
  released level *before* the pin becomes an output so a reset cannot glitch the
  button.
- **Optional power-sense input.** Wire a GPIO to something that is high only
  while the machine runs (a 5 V-standby-referenced divider off a `PWR_LED`
  header, an optocoupler across the LED). Set that pin in the portal and the
  device reports the machine's true state on every poll — the console then shows
  reality instead of what it last asked for, including someone pressing the
  physical button. Leave it at `-1` to disable.
- **Do not use GPIO 6-11** (SPI flash). GPIO 34-39 are input-only, so they are
  valid for sense and refused for the switch. The portal enforces both.

## How it talks to the console

Two calls, both authenticated with `X-Tardis-Key: <pairing key>` — never a
session cookie, which is what the operator's browser uses:

```
POST /api/hardware/poll     {"firmware":"1.0.0","ip":"…","rssi":-57,"uptime_s":42,
                             "power_sense":true}
 200 {"server_time":…, "poll_interval":5, "machine":{…},
      "command": null | {"id":"…","action":"power_on","pulse_ms":500,"expires_in":57}}

POST /api/hardware/ack      {"id":"…","status":"completed"}
 200 {"accepted":true,"machine":{…}}
```

- The console only moves the machine to `online`/`offline` when **this device
  acks**. Until then it shows the transition as in progress.
- An unacked command is repeated on every poll, so a dropped response costs one
  cycle. A command nobody acks within 60 s expires and the console reports the
  failure.
- `409` on an ack means the command already expired or was settled — the device
  drops it instead of retrying. `401` means the key is wrong, and the portal
  says so in plain words.
- The cadence comes from the server (`poll_interval`), so it can be changed
  once on the console instead of reflashing. The device's own setting is the
  fallback when the server does not say.

**If the portal's "Test" (in the Console box) reports it cannot reach the
server, check how `server-app` was started first.** `tardis run` defaults to
`--host 127.0.0.1` — loopback only, reachable from `curl` on that same
machine but invisible to a separate physical device like the ESP32. Start it
with `tardis run --host 0.0.0.0` (or the machine's LAN IP) so the console
actually answers requests arriving over Wi-Fi.

## The portal's own API

Served by the device on port 80. The claim-then-login rule mirrors the console.

| Method | Path | Auth |
| --- | --- | --- |
| `GET` | `/` | public — the UI itself |
| `GET` | `/api/status` | public: booleans only (claimed, configured, Wi-Fi), link detail requires the session |
| `POST` | `/api/claim` | only while unclaimed — sets the portal password |
| `POST` | `/api/login` | password → session cookie |
| `POST` | `/api/logout` | — |
| `GET`/`POST` | `/api/config` | session cookie |
| `POST` | `/api/test` | session cookie — polls the console once and reports what came back |
| `GET` | `/api/wifi/scan` | session cookie — nearby SSIDs, signal and whether each is secured |
| `POST` | `/api/wifi/test` | session cookie — briefly joins a network to check credentials, then restores the saved one |
| `POST` | `/api/reboot` | session cookie |

Security notes:

- The password is stored as a random 16-byte salt plus 20 000 iterations of
  SHA-256 (`include/password.h`) — a stolen flash dump does not hand over the
  password.
- Sessions are a random 128-bit token in an `HttpOnly` cookie, 30 minutes of
  idle time, one operator at a time. Five wrong passwords lock the form for five
  minutes.
- The API key and Wi-Fi password are write-only over the portal API: they go in,
  and only a masked hint (`••••••••sROK`) comes back.
- The setup AP is open (no password) by design, so a phone can join it without
  being told a secret first — it only exists until the device is claimed and
  configured, at which point it closes.
- mDNS (`http://tardis-XXXX.local/`) is the most reliable on iOS/macOS. Some
  Android browsers do not resolve `.local` names; the device's LAN IP (visible
  in `pio device monitor` or your router's client list) always works as a
  fallback.

## Development

```bash
make test        # everything that runs without an ESP32
make unit        # protocol, pulse timing, config rules, SHA-256 (native g++)
make integration # the firmware's flow against the real server-app API
make portal      # serve the config UI at http://127.0.0.1:8090 with a mock device
```

`make portal` runs [`tools/mock_device.py`](tools/mock_device.py), which
re-implements this firmware's HTTP API in Python so the UI can be worked on (and
browser-tested) without hardware. `make integration` runs the device's poll →
pulse → ack loop against a real in-process `server-app`, asserting the pulse
lengths in the table above — if the two sides ever drift apart, that is what
fails.

```
include/    portable core: protocol, pulse state machine, config rules, SHA-256
src/        the ESP32 layers: NVS, Wi-Fi, portal, agent
web/        the portal UI (embedded into the firmware at build time)
tools/      the embed script and the host mock
test/       host unit tests + the cross-app integration test
```

`include/web_assets.h` is generated from `web/portal.html` on every build and is
not committed.

### Not built here

The firmware in `src/` has not been compiled — the PlatformIO package registry
was unreachable from the environment this was written in, so the toolchain could
not be installed. Everything in `include/` is compiled and tested natively
(`make unit`, 119 checks), the portal UI was driven in a real browser against
the mock, and the protocol was exercised against the real server
(`make integration`) — but expect the first `pio run` to want small fixes in the
Arduino-facing files.
