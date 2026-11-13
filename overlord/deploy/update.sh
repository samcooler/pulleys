#!/bin/bash
# Boot-time update: pull the latest code, then mirror the media. Both are
# best-effort: with no network the panel still starts on what's already on disk.
# Whole body is in a function so a git pull that rewrites this file can't
# corrupt the run (bash reads scripts incrementally).
# "update: copying 12/44" -- to the journal, and onto the boot splash if there is one.
progress() {
    echo "update: copying $1/$2"
    plymouth --ping 2>/dev/null && plymouth display-message --text="Copying media $1/$2" 2>/dev/null
    return 0
}

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
        # Only files whose size/modtime changed are downloaded. Partial
        # downloads use a .partial suffix and are renamed when complete.
        # Google Docs (e.g. the Read Me) are skipped rather than exported;
        # --fast-list cuts Drive API calls. Both are ignored by non-Drive remotes.
        sync_args=(--exclude '.DS_Store' --drive-skip-gdocs --fast-list)
        # A dry run first, to know how many files are coming ("copying 12/44").
        total=$(rclone sync "$OVERLORD_MEDIA_REMOTE" "$media" "${sync_args[@]}" \
            --dry-run -v --contimeout 15s --timeout 60s 2>&1 | grep -c 'Skipped copy')
        echo "update: $total file(s) to download"
        n=0
        rclone sync "$OVERLORD_MEDIA_REMOTE" "$media" "${sync_args[@]}" \
            --retries 3 --low-level-retries 5 \
            --contimeout 15s --timeout 60s --stats-one-line -v 2>&1 \
            | while IFS= read -r line; do
                echo "$line"
                case "$line" in
                    *": Copied ("*)
                        n=$((n + 1))
                        progress "$n" "$total" ;;
                esac
            done
        # The pipe's own status is the loop's; rclone's is what says it worked.
        [ "${PIPESTATUS[0]}" -eq 0 ] || echo "update: media sync failed, keeping current media"
    else
        echo "update: OVERLORD_MEDIA_REMOTE not set, skipping media sync"
    fi
    exit 0
}
main "$@"
exit 0
