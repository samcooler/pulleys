#!/usr/bin/env zsh
# Flash connected boards in parallel using esptool.py directly.
#
# Usage: ./flash_all.sh [env] [options]
#   env            platformio environment (default: sensor)
#   -l, --list     list what is connected and exit — no flash, no reset
#   -r, --reset    reset only, no flash write
#   -p <port>      flash only this port (repeatable); skips detection
#   -i <id>        flash only the board with this device ID / name / MAC
#                  (e.g. -i A855, -i N-A855, -i 3C:0F:02:E4:52:2D)
#   --auto         reflash every identified board with the env it already runs
#   --any          skip the hardware-class check
#
# How a board is identified
# -------------------------
# Port names say nothing about what is on the other end, so the board is asked.
# Every role answers "?" on serial with one PULLEYS-ID line (lib/pulleys_whoami,
# probed by tools/identify.py) carrying:
#
#   class  hardware class — s3_16mb | s3_4mb | c3 | esp32. Decides whether an
#          image can fit, and is read from the live chip, so it stays true even
#          when the firmware on it is the wrong role.
#   role   PULLEYS_TYPE_* enum name, plus `env`: the firmware it is RUNNING.
#          Together these separate what the enum alone cannot (arbiter vs
#          arbiter_mesh, station vs station_wroom).
#   id     stable MAC-derived device ID, for addressing one specific board.
#
# A board that does not answer — blank, crashed, or non-Pulleys firmware — falls
# back to an esptool hardware probe, which still yields `class`. That is enough
# to flash safely: class is the safety net, and the env argument names the role.

set -euo pipefail

RESET_ONLY=false
LIST_ONLY=false
SKIP_CLASS_CHECK=false
AUTO_ENV=false
ENV="sensor"
ENV_GIVEN=false
ONLY_PORTS=()
ONLY_IDS=()
expect=""
for arg in "$@"; do
  if [[ -n "$expect" ]]; then
    case "$expect" in
      port) ONLY_PORTS+=("$arg") ;;
      id)   ONLY_IDS+=("${arg:u}") ;;
    esac
    expect=""
  else
    case "$arg" in
      -r|--reset) RESET_ONLY=true ;;
      -l|--list)  LIST_ONLY=true ;;
      --any)      SKIP_CLASS_CHECK=true ;;
      --auto)     AUTO_ENV=true ;;
      -p|--port)  expect="port" ;;
      -i|--id)    expect="id" ;;
      *)          ENV="$arg"; ENV_GIVEN=true ;;
    esac
  fi
done

PYTHON="/Users/sam/.platformio/penv/bin/python"
ESPTOOL="/Users/sam/.platformio/packages/tool-esptoolpy/esptool.py"
BOOT_APP="/Users/sam/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
IDENTIFY="${0:A:h}/tools/identify.py"

# Per-env flash geometry, and the hardware class the env may be written to.
# Sets: CHIP BOOT_ADDR FLASH_MODE FLASH_SIZE WANT_CLASS
env_params() {
  case "$1" in
    station_wroom|screen) CHIP="esp32";   BOOT_ADDR="0x1000"; FLASH_MODE="dio"; FLASH_SIZE="4MB";  WANT_CLASS="esp32"   ;;
    station)              CHIP="esp32c3"; BOOT_ADDR="0x0000"; FLASH_MODE="dio"; FLASH_SIZE="4MB";  WANT_CLASS="c3"      ;;
    arbiter*)             CHIP="esp32s3"; BOOT_ADDR="0x0000"; FLASH_MODE="dio"; FLASH_SIZE="16MB"; WANT_CLASS="s3_16mb" ;;
    *)                    CHIP="esp32s3"; BOOT_ADDR="0x0000"; FLASH_MODE="dio"; FLASH_SIZE="4MB";  WANT_CLASS="s3_4mb"  ;;
  esac
}
env_params "$ENV"

class_roles() {
  case "$1" in
    s3_16mb) echo "arbiter_mesh"           ;;
    s3_4mb)  echo "sensor (traveler dep.)" ;;
    c3)      echo "station (deprecated)"   ;;
    esp32)   echo "screen (st_wroom dep.)" ;;
    *)       echo "unknown"                ;;
  esac
}

# ── esptool fallback: hardware class only, for boards that stay silent ────────
# Uses hard_reset so a probed board resumes running rather than parking in the
# bootloader.
detect_class_hw() {
  local port="$1" out chip flash psram
  set +e
  out=$("$PYTHON" "$ESPTOOL" --chip auto --port "$port" \
        --before default_reset --after hard_reset flash_id 2>&1)
  local rc=$?
  set -e
  [[ $rc -ne 0 ]] && { echo "?"; return 0; }
  chip=$(echo "$out"  | sed -n 's/^Chip is \(.*\)$/\1/p' | head -1)
  flash=$(echo "$out" | sed -n 's/^Detected flash size: \(.*\)$/\1/p' | head -1)
  psram=$(echo "$out" | sed -n 's/.*Embedded PSRAM \([0-9]*MB\).*/\1/p' | head -1)
  case "$chip" in
    ESP32-S3*) if [[ "$psram" == "8MB" || "$flash" == "16MB" ]]; then echo "s3_16mb"; else echo "s3_4mb"; fi ;;
    ESP32-C3*) echo "c3"    ;;
    ESP32-*)   echo "esp32" ;;
    *)         echo "?"     ;;
  esac
}

# ── Build the board table ────────────────────────────────────────────────────
# Rows are: port class role env id label name mac source
typeset -a ROWS
gather() {
  local line port cls role renv id label name mac source
  ROWS=()
  while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    IFS=$'\t' read -r port cls role renv id label name mac source <<< "$line"
    if [[ "$cls" == "?" ]]; then
      # Silent or unopenable: fall back to the hardware probe for the class.
      cls=$(detect_class_hw "$port")
    fi
    ROWS+=("$port"$'\t'"$cls"$'\t'"$role"$'\t'"$renv"$'\t'"$id"$'\t'"$label"$'\t'"$name"$'\t'"$mac"$'\t'"$source")
  done < <("$PYTHON" "$IDENTIFY" "$@" 2>/dev/null)
}

row_field() { echo "$1" | cut -f"$2"; }

print_row() {
  local marker="$1" row="$2"
  printf "  %s %-24s %-8s %-9s %-13s %-6s %-8s %-18s %s\n" \
    "$marker" "$(row_field "$row" 1)" "$(row_field "$row" 2)" "$(row_field "$row" 3)" \
    "$(row_field "$row" 4)" "$(row_field "$row" 5)" "$(row_field "$row" 7)" \
    "$(row_field "$row" 8)" "$(row_field "$row" 9)"
}

print_header() {
  printf "  %s %-24s %-8s %-9s %-13s %-6s %-8s %-18s %s\n" \
    " " "PORT" "CLASS" "ROLE" "RUNNING" "ID" "NAME" "MAC" "VIA"
}

# ── -p: explicit ports bypass detection entirely ─────────────────────────────
if [[ ${#ONLY_PORTS[@]} -gt 0 ]]; then
  ports=("${ONLY_PORTS[@]}")
  if [[ "$LIST_ONLY" == true ]]; then echo "Ports: ${ports[*]}"; exit 0; fi
else
  candidates=(/dev/cu.usbmodem*(N) /dev/cu.usbserial*(N))
  if [[ ${#candidates[@]} -eq 0 ]]; then echo "No USB serial ports found."; exit 1; fi
  echo "Identifying ${#candidates[@]} board(s) over serial…"
  gather "${candidates[@]}"
  print_header

  # ── --auto: each board gets the env it already reports running ─────────────
  if [[ "$AUTO_ENV" == true ]]; then
    typeset -a auto_ports auto_envs
    for row in "${ROWS[@]}"; do
      renv=$(row_field "$row" 4)
      if [[ "$renv" == "?" || "$renv" == "unknown" ]]; then
        print_row " " "$row"
      else
        print_row "→" "$row"
        auto_ports+=("$(row_field "$row" 1)"); auto_envs+=("$renv")
      fi
    done
    if [[ ${#auto_ports[@]} -eq 0 ]]; then
      echo "\nNo board reported an env to reflash. Name an env explicitly."
      exit 1
    fi
    if [[ "$LIST_ONLY" == true ]]; then
      echo "\n--auto would reflash ${#auto_ports[@]} board(s) with their current env."
      exit 0
    fi
    echo "\nReflashing ${#auto_ports[@]} board(s) with their reported env:"
    rc=0
    for i in {1..${#auto_ports[@]}}; do
      echo "  → ${auto_ports[$i]} ← ${auto_envs[$i]}"
      "$0" "${auto_envs[$i]}" -p "${auto_ports[$i]}" 2>&1 | tail -1 || rc=1
    done
    exit $rc
  fi

  # ── Select by class, narrowed by -i if given ───────────────────────────────
  ports=()
  for row in "${ROWS[@]}"; do
    port=$(row_field "$row" 1); cls=$(row_field "$row" 2)
    id=$(row_field "$row" 5);   name=$(row_field "$row" 7); mac=$(row_field "$row" 8)
    marker=" "
    if [[ "$cls" == "$WANT_CLASS" || "$SKIP_CLASS_CHECK" == true ]]; then
      wanted=true
      if [[ ${#ONLY_IDS[@]} -gt 0 ]]; then
        wanted=false
        for want in "${ONLY_IDS[@]}"; do
          [[ "${id:u}" == "$want" || "${name:u}" == "$want" || "${mac:u}" == "$want" ]] && wanted=true
        done
      fi
      if [[ "$wanted" == true ]]; then marker="→"; ports+=("$port"); fi
    fi
    print_row "$marker" "$row"
  done

  if [[ "$LIST_ONLY" == true ]]; then
    echo "\n${#ports[@]} board(s) match class '$WANT_CLASS' (env $ENV)."
    [[ "$ENV_GIVEN" == false ]] && echo "(no env given — showed matches for the default '$ENV')"
    exit 0
  fi
  if [[ ${#ports[@]} -eq 0 ]]; then
    echo "\nNothing to flash for env '$ENV' (class '$WANT_CLASS')."
    echo "Try './flash_all.sh $ENV -l' to see what is connected, or --any to override."
    exit 1
  fi
fi

BUILD=".pio/build/$ENV"
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
