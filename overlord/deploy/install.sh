#!/bin/bash
# Kiosk setup: splash screen, kiosk service, boot-time git pull + media sync.
# Runs on Raspberry Pi OS Lite (Bookworm+) or a plain Debian 12/13 install
# with no desktop (e.g. a mini PC). Run from the repo checkout:
#   sudo overlord/deploy/install.sh
# (or let deploy/bootstrap.sh clone/pull and call it). Safe to re-run: steps
# that are already done are skipped, and the app is restarted on the new code.
set -euo pipefail

[ "$EUID" -eq 0 ] || { echo "run with sudo"; exit 1; }
USER_NAME="${SUDO_USER:?run via sudo from the user that owns the checkout}"
DIR="$(cd "$(dirname "$0")/.." && pwd)"
HERE="$DIR/deploy"
BOOT=/boot/firmware

# Raspberry Pi OS keeps the firmware boot config here; a PC boots through GRUB.
if [ -f "$BOOT/config.txt" ]; then PLATFORM=pi; else PLATFORM=pc; fi

PKGS="mpv python3-serial python3-yaml rclone git plymouth plymouth-themes network-manager"
# Hardware video decode on Intel/AMD (mpv --hwdec=auto-safe picks VAAPI up).
[ "$PLATFORM" = pc ] && PKGS="$PKGS va-driver-all"
apt-get update
apt-get install -y $PKGS

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

# Kernel options for a quiet boot on the display: splash instead of console
# messages, no console cursor, no screen blanking.
KOPTS="quiet splash plymouth.ignore-serial-consoles logo.nologo vt.global_cursor_default=0 consoleblank=0"

if [ "$PLATFORM" = pi ]; then
    # plymouth only shows if boot uses an initramfs, and the firmware rainbow must be off.
    # Bookworm+ ships auto_initramfs=1, which already picks the right initramfs for the
    # board; only add it on images without it. Don't add both initramfs_2712 and
    # initramfs8 lines: the last one wins, so a Pi 5 would load the wrong one.
    if ! grep -qE '^(auto_initramfs=1|initramfs )' $BOOT/config.txt; then
        echo 'auto_initramfs=1' >> $BOOT/config.txt
        REBOOT=1
    fi
    grep -q '^disable_splash=1' $BOOT/config.txt || { echo 'disable_splash=1' >> $BOOT/config.txt; REBOOT=1; }

    CMD=$(cat $BOOT/cmdline.txt)
    CMD=${CMD//console=tty1/console=tty3}
    for opt in $KOPTS; do
        [[ " $CMD " == *" $opt "* ]] || CMD="$CMD $opt"
    done
    if [ "$CMD" != "$(cat $BOOT/cmdline.txt)" ]; then
        echo "$CMD" > $BOOT/cmdline.txt
        REBOOT=1
    fi
else
    # GRUB: same options in GRUB_CMDLINE_LINUX_DEFAULT, and no menu wait.
    GRUB=/etc/default/grub
    [ -f "$GRUB" ] || { echo "no $BOOT/config.txt and no $GRUB: unsupported bootloader"; exit 1; }
    CUR=$(sed -n 's/^GRUB_CMDLINE_LINUX_DEFAULT="\(.*\)"$/\1/p' "$GRUB")
    CMD="$CUR"
    for opt in $KOPTS; do
        [[ " $CMD " == *" $opt "* ]] || CMD="${CMD:+$CMD }$opt"
    done
    GRUB_CHANGED=0
    if [ "$CMD" != "$CUR" ]; then
        if grep -q '^GRUB_CMDLINE_LINUX_DEFAULT=' "$GRUB"; then
            sed -i "s|^GRUB_CMDLINE_LINUX_DEFAULT=.*|GRUB_CMDLINE_LINUX_DEFAULT=\"$CMD\"|" "$GRUB"
        else
            echo "GRUB_CMDLINE_LINUX_DEFAULT=\"$CMD\"" >> "$GRUB"
        fi
        GRUB_CHANGED=1
    fi
    if ! grep -q '^GRUB_TIMEOUT=0$' "$GRUB"; then
        sed -i '/^GRUB_TIMEOUT=/d' "$GRUB"
        echo 'GRUB_TIMEOUT=0' >> "$GRUB"
        GRUB_CHANGED=1
    fi
    if [ "$GRUB_CHANGED" = 1 ]; then
        update-grub
        REBOOT=1
    fi
fi

# If a desktop got installed, keep it from starting: boot to the text target so
# no display manager (gdm etc.) takes the screen. (undo: systemctl set-default graphical.target)
if [ "$(systemctl get-default)" != multi-user.target ]; then
    systemctl set-default multi-user.target
    REBOOT=1
fi

# no login prompt under the video (ssh still works; undo: systemctl unmask getty@tty1)
if [ "$(systemctl is-enabled getty@tty1.service 2>/dev/null)" != masked ]; then
    systemctl mask getty@tty1.service
    REBOOT=1
fi

# --- app services ---
[ -f /etc/overlord.env ] || cp "$HERE/overlord.env.example" /etc/overlord.env
# Media sync: once the Drive service-account key is in place (bootstrap.sh
# walks through getting it), point an unset OVERLORD_MEDIA_REMOTE at it.
HOME_DIR="$(getent passwd "$USER_NAME" | cut -d: -f6)"
SA_KEY="$HOME_DIR/gdrive-sa.json"
if [ -f "$SA_KEY" ] && ! grep -q '^OVERLORD_MEDIA_REMOTE=.' /etc/overlord.env; then
    REMOTE=":drive,service_account_file=$SA_KEY,root_folder_id=1ztfY8nkVEQuVMculoi4RvDAwYBr-PmnH,scope=drive.readonly:"
    if grep -q '^OVERLORD_MEDIA_REMOTE=' /etc/overlord.env; then
        sed -i "s|^OVERLORD_MEDIA_REMOTE=.*|OVERLORD_MEDIA_REMOTE=\"$REMOTE\"|" /etc/overlord.env
    else
        echo "OVERLORD_MEDIA_REMOTE=\"$REMOTE\"" >> /etc/overlord.env
    fi
    echo "Media sync enabled from Google Drive using $SA_KEY."
fi
mkdir -p /var/lib/overlord/media
chown -R "$USER_NAME" /var/lib/overlord
for unit in overlord overlord-update; do
    sed -e "s|@USER@|$USER_NAME|g" -e "s|@DIR@|$DIR|g" "$HERE/$unit.service" > /etc/systemd/system/$unit.service
done
systemctl daemon-reload
systemctl enable overlord-update.service overlord.service

echo
if ! grep -q '^OVERLORD_MEDIA_REMOTE=.' /etc/overlord.env; then
    echo "Media sync is off. To turn it on, re-run deploy/bootstrap.sh (it walks you"
    echo "through the Google Drive service-account key), or put the key at"
    echo "$SA_KEY and re-run install.sh (see README, 'Media sync from Google Drive')."
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
