#!/usr/bin/env bash
# Restart the Overlord's app over SSH, and say whether it came back.
#
#   tools/overlord_restart.sh            pull the latest code, restart the app
#   tools/overlord_restart.sh --update   also rerun the boot update (git pull + media sync)
#                                        the way a reboot does: slower, picks up new media
#   tools/overlord_restart.sh --status   change nothing, just report
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
FALLBACK_IP="${OVERLORD_IP:-172.20.10.2}"   # mDNS drops out now and then; this is the hotspot's lease
PORT="${OVERLORD_PORT:-8080}"

mode="restart"
case "${1:-}" in
    "") ;;
    --update) mode="update" ;;
    --status) mode="status" ;;
    *) echo "usage: $0 [--update|--status]" >&2; exit 2 ;;
esac

command -v sshpass >/dev/null || { echo "needs sshpass: brew install sshpass" >&2; exit 1; }

# Host keys are still checked: the IP is looked up under the same known_hosts
# entry as overlord.local, so a changed key is refused, not ignored.
ssh_overlord() {
    SSHPASS="$OVERLORD_PASSWORD" sshpass -e ssh -4 -o HostKeyAlias=overlord.local \
        -o ConnectTimeout=8 -o PreferredAuthentications=password -o PubkeyAuthentication=no \
        "$OVERLORD_USER@$TARGET" "$@"
}

TARGET="$HOST"
if ! ssh_overlord true 2>/dev/null; then
    TARGET="$FALLBACK_IP"
    ssh_overlord true >/dev/null || { echo "cannot reach the Overlord at $HOST or $FALLBACK_IP" >&2; exit 1; }
    echo "($HOST did not answer; using $FALLBACK_IP)"
fi

if [ "$mode" != "status" ]; then
    # sudo reads the password from stdin; the line goes to the remote's sudo, not to a log.
    case "$mode" in
        restart) remote='cd ~/pulleys && git pull --ff-only 2>&1 | tail -1; sudo -S systemctl restart overlord' ;;
        update)  remote='sudo -S systemctl restart overlord-update overlord' ;;
    esac
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
