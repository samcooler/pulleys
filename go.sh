#!/usr/bin/env zsh
# Build, flash, and monitor in one shot.
# Usage: ./go.sh <env>
#   envs: station_wroom  station  traveler
set -euo pipefail

PIO="/Users/sam/.platformio/penv/bin/pio"

ENV="${1:-station_wroom}"
echo "▶ Building $ENV..."
"$PIO" run -e "$ENV"
echo "▶ Flashing..."
./flash_all.sh "$ENV"
echo "▶ Monitoring (Ctrl+C to stop)..."
"$PIO" device monitor -e "$ENV"
