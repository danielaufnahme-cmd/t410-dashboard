#!/bin/sh
# setup_alpine.sh - turn a fresh Alpine Linux install into a t410dash kiosk.
#
# Run as root from the repository root:   sh setup_alpine.sh
#
# Non-interactive and idempotent: running it again only changes what is
# missing or different. Every file it overwrites is backed up to *.bak
# first. An existing kiosk config.json is never touched.
#
# Copyright (c) 2026 Daniel. MIT License, see LICENSE.

set -eu

KIOSK_USER=kiosk
KIOSK_HOME=/home/$KIOSK_USER
PREFIX=/usr/local
HERE=$(cd "$(dirname "$0")" && pwd)
DEPLOY=$HERE/deploy

log()  { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "run as root"
[ -f /etc/alpine-release ] || die "this script is for Alpine Linux"
[ -f "$HERE/Makefile" ] && [ -d "$DEPLOY" ] || die "run from a full checkout of the repository"

# Back up $1 to $1.bak (or $1.bak.<time> if a .bak already exists).
backup() {
	[ -e "$1" ] || return 0
	b=$1.bak
	[ -e "$b" ] && b=$1.bak.$(date +%Y%m%d%H%M%S)
	cp -p "$1" "$b"
	note "backup: $b"
}

# install_file SRC DEST MODE OWNER - copy only if missing or different.
install_file() {
	src=$1 dest=$2 mode=$3 owner=$4
	mkdir -p "$(dirname "$dest")"
	if [ -f "$dest" ] && cmp -s "$src" "$dest"; then
		note "unchanged: $dest"
	else
		backup "$dest"
		cp "$src" "$dest"
		note "installed: $dest"
	fi
	chmod "$mode" "$dest"
	chown "$owner" "$dest"
}

# ---------------------------------------------------------------------------
log "Enabling the community repository"
REPOS=/etc/apk/repositories
if grep -Eq '^[[:space:]]*[^#[:space:]].*/community[[:space:]]*$' "$REPOS"; then
	note "already enabled"
else
	backup "$REPOS"
	# uncomment the first community line that is not edge
	awk '!done && /^#[[:space:]]*[^[:space:]]+\/community[[:space:]]*$/ && !/edge/ {
		sub(/^#[[:space:]]*/, ""); done = 1
	} { print }' "$REPOS" >"$REPOS.new"
	cat "$REPOS.new" >"$REPOS"
	rm -f "$REPOS.new"
	grep -Eq '^[^#].*/community' "$REPOS" || die "no community repository line in $REPOS"
fi
apk update -q

# ---------------------------------------------------------------------------
log "Installing runtime packages"
# eudev: Xorg needs udev for keyboard/touchpad hotplug (mdev is not enough).
apk add -q \
	xorg-server xf86-video-intel xf86-input-libinput eudev \
	xinit xset openbox \
	libx11 libxft fontconfig \
	doas acpid ca-certificates
# The DejaVu font package was renamed in newer Alpine releases.
apk add -q font-dejavu 2>/dev/null || apk add -q ttf-dejavu

if ! rc-update show sysinit 2>/dev/null | grep -qw udev; then
	log "Switching device manager to udev"
	if command -v setup-devd >/dev/null 2>&1; then
		setup-devd udev
	else
		rc-update add udev sysinit
		rc-update add udev-trigger sysinit
		rc-update add udev-settle sysinit
		rc-update add udev-postmount default
	fi
fi

# ---------------------------------------------------------------------------
log "Building t410dash"
BUILD=$(mktemp -d)
trap 'rm -rf "$BUILD"' EXIT
trap 'exit 1' INT TERM
apk add -q --virtual .build-deps build-base pkgconf libx11-dev libxft-dev
cp -R "$HERE/Makefile" "$HERE/src" "$BUILD/"
make -C "$BUILD" -s
make -C "$BUILD" -s install PREFIX="$PREFIX"
apk del -q .build-deps
note "installed: $PREFIX/bin/t410dash"

# ---------------------------------------------------------------------------
log "Creating user $KIOSK_USER"
if id "$KIOSK_USER" >/dev/null 2>&1; then
	note "already exists"
else
	adduser -D -s /bin/sh "$KIOSK_USER"
fi
for g in video input; do
	id -nG "$KIOSK_USER" | tr ' ' '\n' | grep -qx "$g" || addgroup "$KIOSK_USER" "$g"
done

KOWN=$KIOSK_USER:$KIOSK_USER
install -d -o "$KIOSK_USER" -g "$KIOSK_USER" -m 755 \
	"$KIOSK_HOME/.config" "$KIOSK_HOME/.config/openbox"
install -d -o "$KIOSK_USER" -g "$KIOSK_USER" -m 700 "$KIOSK_HOME/.config/t410dash"

CFG=$KIOSK_HOME/.config/t410dash/config.json
if [ -e "$CFG" ]; then
	note "keeping existing $CFG"
else
	install -o "$KIOSK_USER" -g "$KIOSK_USER" -m 600 "$HERE/config.json" "$CFG"
	note "installed: $CFG (edit it, it stays mode 600)"
fi
chmod 600 "$CFG"

log "Installing Openbox and session files"
install_file "$DEPLOY/openbox/rc.xml"    "$KIOSK_HOME/.config/openbox/rc.xml"    644 "$KOWN"
install_file "$DEPLOY/openbox/autostart" "$KIOSK_HOME/.config/openbox/autostart" 755 "$KOWN"
install_file "$DEPLOY/xinitrc"           "$KIOSK_HOME/.xinitrc"                  755 "$KOWN"
install_file "$DEPLOY/profile"           "$KIOSK_HOME/.profile"                  644 "$KOWN"

# ---------------------------------------------------------------------------
log "Allowing passwordless poweroff for $KIOSK_USER (doas)"
install_file "$DEPLOY/doas-kiosk.conf" /etc/doas.d/kiosk.conf 600 root:root
if doas -C /etc/doas.d/kiosk.conf 2>/dev/null; then
	note "doas rule syntax ok"
fi

# ---------------------------------------------------------------------------
log "Autologin on tty1"
LINE="tty1::respawn:/bin/login -f $KIOSK_USER"
if grep -qxF "$LINE" /etc/inittab; then
	note "already configured"
else
	backup /etc/inittab
	if grep -q '^tty1::' /etc/inittab; then
		sed -i "s|^tty1::.*|$LINE|" /etc/inittab
	else
		printf '%s\n' "$LINE" >>/etc/inittab
	fi
	note "tty1 now logs in $KIOSK_USER (other ttys keep their getty)"
fi

# ---------------------------------------------------------------------------
log "ACPI: lid and AC events never suspend or blank"
install_file "$DEPLOY/acpi/handler.sh" /etc/acpi/handler.sh 755 root:root
if [ ! -e /etc/acpi/events/anything ]; then
	mkdir -p /etc/acpi/events
	printf 'event=.*\naction=/etc/acpi/handler.sh %%e\n' >/etc/acpi/events/anything
	note "installed: /etc/acpi/events/anything"
fi
rc-update show default | grep -qw acpid || rc-update add acpid default

log "Disabling console blanking"
install_file "$DEPLOY/consoleblank.start" /etc/local.d/consoleblank.start 755 root:root
rc-update show default | grep -qw local || rc-update add local default

# ---------------------------------------------------------------------------
log "Done"
note "Edit $CFG (bank, countdown, weather location)."
note "Reboot: tty1 logs in $KIOSK_USER, starts X, Openbox and the dashboard."
note "For a blank-free boot console also add consoleblank=0 to the kernel"
note "command line (default_kernel_opts in /etc/update-extlinux.conf, then"
note "run update-extlinux)."
