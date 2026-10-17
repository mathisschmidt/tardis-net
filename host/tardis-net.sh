#!/bin/sh
# tardis-net — host-side helper for the dual-boot machine called `tardis`.
#
# Every power-on lands in Fedora (headless, reached over SSH). From there,
# `tardis-net go-to-windows` asks GRUB to boot Windows on the *next* boot only;
# the boot after that is Fedora again, because grub2-reboot sets a one-shot
# `next_entry` and leaves Fedora as the saved default.
#
#   sudo ./host/tardis-net.sh install     install as /usr/local/bin/tardis-net
#   tardis-net go-to-windows [--reboot]   next boot: Windows
#   tardis-net cancel                     next boot: Fedora (undo the above)
#   tardis-net status                     what GRUB will boot next
#
# Overrides (mostly for testing):
#   TARDIS_WINDOWS_ENTRY  exact GRUB menu title to boot instead of the detected one
#   TARDIS_GRUB_CFG       grub.cfg to read     (default /boot/grub2/grub.cfg)
#   TARDIS_GRUB_DEFAULTS  /etc/default/grub    (default /etc/default/grub)
#   TARDIS_BIN            install destination  (default /usr/local/bin/tardis-net)

set -eu

GRUB_CFG=${TARDIS_GRUB_CFG:-/boot/grub2/grub.cfg}
GRUB_DEFAULTS=${TARDIS_GRUB_DEFAULTS:-/etc/default/grub}
BIN=${TARDIS_BIN:-/usr/local/bin/tardis-net}

usage() {
	cat <<'EOF'
usage: tardis-net <command>

  go-to-windows [--reboot]  boot Windows on the next power-on only
                            (--reboot: reboot into it right now)
  cancel                    forget that request; next boot is Fedora
  status                    show what GRUB will boot next
  install                   install this script as /usr/local/bin/tardis-net
                            and check the GRUB setup
EOF
}

die() {
	echo "tardis-net: $*" >&2
	exit 1
}

# grub.cfg and grubenv are root-only on Fedora, so every command needs root.
as_root() {
	if [ "$(id -u)" -ne 0 ]; then
		command -v sudo >/dev/null 2>&1 || die "must be run as root"
		exec sudo -- "$0" "$@"
	fi
}

# The first top-level GRUB menu entry whose title mentions Windows, as written
# by os-prober: menuentry 'Windows Boot Manager (on /dev/nvme0n1p1)' --class ...
windows_entry() {
	if [ -n "${TARDIS_WINDOWS_ENTRY:-}" ]; then
		printf '%s\n' "$TARDIS_WINDOWS_ENTRY"
		return
	fi
	[ -r "$GRUB_CFG" ] || die "cannot read $GRUB_CFG"
	sed -n "s/^menuentry '\([^']*Windows[^']*\)'.*/\1/p" "$GRUB_CFG" | head -n 1
}

require_windows_entry() {
	entry=$(windows_entry)
	[ -n "$entry" ] || die "no Windows entry in $GRUB_CFG — enable os-prober:
  set GRUB_DISABLE_OS_PROBER=false in $GRUB_DEFAULTS, then run
  sudo grub2-mkconfig -o $GRUB_CFG"
	printf '%s\n' "$entry"
}

cmd_go_to_windows() {
	reboot=no
	for arg in "$@"; do
		case $arg in
			--reboot) reboot=yes ;;
			*) usage >&2; exit 2 ;;
		esac
	done
	entry=$(require_windows_entry)
	grub2-reboot "$entry"
	echo "Next boot: Windows ($entry). The boot after that is Fedora again."
	if [ "$reboot" = yes ]; then
		echo "Rebooting now."
		systemctl reboot
	else
		echo "Shut down, then power on from the console to land in Windows."
	fi
}

cmd_cancel() {
	[ $# -eq 0 ] || { usage >&2; exit 2; }
	grub2-editenv - unset next_entry
	echo "Next boot: Fedora."
}

cmd_status() {
	[ $# -eq 0 ] || { usage >&2; exit 2; }
	env=$(grub2-editenv list)
	saved=$(printf '%s\n' "$env" | sed -n 's/^saved_entry=//p')
	next=$(printf '%s\n' "$env" | sed -n 's/^next_entry=//p')
	echo "Default entry: ${saved:-(not set)}"
	echo "Windows entry: $(windows_entry || true)"
	if [ -n "$next" ]; then
		echo "Next boot:     $next (one time only)"
	else
		echo "Next boot:     default"
	fi
}

cmd_install() {
	[ $# -eq 0 ] || { usage >&2; exit 2; }
	install -D -m 0755 "$0" "$BIN"
	echo "Installed $BIN"

	ok=yes
	if ! grep -Eq '^GRUB_DEFAULT="?saved"?$' "$GRUB_DEFAULTS" 2>/dev/null; then
		ok=no
		echo "! $GRUB_DEFAULTS should contain GRUB_DEFAULT=saved, so Fedora stays the default."
	fi
	if [ -z "$(windows_entry 2>/dev/null || true)" ]; then
		ok=no
		echo "! No Windows entry in $GRUB_CFG. Set GRUB_DISABLE_OS_PROBER=false in"
		echo "  $GRUB_DEFAULTS, then: sudo grub2-mkconfig -o $GRUB_CFG"
	fi
	if [ "$ok" = yes ]; then
		echo "GRUB looks ready. Windows entry: $(windows_entry)"
	fi
}

[ $# -gt 0 ] || { usage >&2; exit 2; }
command=$1
shift
case $command in
	go-to-windows) as_root "$command" "$@"; cmd_go_to_windows "$@" ;;
	cancel) as_root "$command" "$@"; cmd_cancel "$@" ;;
	status) as_root "$command" "$@"; cmd_status "$@" ;;
	install) as_root "$command" "$@"; cmd_install "$@" ;;
	-h | --help | help) usage ;;
	*) usage >&2; exit 2 ;;
esac
