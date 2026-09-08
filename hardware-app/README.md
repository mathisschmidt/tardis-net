# hardware-app

An ESP32 wired across the front-panel power switch of the machine called
`tardis`. It asks [`server-app`](../server-app) what to do every few seconds and,
when told to, holds the switch closed for exactly as long as the action needs:

| Action | Pulse | What it does to an ATX machine |
| --- | --- | --- |
| `power_on` | ~500 ms | a short press starts the machine |
| `graceful_shutdown` | ~500 ms | the same press asks a running OS to shut down |
| `hard_power_off` | ~5 s | holding the button cuts power immediately |

Wi-Fi and the device's own portal are two separate things now:
[WiFiManager](https://github.com/tzapu/WiFiManager) owns joining and
remembering the real network, entirely on its own; this device's own web
portal only handles the console's address and the API key, and is
password-protected from the very first connection.

## First run

No computer or serial monitor is needed for any of this — a phone is enough.

1. **Flash it.** `pio run -t upload` (a monitor is only useful for
   watching the log, not required to set the device up).
2. **Join the setup network.** With no Wi-Fi credentials stored, WiFiManager
   opens an open (no-password) access point called `tardis-setup-XXXX` —
   `XXXX` is a short id unique to that board. Join it from your phone's Wi-Fi
   settings; a **"Sign in to network" prompt should open WiFiManager's own
   page by itself** within a few seconds. Pick your network, enter its
   password, and save — this screen is WiFiManager's, not this device's, so it
   looks different from the rest of the portal.
3. **Wait for it to join, then open the portal.** Once connected, this
   device's own portal starts listening and is reachable at
   **`http://tardis.local/`** (see [Hostname](#hostname) below if another
   `tardis` is already on the network). No computer needed to find the IP.
4. **Claim the portal.** Nobody has set a password yet, so the first screen
   asks you to choose one. From then on that page always asks for it — the
   same shape as the console's "enrol once, then just the code".
5. **Configure it.** Fill in the console address (`http://<host>:8000`) and
   the **pairing key** copied from the console's dashboard, then save.
6. **Check the console.** The dashboard's *Hardware link* card turns to
   `Polling` within a few seconds, and the power button starts working.

Revisiting the portal later shows the current configuration and lets you change
it. The API key is never sent back to the browser — it shows as
`•••••••• (stored)`, and leaving the field blank keeps what is saved.

### Hostname

Every device just answers as `tardis.local` — there is nothing device-specific
to remember. If another `tardis` device is already on the network, this one
probes for the first free name and falls back to `tardis-2.local`,
`tardis-3.local`, and so on, so two boards never fight over the same name.
This is done in application code (`NetworkStation::startMdns`,
`services/network_station.cpp`) rather than relying on the mDNS stack's own
conflict resolution, since the ESP-IDF version this board's platform pins
predates that being built in.

### Starting over

Two destructive actions live at the bottom of the configured portal, each
behind a confirmation prompt:

- **Forget Wi-Fi** — erases the stored Wi-Fi credentials (via WiFiManager) and
  reboots, so the device opens its `tardis-setup-XXXX` network again to be
  pointed at a different one. Server settings and the portal password are
  untouched.
- **Reset password** — clears just the portal password; the device goes back
  to the "claim it" screen so a new one can be set. Wi-Fi and the server
  settings are untouched.

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
console**, check how `server-app` was started. `tardis run` now binds
`0.0.0.0` by default and prints the LAN address to type into this portal; if it
was started with `--host 127.0.0.1` it answers only that machine, and no
separate device can ever poll it.

## The portal's own API

Served by the device on port 80, and only starts once the station has actually
joined the real network (WiFiManager's own setup-AP webserver also uses port
80, so ours can't bind it until that one has torn itself down — see
`main.cpp`'s `loop()`). The claim-then-login rule mirrors the console.

| Method | Path | Auth |
| --- | --- | --- |
| `GET` | `/` | public — the UI itself |
| `GET` | `/api/status` | public: booleans only (claimed, configured, Wi-Fi), link detail requires the session |
| `POST` | `/api/claim` | only while unclaimed — sets the portal password |
| `POST` | `/api/login` | password → session cookie |
| `POST` | `/api/logout` | — |
| `GET`/`POST` | `/api/config` | session cookie — server address, API key, poll interval, switch/sense pins |
| `POST` | `/api/test` | session cookie — polls the console once and reports what came back |
| `POST` | `/api/reboot` | session cookie |
| `POST` | `/api/forget-wifi` | session cookie — erases the Wi-Fi credentials and reboots |
| `POST` | `/api/reset-password` | session cookie — clears the portal password (device goes back to unclaimed) |

Wi-Fi itself has no API here at all — WiFiManager's own captive portal is what
an operator drives to join or change the network; this device's portal never
sees the SSID or password.

Security notes:

- The password is stored as a random 16-byte salt plus 20 000 iterations of
  SHA-256 (`include/password.h`) — a stolen flash dump does not hand over the
  password.
- Sessions are a random 128-bit token in an `HttpOnly` cookie, 30 minutes of
  idle time, one operator at a time. Five wrong passwords lock the form for five
  minutes.
- The API key is write-only over the portal API: it goes in, and only a masked
  hint (`••••••••sROK`) comes back.
- WiFiManager's setup AP is open (no password) by design, so a phone can join
  it without being told a secret first — it only exists until the device has
  somewhere to join, or until **Forget Wi-Fi** reopens it.
- mDNS (`http://tardis.local/`) is the most reliable on iOS/macOS. Some Android
  browsers do not resolve `.local` names; the device's LAN IP (visible in
  `pio device monitor` or your router's client list) always works as a
  fallback.

## Development

```bash
make test        # everything below, in order
make unit        # protocol, pulse timing, config rules, SHA-256 (native g++)
make protocol    # the wire protocol against a real in-process server-app
make firmware    # the REAL firmware, run as a process against a real server-app
make portal      # …the same build, left running: http://127.0.0.1:8095/
```

`make host` compiles `src/**/*.cpp` for this machine against the Arduino shims
in [`test/host/shim`](test/host/shim) — a small fake for GPIO, Wi-Fi, NVS and
`WiFiManager` (the fake just joins the first network the fake radio can see;
the real library's own captive portal is only exercised on hardware), with a
real socket HTTP server and client behind `WebServer` and `HTTPClient`. So the
portal, the config store and the agent under test are the same translation units
that get flashed; only the hardware underneath is faked. `make portal` serves
that build so the UI can be opened in a browser, and `make firmware` drives the
whole thing — claim the portal, enter a console address and key, save, then
assert the switch really goes HIGH for 500 ms (or 5 s) when the console queues a
command.

There is deliberately no second, hand-written mock of the device: a
re-implementation drifts from the firmware, and a bug that lives in both is
invisible.

```
include/         portable core: protocol, pulse state machine, config rules, SHA-256
src/             the ESP32 layers: NVS, portal, agent
src/services/    network_station.{h,cpp} — wraps WiFiManager, starts mDNS once joined
web/             the portal UI (embedded into the firmware at build time)
tools/           the embed script
test/host/       native unit tests, the Arduino shims, and the firmware end-to-end test
test/integration/  the wire protocol against server-app
```

`include/web_assets.h` is generated from `web/portal.html` on every build and is
not committed.

## Verified builds

`pio run` is the supported way to build. For the record, the firmware in this
commit was also compiled and linked for `lolin32_lite` against Arduino-ESP32
2.0.17 (WiFiManager 2.0.17 pulled in as `lib_deps`) with xtensa-esp32-elf GCC
8.4.0 (the toolchain `pio` fetches), producing a flashable image:

| | bytes |
| --- | --- |
| `.flash.text` | 796 995 |
| `.flash.rodata` | 218 148 |
| `.iram0.text` | 83 311 |
| `.dram0.data` + `.dram0.bss` | 50 684 |

That is 1 125 213 of the 1 310 720-byte default app partition (85.8%) and
50 684 of the 327 680 bytes of RAM (15.5%) — noticeably more flash than before
WiFiManager was added, though still comfortably inside the partition. It has
not been run on a physical board — the pulse timing, portal flow and console
handshake were verified with the host build described above.
