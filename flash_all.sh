#!/usr/bin/env zsh
# Flash all connected boards in parallel using esptool.py directly.
# Usage: ./flash_all.sh [env] [-r]
#   env: platformio environment (default: traveler)
#   -r:  reset only — no flash write
#   -p <port>: flash only this port (repeatable)
#
# Several roles now share the ESP32-S3 on /dev/cu.usbmodem*, so flashing every
# matching port would push sensor firmware onto an arbiter. Use -p when more
# than one S3 role is connected.
#
# Build first with: pio run -e <env>
# Then flash all connected boards: ./flash_all.sh <env>

set -euo pipefail

RESET_ONLY=false
ENV="traveler"
ONLY_PORTS=()
expect_port=false
for arg in "$@"; do
  if [[ "$expect_port" == true ]]; then
    ONLY_PORTS+=("$arg"); expect_port=false
  elif [[ "$arg" == "-r" || "$arg" == "--reset" ]]; then
    RESET_ONLY=true
  elif [[ "$arg" == "-p" || "$arg" == "--port" ]]; then
    expect_port=true
  else
    ENV="$arg"
  fi
done
BUILD=".pio/build/$ENV"
PYTHON="/Users/sam/.platformio/penv/bin/python"
ESPTOOL="/Users/sam/.platformio/packages/tool-esptoolpy/esptool.py"
BOOT_APP="/Users/sam/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

# Auto-detect chip, bootloader address, flash mode/size from environment name
case "$ENV" in
  station_wroom) CHIP="esp32";   BOOT_ADDR="0x1000"; FLASH_MODE="dio"; FLASH_SIZE="4MB"  ;;
  screen)        CHIP="esp32";   BOOT_ADDR="0x1000"; FLASH_MODE="dio"; FLASH_SIZE="4MB"  ;;
  station*)      CHIP="esp32c3"; BOOT_ADDR="0x0000"; FLASH_MODE="dio"; FLASH_SIZE="4MB"  ;;
  arbiter*)      CHIP="esp32s3"; BOOT_ADDR="0x0000"; FLASH_MODE="dio"; FLASH_SIZE="16MB" ;;
  *)             CHIP="esp32s3"; BOOT_ADDR="0x0000"; FLASH_MODE="dio"; FLASH_SIZE="4MB"  ;;
esac

# Verify build exists (not needed for reset-only)
if [[ "$RESET_ONLY" == false && ! -f "$BUILD/firmware.bin" ]]; then
  echo "No firmware found at $BUILD/firmware.bin — run 'pio run -e $ENV' first."
  exit 1
fi

# Find connected USB serial ports, restricted to the ones this chip uses:
# native-USB parts (C3/S3) enumerate as usbmodem, the WROOM's CH340 as usbserial.
# Without this, flashing with a sensor and a screen both plugged in would push
# the wrong image to one of them.
if [[ ${#ONLY_PORTS[@]} -gt 0 ]]; then
  ports=("${ONLY_PORTS[@]}")
elif [[ "$CHIP" == "esp32" ]]; then
  ports=(/dev/cu.usbserial*(N))
else
  ports=(/dev/cu.usbmodem*(N))
fi
if [[ ${#ports[@]} -eq 0 ]]; then
  echo "No USB serial ports found for $CHIP."
  exit 1
fi

if [[ "$RESET_ONLY" == true ]]; then
  echo "Resetting ($CHIP) ${#ports[@]} board(s): ${ports[*]}"
else
  echo "Flashing $ENV ($CHIP) to ${#ports[@]} board(s): ${ports[*]}"
fi

flash() {
  local port="$1"
  if [[ "$RESET_ONLY" == true ]]; then
    "$PYTHON" "$ESPTOOL" --chip "$CHIP" --port "$port" --before default_reset --after hard_reset run 2>&1 | tail -1
  else
    "$PYTHON" "$ESPTOOL" --chip "$CHIP" --port "$port" --baud 460800 \
      --before default_reset --after hard_reset \
      write_flash -z --flash_mode "$FLASH_MODE" --flash_freq 80m --flash_size "$FLASH_SIZE" \
      $BOOT_ADDR "$BUILD/bootloader.bin" \
      0x8000  "$BUILD/partitions.bin" \
      0xe000  "$BOOT_APP" \
      0x10000 "$BUILD/firmware.bin" 2>&1 | tail -2
  fi
  echo "  ✓ $port"
}

for port in "${ports[@]}"; do
  flash "$port" &
done
wait

echo "Done — ${#ports[@]} board(s) flashed."
