#!/bin/bash
# Pi setup: splash screen, kiosk service, boot-time git pull + media sync.
# Run on Raspberry Pi OS Lite (Bookworm or later) from the repo checkout:
#   sudo overlord/deploy/install.sh
# (or let deploy/bootstrap.sh clone/pull and call it). Safe to re-run: steps
# that are already done are skipped, and the app is restarted on the new code.
set -euo pipefail

[ "$EUID" -eq 0 ] || { echo "run with sudo"; exit 1; }
USER_NAME="${SUDO_USER:?run via sudo from the user that owns the checkout}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
HERE="$DIR/deploy"
BOOT=/boot/firmware

apt-get update
apt-get install -y mpv python3-serial python3-yaml rclone git plymouth plymouth-themes

# --- splash: plymouth theme from deploy/splash.png ---
# Rebuilding the initramfs takes a while, so only do it when the theme changed.
REBOOT=0
THEME=/usr/share/plymouth/themes/overlord
mkdir -p "$THEME"
THEME_CHANGED=0
for f in "$HERE"/plymouth/overlord/* "$HERE/splash.png"; do
    cmp -s "$f" "$THEME/$(basename "$f")" || { cp "$f" "$THEME/"; THEME_CHANGED=1; }
done
if [ "$THEME_CHANGED" = 1 ] || [ "$(plymouth-set-default-theme)" != overlord ]; then
    plymouth-set-default-theme -R overlord   # also rebuilds the initramfs
    REBOOT=1
fi

# plymouth only shows if boot uses an initramfs, and the firmware rainbow must be off.
# Bookworm+ ships auto_initramfs=1, which already picks the right initramfs for the
# board; only add it on images without it. Don't add both initramfs_2712 and
# initramfs8 lines: the last one wins, so a Pi 5 would load the wrong one.
if ! grep -qE '^(auto_initramfs=1|initramfs )' $BOOT/config.txt; then
    echo 'auto_initramfs=1' >> $BOOT/config.txt
    REBOOT=1
fi
grep -q '^disable_splash=1' $BOOT/config.txt || { echo 'disable_splash=1' >> $BOOT/config.txt; REBOOT=1; }

# quiet boot on the display, no console cursor, no screen blanking.
CMD=$(cat $BOOT/cmdline.txt)
CMD=${CMD//console=tty1/console=tty3}
for opt in quiet splash plymouth.ignore-serial-consoles logo.nologo vt.global_cursor_default=0 consoleblank=0; do
    [[ " $CMD " == *" $opt "* ]] || CMD="$CMD $opt"
done
if [ "$CMD" != "$(cat $BOOT/cmdline.txt)" ]; then
    echo "$CMD" > $BOOT/cmdline.txt
    REBOOT=1
fi

# no login prompt under the video (ssh still works; undo: systemctl unmask getty@tty1)
if [ "$(systemctl is-enabled getty@tty1.service 2>/dev/null)" != masked ]; then
    systemctl mask getty@tty1.service
    REBOOT=1
fi

# --- app services ---
[ -f /etc/overlord.env ] || cp "$HERE/overlord.env.example" /etc/overlord.env
mkdir -p /var/lib/overlord/media
chown -R "$USER_NAME" /var/lib/overlord
for unit in overlord overlord-update; do
    sed -e "s|@USER@|$USER_NAME|g" -e "s|@DIR@|$DIR|g" "$HERE/$unit.service" > /etc/systemd/system/$unit.service
done
systemctl daemon-reload
systemctl enable overlord-update.service overlord.service

echo
if ! grep -q '^OVERLORD_MEDIA_REMOTE=.' /etc/overlord.env; then
    echo "Media sync is off. To turn it on: as $USER_NAME run 'rclone config' to"
    echo "create the remote, then set OVERLORD_MEDIA_REMOTE in /etc/overlord.env"
    echo "(e.g. gdrive:pulleys-media)."
fi
if [ "$REBOOT" = 1 ]; then
    echo "Boot config changed: sudo reboot to finish."
else
    # Code is already pulled (by bootstrap.sh or by hand), so skip the boot-time
    # update and just restart the app.
    systemctl restart overlord.service
    echo "Restarted overlord on $(git -C "$DIR" log -1 --format='%h %s')."
    echo "Logs: journalctl -u overlord -f"
fi
