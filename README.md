# tardis-net

Home for the tools that keep `tardis` reachable.

- [`server-app/`](server-app/) — a TOTP-protected web console (FastAPI + Jinja +
  htmx + Alpine + Tailwind) with one button: power `tardis` on or off, and see
  its status live. Setup and API reference in
  [`server-app/README.md`](server-app/README.md).
- [`hardware-app/`](hardware-app/) — the ESP32 that actually presses the switch.
  It polls the console for work and holds `tardis`'s power button for 500 ms or
  5 s depending on what was asked. Wiring, first-run setup and its config portal
  in [`hardware-app/README.md`](hardware-app/README.md).

The two are paired by a key the console generates and the device stores: the
console's pages take the operator's TOTP session, the device's two endpoints take
that key, and neither credential opens the other's doors.
