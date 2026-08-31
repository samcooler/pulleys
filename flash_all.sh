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

# Piping esptool through tail loses its exit status, so capture the output and
# check the status explicitly — a flashing tool that reports success on a failed
# write is worse than one that says nothing.
flash() {
  local port="$1" out rc   # not `status`: that name is read-only in zsh
  # errexit would abort this function at the assignment below, before the exit
  # status could be inspected, losing the esptool diagnostics.
  set +e
  if [[ "$RESET_ONLY" == true ]]; then
    out=$("$PYTHON" "$ESPTOOL" --chip "$CHIP" --port "$port" --before default_reset --after hard_reset run 2>&1)
    rc=$?
  else
    out=$("$PYTHON" "$ESPTOOL" --chip "$CHIP" --port "$port" --baud 460800 \
      --before default_reset --after hard_reset \
      write_flash -z --flash_mode "$FLASH_MODE" --flash_freq 80m --flash_size "$FLASH_SIZE" \
      $BOOT_ADDR "$BUILD/bootloader.bin" \
      0x8000  "$BUILD/partitions.bin" \
      0xe000  "$BOOT_APP" \
      0x10000 "$BUILD/firmware.bin" 2>&1)
    rc=$?
  fi
  set -e
  if [[ $rc -eq 0 ]]; then
    echo "  ✓ $port"
  else
    echo "  ✗ $port — FAILED (exit $rc)"
    echo "$out" | tail -4 | sed 's/^/      /'
    return 1
  fi
}

# Flash in parallel, then collect each job's status. A background job runs in a
# subshell and cannot set a variable in the parent, so the result has to come
# back through `wait <pid>`.
pids=()
for port in "${ports[@]}"; do
  flash "$port" &
  pids+=($!)
done

failed=0
for pid in "${pids[@]}"; do
  wait "$pid" || failed=$((failed + 1))
done

if [[ $failed -gt 0 ]]; then
  echo "FAILED — $failed of ${#ports[@]} board(s) did not flash."
  exit 1
fi
echo "Done — ${#ports[@]} board(s) flashed."
