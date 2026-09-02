#!/usr/bin/env zsh
# Flash connected boards in parallel using esptool.py directly.
#
# Usage: ./flash_all.sh [env] [options]
#   env            platformio environment. With no env named, every attached
#                  board is reflashed with the env it already runs — the same
#                  thing --auto asks for, which is the default.
#   -l, --list     list what is connected and exit — no flash, no reset
#   -r, --reset    reset only, no flash write
#   -p <port>      flash only this port (repeatable); skips detection
#   -i <id>        flash only the board with this device ID / name / MAC
#                  (e.g. -i A855, -i N-A855, -i 3C:0F:02:E4:52:2D)
#   --auto         the default: reflash every board with the env it already
#                  runs, falling back to the role its class calls for when a
#                  board cannot say; honours -i
#   --install      the provisioning pass instead: register every board in the
#                  channel table and flash the role its class calls for,
#                  ignoring the (possibly retired) firmware it runs now
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
#
# Provisioning (--install)
# ------------------------
# The install pass, for a crate of boards rather than a bench of them. Every
# attached board ends up registered in CHANNEL_ASSIGNMENT and running the
# current firmware for its hardware:
#
#   1. identify every board, and pick its role from `class`, not from the
#      firmware it happens to be running — the crate is full of boards carrying
#      retired traveler/station images, and the default pass would faithfully
#      reflash them with those
#   2. hand every board to tools/install_map.py: sensors get the lowest free
#      rope channel, screens get a display spread across the ones available.
#      Boards already listed are left exactly as they are, so anything set by
#      hand survives
#   3. build the envs actually needed, so the table edit is in the image
#   4. flash each board with its own env
#
# A board that stayed silent has no device ID yet, so it cannot be registered
# before it is flashed. Those get a second pass automatically once the new
# firmware can answer for itself. The whole thing is idempotent: run it again
# after swapping a board and only the new board changes.

set -euo pipefail

RESET_ONLY=false
LIST_ONLY=false
SKIP_CLASS_CHECK=false
AUTO_ENV=false
PROVISION=false
PROVISION_ASKED=false
ENV=""
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
      --install|--provision) PROVISION_ASKED=true ;;
      -p|--port)  expect="port" ;;
      -i|--id)    expect="id" ;;
      *)          ENV="$arg"; ENV_GIVEN=true ;;
    esac
  fi
done

# No env named: reflash each board with whatever it reports running. Explicit
# ports skip identification entirely, so there is nothing to read an env from.
if [[ "$ENV_GIVEN" == false ]]; then
  if [[ ${#ONLY_PORTS[@]} -gt 0 ]]; then
    echo "-p needs an explicit env: detection is skipped, so there is nothing to"
    echo "read a running env from. e.g. ./flash_all.sh sensor -p ${ONLY_PORTS[1]}"
    exit 1
  fi
  # Bare invocation means "keep every board doing what it is doing", the same
  # thing --auto asks for. Reinstalling the crate is the rarer, more invasive
  # act, so it has to be asked for by name: --install.
  if [[ "$PROVISION_ASKED" == true ]]; then PROVISION=true; else AUTO_ENV=true; fi
  ENV="sensor"   # placeholder: both modes name a real env per board
fi

if [[ "$PROVISION_ASKED" == true && "$ENV_GIVEN" == true ]]; then
  echo "--install picks each board's env from its hardware class, so it cannot"
  echo "also be given one. Drop '$ENV', or drop --install."
  exit 1
fi

PYTHON="/Users/sam/.platformio/penv/bin/python"
PIO="/Users/sam/.platformio/penv/bin/pio"
ASSIGN="${0:A:h}/tools/install_map.py"
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

# The env a board of this class should be running today. Distinct from
# class_roles() below, which is descriptive text for the listing: this one is
# the decision, and it deliberately names no deprecated env — provisioning a
# crate must never reinstall a retired role.
class_env() {
  case "$1" in
    s3_4mb)  echo "sensor"       ;;
    esp32)   echo "screen"       ;;
    s3_16mb) echo "arbiter_mesh" ;;
    *)       echo ""             ;;   # c3/station is retired; unknown is unsafe
  esac
}

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

  # ── Provisioning: register, build, flash — the install pass ────────────────
  if [[ "$PROVISION" == true ]]; then
    typeset -a prov_ports prov_envs prov_specs prov_disp prov_unknown
    for row in "${ROWS[@]}"; do
      port=$(row_field "$row" 1); cls=$(row_field "$row" 2)
      id=$(row_field "$row" 5);   name=$(row_field "$row" 7); mac=$(row_field "$row" 8)
      tenv=$(class_env "$cls")
      wanted=true
      if [[ ${#ONLY_IDS[@]} -gt 0 ]]; then
        wanted=false
        for want in "${ONLY_IDS[@]}"; do
          [[ "${id:u}" == "$want" || "${name:u}" == "$want" || "${mac:u}" == "$want" ]] && wanted=true
        done
      fi
      if [[ "$wanted" == false || -z "$tenv" ]]; then
        print_row " " "$row"
        continue
      fi
      print_row "→" "$row"
      prov_ports+=("$port"); prov_envs+=("$tenv")
      # Only sensors carry a channel. A board that never answered has no ID to
      # register with; it is flashed anyway and picked up on the second pass.
      # Sensors carry a rope channel, screens carry a display; both are keyed
      # by device ID. A board that never answered has no ID to register with;
      # it is flashed anyway and picked up on the second pass.
      case "$tenv" in
        sensor|screen)
          if [[ "$id" == "?" || -z "$id" ]]; then
            prov_unknown+=("$port")
          elif [[ "$tenv" == "sensor" ]]; then
            prov_specs+=("${id}=${name}")
          else
            prov_disp+=("${id}=${name}")
          fi
          ;;
      esac
    done

    if [[ ${#prov_ports[@]} -eq 0 ]]; then
      echo "\nNo board to provision. Boards of a retired class (c3/station) and"
      echo "boards whose class could not be read are skipped — name an env"
      echo "explicitly to flash one of those anyway."
      exit 1
    fi

    # 1. register any board the install map does not know yet
    if [[ "$LIST_ONLY" == false ]]; then
      if [[ ${#prov_specs[@]} -gt 0 ]]; then
        echo "\nChannel table:"
        python3 "$ASSIGN" --add --kind channel "${prov_specs[@]}" | sed 's/^/  /'
      fi
      if [[ ${#prov_disp[@]} -gt 0 ]]; then
        echo "\nDisplay table:"
        python3 "$ASSIGN" --add --kind display "${prov_disp[@]}" | sed 's/^/  /'
      fi
    fi

    if [[ "$LIST_ONLY" == true ]]; then
      # The point of -l is deciding whether to edit the map before flashing, so
      # show each board beside the entry it has or would get — not just a count.
      echo "\nAttached boards and their install-map entries:"
      printf "  %-22s %-8s %-6s %-7s %-22s %s\n" \
             "PORT" "NAME" "ID" "ROLE" "ENTRY" "STATUS"
      # Resolve each table in one call so the preview allocates exactly as a
      # real run would; asking per board would show two new sensors the same
      # channel, since neither call would know about the other.
      typeset -A resolved
      for kind spec_list in channel "${prov_specs[*]}" display "${prov_disp[*]}"; do
        [[ -z "$spec_list" ]] && continue
        while IFS=$'\t' read -r rid rstate rvals; do
          [[ -z "$rid" ]] && continue
          resolved[$rid]="$rstate"$'\t'"$rvals"
        done < <(python3 "$ASSIGN" --resolve --kind "$kind" ${=spec_list} 2>/dev/null)
      done

      for i in {1..${#prov_ports[@]}}; do
        port="${prov_ports[$i]}"; tenv="${prov_envs[$i]}"
        pid="?"; pname="?"
        for row in "${ROWS[@]}"; do
          if [[ "$(row_field "$row" 1)" == "$port" ]]; then
            pid=$(row_field "$row" 5); pname=$(row_field "$row" 7)
          fi
        done
        entry="—"; stat="silent — registered after flashing"   # not `status`: read-only in zsh
        if [[ -n "${resolved[${pid:u}]:-}" ]]; then
          state="${${resolved[${pid:u}]}%%$'\t'*}"
          entry="${${resolved[${pid:u}]}#*$'\t'}"
          if [[ "$state" == "listed" ]]; then stat="listed"
          else stat="new — would be added"; fi
        elif [[ "$tenv" != "sensor" && "$tenv" != "screen" ]]; then
          entry="n/a"; stat="no entry needed"
        fi
        printf "  %-22s %-8s %-6s %-7s %-22s %s\n" \
               "$port" "$pname" "$pid" "$tenv" "$entry" "$stat"
      done
      echo "\nEdit lib/pulleys_install/pulleys_install.h to change any of these,"
      echo "then re-run. Nothing above has been written or flashed."
      exit 0
    fi

    # 2. build every env in play, so the table edit above is actually in the image
    typeset -A seen_env
    typeset -a build_args
    for e in "${prov_envs[@]}"; do
      if [[ -z "${seen_env[$e]:-}" ]]; then seen_env[$e]=1; build_args+=("-e" "$e"); fi
    done
    echo "\nBuilding: ${(k)seen_env}"
    if ! "$PIO" run "${build_args[@]}" >/dev/null 2>&1; then
      echo "Build FAILED — not flashing. Run 'pio run ${build_args[*]}' to see why."
      exit 1
    fi

    # 3. flash each board with its own env
    echo "\nFlashing ${#prov_ports[@]} board(s):"
    rc=0
    for i in {1..${#prov_ports[@]}}; do
      echo "  → ${prov_ports[$i]} ← ${prov_envs[$i]}"
      "$0" "${prov_envs[$i]}" -p "${prov_ports[$i]}" 2>&1 | tail -1 || rc=1
    done

    # 4. second pass for boards that could not identify themselves before. They
    #    can now, so register them and reflash just those with the real table.
    if [[ ${#prov_unknown[@]} -gt 0 && $rc -eq 0 ]]; then
      echo "\n${#prov_unknown[@]} board(s) were silent before flashing; asking again…"
      gather "${prov_unknown[@]}"
      typeset -a late_ports late_envs late_chan late_disp
      for row in "${ROWS[@]}"; do
        id=$(row_field "$row" 5); name=$(row_field "$row" 7); cls=$(row_field "$row" 2)
        [[ "$id" == "?" || -z "$id" ]] && continue
        tenv=$(class_env "$cls"); [[ -z "$tenv" ]] && continue
        late_ports+=("$(row_field "$row" 1)"); late_envs+=("$tenv")
        if [[ "$tenv" == "sensor" ]]; then late_chan+=("${id}=${name}")
        else                                late_disp+=("${id}=${name}"); fi
      done
      if [[ ${#late_ports[@]} -gt 0 ]]; then
        [[ ${#late_chan[@]} -gt 0 ]] && \
          python3 "$ASSIGN" --add --kind channel "${late_chan[@]}" | sed 's/^/  /'
        [[ ${#late_disp[@]} -gt 0 ]] && \
          python3 "$ASSIGN" --add --kind display "${late_disp[@]}" | sed 's/^/  /'
        typeset -A late_seen
        typeset -a late_build
        for e in "${late_envs[@]}"; do
          if [[ -z "${late_seen[$e]:-}" ]]; then late_seen[$e]=1; late_build+=("-e" "$e"); fi
        done
        if "$PIO" run "${late_build[@]}" >/dev/null 2>&1; then
          for i in {1..${#late_ports[@]}}; do
            echo "  → ${late_ports[$i]} ← ${late_envs[$i]} (re-flash with its assignment)"
            "$0" "${late_envs[$i]}" -p "${late_ports[$i]}" 2>&1 | tail -1 || rc=1
          done
        else
          echo "  Build FAILED on the second pass — those boards keep their fallback."
          rc=1
        fi
      else
        echo "  Still silent — they will run on their fallback until they answer."
      fi
    fi

    echo "\nInstall map now:"
    python3 "$ASSIGN" --kind channel --list | awk -F'\t' '{printf "  %s  ch%-8s %s\n", $1, $2, $3}'
    python3 "$ASSIGN" --kind display --list | awk -F'\t' '{printf "  %s  %-10s %s\n", $1, $2, $3}'
    exit $rc
  fi

  # ── --auto: each board gets the env it already reports running ─────────────
  if [[ "$AUTO_ENV" == true ]]; then
    typeset -a auto_ports auto_envs
    for row in "${ROWS[@]}"; do
      renv=$(row_field "$row" 4)
      id=$(row_field "$row" 5); name=$(row_field "$row" 7); mac=$(row_field "$row" 8)
      wanted=true
      if [[ ${#ONLY_IDS[@]} -gt 0 ]]; then
        wanted=false
        for want in "${ONLY_IDS[@]}"; do
          [[ "${id:u}" == "$want" || "${name:u}" == "$want" || "${mac:u}" == "$want" ]] && wanted=true
        done
      fi
      # A board that cannot say what it runs — blank, crashed, or carrying
      # non-Pulleys firmware — still reported a hardware class, and that is
      # enough to name an env. Falling back to it is the whole reason a silent
      # board is worth plugging in: refusing would leave it silent forever.
      if [[ "$renv" == "?" || "$renv" == "unknown" ]]; then
        renv=$(class_env "$(row_field "$row" 2)")
      fi
      if [[ "$wanted" == false || -z "$renv" ]]; then
        print_row " " "$row"
      else
        print_row "→" "$row"
        auto_ports+=("$(row_field "$row" 1)"); auto_envs+=("$renv")
      fi
    done
    if [[ ${#auto_ports[@]} -eq 0 ]]; then
      if [[ ${#ONLY_IDS[@]} -gt 0 ]]; then
        echo "\nNo board matched ${ONLY_IDS[*]} with an env to reflash."
      else
        echo "\nNo attached board has an env to flash: none reported one, and"
        echo "none is a hardware class with a current role. Name an env explicitly."
      fi
      exit 1
    fi
    if [[ "$LIST_ONLY" == true ]]; then
      if [[ "$RESET_ONLY" == true ]]; then
        echo "\nWould reset ${#auto_ports[@]} board(s)."
      else
        echo "\nWould reflash ${#auto_ports[@]} board(s) with their current env."
      fi
      [[ "$ENV_GIVEN" == false ]] && echo "(no env given — each board keeps the env it runs)"
      exit 0
    fi
    # -r must ride along, or a reset-only run would flash the boards instead.
    typeset -a pass_flags
    [[ "$RESET_ONLY" == true ]] && pass_flags+=("-r")
    [[ "$SKIP_CLASS_CHECK" == true ]] && pass_flags+=("--any")
    if [[ "$RESET_ONLY" == true ]]; then
      echo "\nResetting ${#auto_ports[@]} board(s):"
    else
      echo "\nReflashing ${#auto_ports[@]} board(s) with their own env:"
    fi
    rc=0
    for i in {1..${#auto_ports[@]}}; do
      echo "  → ${auto_ports[$i]} ← ${auto_envs[$i]}"
      "$0" "${auto_envs[$i]}" -p "${auto_ports[$i]}" "${pass_flags[@]}" 2>&1 | tail -1 || rc=1
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
if [[ "$RESET_ONLY" == true ]]; then
  echo "Done — ${#ports[@]} board(s) reset."
else
  echo "Done — ${#ports[@]} board(s) flashed."
fi
