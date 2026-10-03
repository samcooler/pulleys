#!/bin/bash
# Boot-time update: pull the latest code, then mirror the media. Both are
# best-effort: with no network the panel still starts on what's already on disk.
# Whole body is in a function so a git pull that rewrites this file can't
# corrupt the run (bash reads scripts incrementally).
main() {
    repo="$(cd "$(dirname "$0")/.." && git rev-parse --show-toplevel)"
    media="${OVERLORD_MEDIA:-/var/lib/overlord/media}"

    # network-online.target can pass before DNS works; wait up to 30s for the git host.
    for _ in $(seq 15); do
        getent hosts github.com >/dev/null && break
        sleep 2
    done

    echo "update: git pull"
    timeout 60 git -C "$repo" pull --ff-only || echo "update: git pull failed, keeping current code"

    if [ -n "$OVERLORD_MEDIA_REMOTE" ]; then
        echo "update: syncing media from $OVERLORD_MEDIA_REMOTE"
        mkdir -p "$media"
        # sync mirrors the remote exactly (removed files are deleted locally).
        # Partial downloads use a .partial suffix and are renamed when complete.
        rclone sync "$OVERLORD_MEDIA_REMOTE" "$media" \
            --exclude '.DS_Store' --retries 3 --low-level-retries 5 \
            --contimeout 15s --timeout 60s --stats-one-line -v \
            || echo "update: media sync failed, keeping current media"
    else
        echo "update: OVERLORD_MEDIA_REMOTE not set, skipping media sync"
    fi
    exit 0
}
main "$@"
exit 0
