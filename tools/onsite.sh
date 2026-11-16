#!/usr/bin/env bash
# One trip to the Overlord's own Wi-Fi (overlord-ap): deploy, flash, bring the logs back.
#
#   tools/onsite.sh                     everything below
#   tools/onsite.sh --ids A9C8,C6D1     flash only these boards (default: ONSITE_IDS below)
#   tools/onsite.sh --no-flash          deploy and fetch logs only
#
# 1. fetch the logs first (field_logs/<time>/), so a failed deploy still brings them back
# 2. copy this working tree to the Overlord and rerun its installer (--push-install)
# 3. flash the boards through the Overlord with fw
# Each step goes on to the next even if one fails; it says which did.
set -uo pipefail
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
ONSITE_IDS="${ONSITE_IDS:-A9C8,1966,28A2,4466,460B,59A4,C6D1,D08E}"
flash=1
while [ $# -gt 0 ]; do
    case "$1" in
        --ids) ONSITE_IDS="$2"; shift 2 ;;
        --no-flash) flash=0; shift ;;
        *) echo "usage: $0 [--ids ID,ID] [--no-flash]" >&2; exit 2 ;;
    esac
done
failed=""
echo "== logs"; tools/overlord_restart.sh --fetch || failed="$failed logs"
echo "== deploy"; tools/overlord_restart.sh --push-install || failed="$failed deploy"
if [ "$flash" = 1 ]; then
    echo "== flash $ONSITE_IDS"
    OVERLORD_URL=http://10.42.0.1:8080 /usr/bin/python3 fw flash --id "$ONSITE_IDS" || failed="$failed flash"
fi
echo
if [ -n "$failed" ]; then echo "done, with failures:$failed"; exit 1; fi
[ "$flash" = 1 ] && echo "done: logs fetched, Overlord deployed, boards flashed" || echo "done: logs fetched, Overlord deployed"
