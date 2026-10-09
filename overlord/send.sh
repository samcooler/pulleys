#!/usr/bin/env bash
# Send one or more trigger events to a running `overlord.py --test`, then exit.
# usage: ./send.sh video_once_test1 [video_once_test2 ...]
set -euo pipefail

SOCK="${OVERLORD_SOCK:-/tmp/overlord.sock}"

if [ $# -eq 0 ]; then
    echo "usage: $0 <event> [event ...]" >&2
    exit 1
fi

# Python closes the connection after sending; macOS nc keeps it open and hangs.
exec python3 - "$SOCK" "$@" <<'PY'
import socket, sys

path, events = sys.argv[1], sys.argv[2:]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(2.0)
try:
    s.connect(path)
except OSError as e:
    sys.exit(f"send.sh: can't connect to {path} ({e}); is `overlord.py --test` running?")
s.sendall("".join(f"{e}\n" for e in events).encode())
s.close()
PY
