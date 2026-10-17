#!/bin/bash
# Fresh Pi -> running overlord, in one command. Safe to re-run: every run pulls
# the latest code and re-applies the deploy.
#
#   curl -fsSL <url>/bootstrap.sh | bash
#
# Run it as the normal login user (not root); it uses sudo where needed.
# On a fresh Pi it also walks you through pasting the Google Drive service
# account key that the boot-time media sync uses (skipped once the key exists).
# Overrides (env vars): PULLEYS_REPO, PULLEYS_BRANCH, PULLEYS_DIR,
# PULLEYS_SA_KEY (path to an already-downloaded key file to install).
#
# Everything is inside functions called from main(), so a half-downloaded
# script never runs, and a git pull that rewrites this file can't corrupt a run
# in progress.

SA_ACCOUNT="synclever-sync@synclever.iam.gserviceaccount.com"
SA_CONSOLE="https://console.cloud.google.com/iam-admin/serviceaccounts?project=synclever"

# Print the client_email of a service-account key file, or fail.
sa_key_email() {
    python3 - "$1" <<'PY'
import json, sys
try:
    k = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(1)
if k.get("type") != "service_account" or not k.get("private_key") or not k.get("client_email"):
    sys.exit(1)
print(k["client_email"])
PY
}

# Guided setup of ~/gdrive-sa.json. Talks to the terminal directly because the
# script itself usually arrives on stdin (curl | bash).
setup_drive_key() {
    local key="$HOME/gdrive-sa.json" email tmp line

    if email=$(sa_key_email "$key" 2>/dev/null); then
        echo "bootstrap: Drive key present at $key ($email)"
        return
    fi
    if [ -n "${PULLEYS_SA_KEY:-}" ]; then
        email=$(sa_key_email "$PULLEYS_SA_KEY") || { echo "bootstrap: $PULLEYS_SA_KEY is not a service-account key" >&2; exit 1; }
        install -m 600 "$PULLEYS_SA_KEY" "$key"
        echo "bootstrap: installed Drive key from $PULLEYS_SA_KEY ($email)"
        return
    fi
    if ! { exec 3<>/dev/tty; } 2>/dev/null; then
        echo "bootstrap: no terminal, skipping Drive key setup (see README, 'Media sync from Google Drive')"
        return
    fi

    cat >&3 <<BANNER

=== Google Drive media sync ===
On every boot the panel mirrors the shared overlord_media Drive folder into
/var/lib/overlord/media. That needs a key for the service account
  $SA_ACCOUNT
To get one:
  1. Open  $SA_CONSOLE
  2. Click $SA_ACCOUNT
  3. Keys > Add key > Create new key > JSON. A .json file downloads.
  4. Open that file in a text editor, select all, copy.

Paste the whole key below. It ends with a line containing only "}".
(Press Enter on an empty line to skip; media sync stays off until you re-run
bootstrap or copy the key to $key yourself.)
BANNER
    tmp=$(mktemp) && chmod 600 "$tmp"
    while :; do
        : > "$tmp"
        printf 'key> ' >&3
        while IFS= read -r line <&3; do
            line=${line%$'\r'}
            if ! [ -s "$tmp" ] && [ -z "$line" ]; then
                rm -f "$tmp"; exec 3>&-
                echo "bootstrap: skipped Drive key setup"
                return
            fi
            printf '%s\n' "$line" >> "$tmp"
            # Stop at the closing brace of a pretty-printed key, or as soon as
            # what was pasted (e.g. a single-line key) is already valid.
            case "${line//[[:space:]]/}" in
                '}') break ;;
                *'}') sa_key_email "$tmp" >/dev/null 2>&1 && break ;;
            esac
        done
        if email=$(sa_key_email "$tmp" 2>/dev/null); then
            break
        fi
        echo "bootstrap: that is not a valid service-account key JSON; try again (or Enter to skip)" >&3
    done
    exec 3>&-
    install -m 600 "$tmp" "$key" && rm -f "$tmp"
    echo "bootstrap: saved Drive key to $key ($email)"
    if [ "$email" != "$SA_ACCOUNT" ]; then
        echo "bootstrap: note: key is for $email, not $SA_ACCOUNT; it must have access to the media folder"
    fi
}

main() {
    set -euo pipefail

    local repo="${PULLEYS_REPO:-https://github.com/samcooler/pulleys.git}"
    local branch="${PULLEYS_BRANCH:-main}"
    local dir="${PULLEYS_DIR:-$HOME/pulleys}"

    if [ "$EUID" -eq 0 ]; then
        echo "bootstrap: run this as your normal user, not root (it calls sudo itself)" >&2
        exit 1
    fi

    if ! command -v git >/dev/null; then
        echo "bootstrap: installing git"
        sudo apt-get update
        sudo apt-get install -y git
    fi

    if [ -d "$dir/.git" ]; then
        echo "bootstrap: updating $dir ($branch)"
        if [ -n "$(git -C "$dir" status --porcelain --untracked-files=no)" ]; then
            echo "bootstrap: $dir has local edits; commit, stash or discard them first:" >&2
            git -C "$dir" status --short --untracked-files=no >&2
            exit 1
        fi
        git -C "$dir" fetch --prune origin
        git -C "$dir" checkout -q "$branch"
        git -C "$dir" merge --ff-only "origin/$branch"
    elif [ -e "$dir" ]; then
        echo "bootstrap: $dir exists but isn't a git checkout; move it aside first" >&2
        exit 1
    else
        echo "bootstrap: cloning $repo ($branch) into $dir"
        git clone --branch "$branch" "$repo" "$dir"
    fi

    echo "bootstrap: at $(git -C "$dir" log -1 --format='%h %s')"
    setup_drive_key
    sudo "$dir/overlord/deploy/install.sh"
}

main "$@"
