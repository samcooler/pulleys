#!/usr/bin/env zsh
# Flash all connected boards in parallel using esptool.py directly.
# Usage: ./flash_all.sh [env]
#   env: platformio environment (default: traveler)
#
# Build first with: pio run -e <env>
# Then flash all connected boards: ./flash_all.sh <env>

set -euo pipefail

ENV="${1:-traveler}"
BUILD=".pio/build/$ENV"
PYTHON="/Users/sam/.platformio/penv/bin/python"
ESPTOOL="/Users/sam/.platformio/packages/tool-esptoolpy/esptool.py"
BOOT_APP="/Users/sam/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

# Auto-detect chip from environment name
case "$ENV" in
  station*) CHIP="esp32c3" ;;
  *)        CHIP="esp32s3" ;;
esac

# Verify build exists
if [[ ! -f "$BUILD/firmware.bin" ]]; then
  echo "No firmware found at $BUILD/firmware.bin — run 'pio run -e $ENV' first."
  exit 1
fi

# Find all connected USB serial ports
ports=(/dev/cu.usbmodem*(N))
if [[ ${#ports[@]} -eq 0 ]]; then
  echo "No USB serial ports found."
  exit 1
fi

echo "Flashing $ENV ($CHIP) to ${#ports[@]} board(s): ${ports[*]}"

flash() {
  local port="$1"
  "$PYTHON" "$ESPTOOL" --chip "$CHIP" --port "$port" --baud 460800 \
    --before default_reset --after hard_reset \
    write_flash -z --flash_mode dio --flash_freq 80m --flash_size 4MB \
    0x0000  "$BUILD/bootloader.bin" \
    0x8000  "$BUILD/partitions.bin" \
    0xe000  "$BOOT_APP" \
    0x10000 "$BUILD/firmware.bin" 2>&1 | tail -2
  echo "  ✓ $port"
}

for port in "${ports[@]}"; do
  flash "$port" &
done
wait

echo "Done — ${#ports[@]} board(s) flashed."
