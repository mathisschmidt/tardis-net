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
- [`host/`](host/) — `tardis-net`, a small command for `tardis` itself: it picks
  which OS the next boot lands in. Setup below in
  [Setting up tardis](#setting-up-tardis-the-host).

The two are paired by a key the console generates and the device stores: the
console's pages take the operator's TOTP session, the device's two endpoints take
that key, and neither credential opens the other's doors.

## Setting up tardis (the host)

`tardis` dual-boots Fedora and Windows, and each OS has its own job:

- **Fedora** is what every power-on lands in. It runs headless as a plain server
  and is reached over SSH. There is no login screen to get past and nothing that
  waits for someone at the keyboard.
- **Windows** is for remote desktop with [Sunshine](https://github.com/LizardByte/Sunshine).
  On Windows Sunshine runs as a system service: it starts at boot and can stream
  the login screen. On Fedora it needs a logged-in Wayland session, auto-login,
  and an unlocked keyring, which is fragile when nobody is at the machine.

Once you are in Fedora you decide: stay, or run `tardis-net go-to-windows` so
the **next** boot is Windows. That choice holds for one boot only. Whatever
happens in Windows, the boot after it is Fedora again.

### Fedora: headless by default

```bash
sudo systemctl set-default multi-user.target   # boot to a text console, no SDDM/Plasma
sudo systemctl enable --now sshd
```

Use SSH keys only. Copy your key with `ssh-copy-id`, then create
`/etc/ssh/sshd_config.d/10-tardis.conf` containing `PasswordAuthentication no`,
and run `sudo systemctl reload sshd`.

When you are at the machine and want the desktop, log in on the text console and
run `sudo systemctl start sddm`. It lasts until the next reboot. To make the
desktop the default again: `sudo systemctl set-default graphical.target`.

### GRUB: Fedora as the default, Windows in the menu

In `/etc/default/grub`:

```bash
GRUB_DEFAULT=saved              # Fedora's default; one-shot choices rely on it
GRUB_DISABLE_OS_PROBER=false    # Fedora hides other OSes unless this is set
```

Then regenerate the menu, which adds the Windows entry, and make sure the saved
default is Fedora:

```bash
sudo grub2-mkconfig -o /boot/grub2/grub.cfg
sudo grubby --default-title                            # should name Fedora
sudo grubby --set-default "/boot/vmlinuz-$(uname -r)"  # if it does not
```

Kernel updates keep Fedora as the default on their own.

### Install `tardis-net`

From a clone of this repository on `tardis`:

```bash
sudo ./host/tardis-net.sh install    # → /usr/local/bin/tardis-net, then checks GRUB
```

The install reports anything left to fix: `GRUB_DEFAULT` not `saved`, or no
Windows entry found. It never edits GRUB's configuration itself.

| Command | What it does |
| --- | --- |
| `tardis-net go-to-windows` | next boot is Windows (`grub2-reboot`, one time only) |
| `tardis-net go-to-windows --reboot` | the same, and reboot into it right now |
| `tardis-net cancel` | forget that; next boot is Fedora |
| `tardis-net status` | default entry, Windows entry, and what boots next |

Every command needs root and re-runs itself with `sudo`. The Windows entry is the
first GRUB menu title containing "Windows". If that is the wrong one, set
`TARDIS_WINDOWS_ENTRY` to the exact title.

### Day to day

1. Power `tardis` on from the console. It boots Fedora.
2. `ssh tardis`.
3. Stay and work, or `tardis-net go-to-windows --reboot` and connect with
   Moonlight once Windows is up.
4. Shut Windows down from the console (or restart it). The next boot is Fedora.

### Windows side

- Install Sunshine with its service, the installer's default. It then starts at
  boot and can stream the login screen.
- **Turn off Fast Startup** (Control Panel → Power Options → *Choose what the
  power buttons do*). Otherwise "shut down", including the console's graceful
  shutdown pulse, is really a hibernate. The machine then never fully powers
  off, and Fedora may find the shared disks in a dirty state.
