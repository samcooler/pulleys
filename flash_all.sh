#!/usr/bin/env zsh
# Flash connected boards in parallel using esptool.py directly.
# Usage: ./flash_all.sh [env] [-r] [-l] [-p <port>] [--any]
#   env:       platformio environment (default: traveler)
#   -r:        reset only — no flash write
#   -l:        list detected boards and exit (no flash, no reset)
#   -p <port>: flash only this port (repeatable); skips detection
#   --any:     skip the board-class check and flash every port of the right chip
#
# Board detection
# ---------------
# Port names do not say what is on the other end, and several roles share a
# chip, so flashing "every usbmodem" once meant pushing sensor firmware onto an
# arbiter. This script now asks each board what it is (esptool --chip auto) and
# only writes to boards whose hardware class matches the target env.
#
# Detection reads chip type + flash size, giving these classes:
#   s3_16mb  ESP32-S3, 16MB flash, 8MB PSRAM   → arbiter, arbiter_mesh
#   s3_4mb   ESP32-S3, 4MB flash, 2MB PSRAM    → traveler, sensor
#   c3       ESP32-C3                          → station
#   esp32    ESP32-D0WD (WROOM / D1 Mini)      → station_wroom, screen
#
# Hardware cannot distinguish two roles on the same class (sensor vs traveler,
# screen vs station_wroom) — the env argument settles that. What detection does
# guarantee is that a 16MB arbiter never receives a 4MB sensor image.
#
# Build first with: pio run -e <env>
# Then flash all matching boards: ./flash_all.sh <env>

set -euo pipefail

RESET_ONLY=false
LIST_ONLY=false
SKIP_CLASS_CHECK=false
ENV="traveler"
ONLY_PORTS=()
expect_port=false
for arg in "$@"; do
  if [[ "$expect_port" == true ]]; then
    ONLY_PORTS+=("$arg"); expect_port=false
  elif [[ "$arg" == "-r" || "$arg" == "--reset" ]]; then
    RESET_ONLY=true
  elif [[ "$arg" == "-l" || "$arg" == "--list" ]]; then
    LIST_ONLY=true
  elif [[ "$arg" == "--any" ]]; then
    SKIP_CLASS_CHECK=true
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

# Auto-detect chip, bootloader address, flash mode/size from environment name,
# plus the board class that env is allowed to be written to.
case "$ENV" in
  station_wroom) CHIP="esp32";   BOOT_ADDR="0x1000"; FLASH_MODE="dio"; FLASH_SIZE="4MB";  WANT_CLASS="esp32"   ;;
  screen)        CHIP="esp32";   BOOT_ADDR="0x1000"; FLASH_MODE="dio"; FLASH_SIZE="4MB";  WANT_CLASS="esp32"   ;;
  station*)      CHIP="esp32c3"; BOOT_ADDR="0x0000"; FLASH_MODE="dio"; FLASH_SIZE="4MB";  WANT_CLASS="c3"      ;;
  arbiter*)      CHIP="esp32s3"; BOOT_ADDR="0x0000"; FLASH_MODE="dio"; FLASH_SIZE="16MB"; WANT_CLASS="s3_16mb" ;;
  *)             CHIP="esp32s3"; BOOT_ADDR="0x0000"; FLASH_MODE="dio"; FLASH_SIZE="4MB";  WANT_CLASS="s3_4mb"  ;;
esac

# Which envs each class can legitimately carry — used for the -l listing only.
class_roles() {
  case "$1" in
    s3_16mb) echo "arbiter | arbiter_mesh" ;;
    s3_4mb)  echo "traveler | sensor"      ;;
    c3)      echo "station"                ;;
    esp32)   echo "station_wroom | screen" ;;
    *)       echo "unknown"                ;;
  esac
}

# Ask one board what it is. Echoes "<class>\t<chip text>\t<flash>\t<mac>", or
# "unknown" when the port does not answer the ESP bootloader handshake (a
# Bluetooth tty, a board held in reset, an unplugged cable).
# Detection uses hard_reset so a probed board resumes running its firmware
# rather than being left parked in the bootloader.
detect_port() {
  local port="$1" out chip flash mac psram cls
  set +e
  out=$("$PYTHON" "$ESPTOOL" --chip auto --port "$port" \
        --before default_reset --after hard_reset flash_id 2>&1)
  local rc=$?
  set -e
  if [[ $rc -ne 0 ]]; then
    print -r -- $'unknown\t-\t-\t-'
    return 0
  fi
  chip=$(echo "$out"  | sed -n 's/^Chip is \(.*\)$/\1/p' | head -1)
  flash=$(echo "$out" | sed -n 's/^Detected flash size: \(.*\)$/\1/p' | head -1)
  mac=$(echo "$out"   | sed -n 's/^MAC: \(.*\)$/\1/p' | head -1)
  psram=$(echo "$out" | sed -n 's/.*Embedded PSRAM \([0-9]*MB\).*/\1/p' | head -1)

  case "$chip" in
    ESP32-S3*)
      # PSRAM size is the reliable discriminator: an S3 module's flash can read
      # back as 4MB on a 16MB part if the stub misdetects, but 8MB vs 2MB PSRAM
      # tracks the two board variants exactly. Fall back to flash size.
      if [[ "$psram" == "8MB" || "$flash" == "16MB" ]]; then cls="s3_16mb"
      else cls="s3_4mb"; fi ;;
    ESP32-C3*) cls="c3"    ;;
    ESP32-D0WD*|ESP32-*)  cls="esp32" ;;
    *)         cls="unknown" ;;
  esac
  print -r -- "$cls\t${chip:--}\t${flash:--}\t${mac:--}"
}

# Candidate ports: native-USB parts (C3/S3) enumerate as usbmodem, the WROOM's
# CH340 as usbserial. Detection then narrows further by actual board class.
all_candidates=(/dev/cu.usbmodem*(N) /dev/cu.usbserial*(N))

# Probe every candidate in parallel — each probe costs ~2s of serial handshake,
# and doing them serially is the slowest part of a multi-board flash.
probe_all() {
  local tmpdir port
  tmpdir=$(mktemp -d)
  for port in "${all_candidates[@]}"; do
    ( detect_port "$port" > "$tmpdir/${port:t}" ) &
  done
  wait
  for port in "${all_candidates[@]}"; do
    print -r -- "$port\t$(cat "$tmpdir/${port:t}")"
  done
  rm -rf "$tmpdir"
}

if [[ ${#all_candidates[@]} -eq 0 ]]; then
  echo "No USB serial ports found."
  exit 1
fi

# -p bypasses detection entirely: an explicit port is the user overriding us.
if [[ ${#ONLY_PORTS[@]} -gt 0 ]]; then
  ports=("${ONLY_PORTS[@]}")
else
  echo "Detecting ${#all_candidates[@]} candidate port(s)…"
  probe_lines=("${(@f)$(probe_all)}")

  ports=()
  for line in "${probe_lines[@]}"; do
    port=$(echo "$line" | cut -f1)
    cls=$(echo "$line"  | cut -f2)
    chip=$(echo "$line" | cut -f3)
    flash=$(echo "$line" | cut -f4)
    mac=$(echo "$line"  | cut -f5)
    if [[ "$cls" == "unknown" ]]; then
      printf "  %-24s  %s\n" "$port" "no ESP bootloader response — skipped"
      continue
    fi
    marker=" "
    if [[ "$cls" == "$WANT_CLASS" || "$SKIP_CLASS_CHECK" == true ]]; then
      marker="→"
      ports+=("$port")
    fi
    printf "  %s %-24s  %-8s %-26s %-6s %-17s  [%s]\n" \
      "$marker" "$port" "$cls" "$chip" "$flash" "$mac" "$(class_roles "$cls")"
  done

  if [[ "$LIST_ONLY" == true ]]; then
    echo "\nDetected ${#ports[@]} board(s) matching class '$WANT_CLASS' (env $ENV)."
    exit 0
  fi

  if [[ ${#ports[@]} -eq 0 ]]; then
    echo "\nNo boards of class '$WANT_CLASS' found for env '$ENV'."
    echo "Run './flash_all.sh $ENV -l' to see what is connected, or --any to override."
    exit 1
  fi
fi

if [[ "$LIST_ONLY" == true ]]; then
  echo "Ports: ${ports[*]}"
  exit 0
fi

# Verify build exists (not needed for reset-only)
if [[ "$RESET_ONLY" == false && ! -f "$BUILD/firmware.bin" ]]; then
  echo "No firmware found at $BUILD/firmware.bin — run 'pio run -e $ENV' first."
  exit 1
fi

if [[ "$RESET_ONLY" == true ]]; then
  echo "\nResetting ($CHIP) ${#ports[@]} board(s): ${ports[*]}"
else
  echo "\nFlashing $ENV ($CHIP) to ${#ports[@]} board(s): ${ports[*]}"
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
