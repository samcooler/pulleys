#!/bin/bash
# One-time Pi setup: splash screen, kiosk service, boot-time git pull + media sync.
# Run on Raspberry Pi OS Lite (Bookworm) from the repo checkout:  sudo overlord/deploy/install.sh
set -euo pipefail

[ "$EUID" -eq 0 ] || { echo "run with sudo"; exit 1; }
USER_NAME="${SUDO_USER:?run via sudo from the user that owns the checkout}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
HERE="$DIR/deploy"
BOOT=/boot/firmware

apt-get update
apt-get install -y mpv python3-serial python3-yaml rclone git plymouth plymouth-themes

# --- splash: plymouth theme from deploy/splash.png ---
THEME=/usr/share/plymouth/themes/overlord
mkdir -p "$THEME"
cp "$HERE"/plymouth/overlord/* "$THEME/"
cp "$HERE/splash.png" "$THEME/splash.png"
plymouth-set-default-theme -R overlord   # also rebuilds the initramfs

# plymouth only shows if boot uses an initramfs, and the firmware rainbow must be off.
grep -q '^initramfs ' $BOOT/config.txt || echo 'initramfs initramfs_2712 followkernel
initramfs initramfs8 followkernel' >> $BOOT/config.txt
grep -q '^disable_splash=1' $BOOT/config.txt || echo 'disable_splash=1' >> $BOOT/config.txt

# quiet boot on the display, no console cursor, no screen blanking.
CMD=$(cat $BOOT/cmdline.txt)
CMD=${CMD//console=tty1/console=tty3}
for opt in quiet splash plymouth.ignore-serial-consoles logo.nologo vt.global_cursor_default=0 consoleblank=0; do
    [[ " $CMD " == *" $opt "* ]] || CMD="$CMD $opt"
done
echo "$CMD" > $BOOT/cmdline.txt

# no login prompt under the video (ssh still works; undo: systemctl unmask getty@tty1)
systemctl mask getty@tty1.service

# --- app services ---
[ -f /etc/overlord.env ] || cp "$HERE/overlord.env.example" /etc/overlord.env
mkdir -p /var/lib/overlord/media
chown -R "$USER_NAME" /var/lib/overlord
for unit in overlord overlord-update; do
    sed -e "s|@USER@|$USER_NAME|g" -e "s|@DIR@|$DIR|g" "$HERE/$unit.service" > /etc/systemd/system/$unit.service
done
systemctl daemon-reload
systemctl enable overlord-update.service overlord.service

cat <<MSG

Done. Remaining by hand:
  1. As $USER_NAME run 'rclone config' to create the media remote, then set
     OVERLORD_MEDIA_REMOTE in /etc/overlord.env (e.g. gdrive:pulleys-media).
  2. The checkout must be able to 'git pull' without a password: use an HTTPS
     remote on a public repo, or add a read-only deploy key.
  3. sudo reboot
MSG
