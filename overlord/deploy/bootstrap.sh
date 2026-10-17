#!/bin/bash
# Fresh Pi -> running overlord, in one command. Safe to re-run: every run pulls
# the latest code and re-applies the deploy.
#
#   curl -fsSL <url>/bootstrap.sh | bash
#
# Run it as the normal login user (not root); it uses sudo where needed.
# Overrides (env vars): PULLEYS_REPO, PULLEYS_BRANCH, PULLEYS_DIR.
#
# Everything is inside main(), so a half-downloaded script never runs, and a
# git pull that rewrites this file can't corrupt a run in progress.
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
    sudo "$dir/overlord/deploy/install.sh"
}

main "$@"
