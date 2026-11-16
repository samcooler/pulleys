#!/usr/bin/env bash
# Restart the Overlord's app over SSH, and say whether it came back.
#
#   tools/overlord_restart.sh            pull the latest code, restart the app
#   tools/overlord_restart.sh --update   also rerun the boot update (git pull + media sync)
#                                        the way a reboot does: slower, picks up new media
#   tools/overlord_restart.sh --status   change nothing, just report
#   tools/overlord_restart.sh --push     copy this working tree over (rsync, no git pull), restart:
#                                        for when the Overlord has no internet, e.g. on its own AP.
#                                        Its checkout is then dirty; when it is back online, run
#                                        "git checkout . && git clean -fd" there (this script's
#                                        restart says so) before pulling again.
#   tools/overlord_restart.sh --push-install  --push, then rerun deploy/install.sh: a full deploy
#                                        with no internet (the Overlord on its own AP)
#   tools/overlord_restart.sh --install  git pull, then rerun overlord/deploy/install.sh (apt, units,
#                                        fallback AP); restarts the app, or says a reboot is needed
#   tools/overlord_restart.sh --logs     change nothing, print the app's recent log
#                                        (OVERLORD_LOG_LINES, default 80)
#
# The app only reads its code and its serial port at startup, so "pull, then
# restart" is the normal way to deploy, and a bridge plugged in after the app
# started needs a restart to be seen.
#
# The login is the one in overlord/README.md. It is not a secret (a kiosk on a
# private network), so it lives here in the open; this script is the one place
# it is used, so use this script rather than typing ssh by hand.
set -euo pipefail

OVERLORD_USER="overlord"
OVERLORD_PASSWORD="strings"
HOST="${OVERLORD_HOST:-overlord.local}"
# mDNS drops out now and then; then try the phone hotspot's lease, then the
# Overlord's own fallback AP (overlord-ap, see overlord/README.md).
FALLBACK_IPS="${OVERLORD_IP:-172.20.10.2} 10.42.0.1"
PORT="${OVERLORD_PORT:-8080}"

mode="restart"
case "${1:-}" in
    "") ;;
    --update) mode="update" ;;
    --status) mode="status" ;;
    --push) mode="push" ;;
    --install) mode="install" ;;
    --push-install) mode="push-install" ;;
    --logs) mode="logs" ;;
    *) echo "usage: $0 [--update|--status|--push|--push-install|--install|--logs]" >&2; exit 2 ;;
esac

command -v sshpass >/dev/null || { echo "needs sshpass: brew install sshpass" >&2; exit 1; }

# Host keys are still checked: the IP is looked up under the same known_hosts
# entry as overlord.local, so a changed key is refused, not ignored.
ssh_overlord() {
    SSHPASS="$OVERLORD_PASSWORD" sshpass -e ssh -4 -o HostKeyAlias=overlord.local \
        -o ConnectTimeout=8 -o PreferredAuthentications=password -o PubkeyAuthentication=no \
        "$OVERLORD_USER@$TARGET" "$@"
}

# Why one try failed: sshpass says nothing for its own refusals, only an exit code.
why() {
    case "$1" in
        5) echo "wrong password (sshpass 5)" ;;
        6) echo "host key not known or changed (sshpass 6): if it was reinstalled, run: ssh-keygen -R overlord.local, then ssh overlord@<ip> once and accept the key" ;;
        *) echo "exit $1: $(echo "$2" | tail -1)" ;;
    esac
}
TARGET="$HOST"
errs=""
rc=0; err=$(ssh_overlord true 2>&1) || rc=$?
if [ "$rc" != 0 ]; then
    errs="  $HOST: $(why $rc "$err")"
    TARGET=""
    for ip in $FALLBACK_IPS; do
        if err=$(TARGET="$ip" ssh_overlord true 2>&1); then TARGET="$ip"; break; else rc=$?; fi
        errs="$errs
  $ip: $(why $rc "$err")"
    done
    [ -n "$TARGET" ] || { echo "cannot reach the Overlord over ssh at $HOST or $FALLBACK_IPS:" >&2; echo "$errs" >&2; exit 1; }
    echo "($HOST did not answer; using $TARGET)"
fi

if [ "$mode" = "logs" ]; then
    ssh_overlord "journalctl -u overlord -n ${OVERLORD_LOG_LINES:-80} --no-pager 2>&1 || sudo -S journalctl -u overlord -n ${OVERLORD_LOG_LINES:-80} --no-pager" <<<"$OVERLORD_PASSWORD" 2>&1 | grep -v '^\[sudo\]'
    exit 0
fi

if [ "$mode" != "status" ]; then
    # sudo reads the password from stdin; the line goes to the remote's sudo, not to a log.
    case "$mode" in
        restart) remote='cd ~/pulleys && { git pull --ff-only 2>&1 | tail -1; [ -z "$(git status --porcelain --untracked-files=no)" ] || echo "WARNING: checkout has local changes (from --push?); git checkout . && git clean -fd to pull again"; }; sudo -S systemctl restart overlord' ;;
        install) remote='cd ~/pulleys && git pull --ff-only 2>&1 | tail -1 && sudo -S overlord/deploy/install.sh 2>&1 | tail -15' ;;
        push)    remote='sudo -S systemctl restart overlord' ;;
        push-install) remote='cd ~/pulleys && sudo -S overlord/deploy/install.sh 2>&1 | tail -15; sudo -n systemctl restart overlord 2>/dev/null || true' ;;
        update)  remote='sudo -S systemctl restart overlord-update overlord' ;;
    esac
    if [ "$mode" = push ] || [ "$mode" = push-install ]; then
        # Tracked and new (not ignored) files, as they are on disk here; .git is left alone.
        repo="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
        echo "Copying $(git -C "$repo" log -1 --format=%h) + local changes to $TARGET..."
        (cd "$repo" && git ls-files -co --exclude-standard -z) |
            SSHPASS="$OVERLORD_PASSWORD" rsync -a --from0 --files-from=- \
                -e "sshpass -e ssh -4 -o HostKeyAlias=overlord.local -o PreferredAuthentications=password -o PubkeyAuthentication=no" \
                "$repo/" "$OVERLORD_USER@$TARGET:pulleys/"
    fi
    echo "Restarting the Overlord ($mode)..."
    ssh_overlord "$remote" <<<"$OVERLORD_PASSWORD" 2>&1 | grep -v '^\[sudo\]' || true
fi

# Wait for the app to answer (an --update restart can take a few minutes if media is syncing).
deadline=$((SECONDS + ${OVERLORD_WAIT:-300}))
until curl -s -m 3 -o /dev/null "http://$TARGET:$PORT/volume"; do
    [ "$SECONDS" -lt "$deadline" ] || { echo "the Overlord's app did not come back within ${OVERLORD_WAIT:-300}s" >&2; exit 1; }
    sleep 3
done

ssh_overlord 'cd ~/pulleys && echo "code:   $(git log -1 --format="%h %s")"; echo "app:    $(systemctl is-active overlord)"'
# The serial link comes up a few seconds after the app starts answering, so give
# the bridge a moment before calling it missing.
report() {
    curl -s -m 8 "http://$TARGET:$PORT/api/firmware" | python3 -c "
import json, sys
d = json.load(sys.stdin)
bridge = d['bridge']
if '$1' == 'wait':
    sys.exit(0 if bridge['connected'] else 1)
print('bridge:', ('connected, ' + str(bridge['id'])) if bridge['connected'] else 'NOT connected (replug it, then restart)')
print('job:   ', d['active_job'] or 'none')
"
}
if [ "$mode" != "status" ]; then
    for _ in $(seq 10); do report wait && break; sleep 3; done
fi
report show
