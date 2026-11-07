#!/usr/bin/env python3
"""Convert a game state config from YAML to JSON, optionally POSTing it.

    game_to_json.py [config.yaml] [-o out.json] [--pretty] [--post HOST]

Reads game_state_machine/state_config.yaml by default and writes the JSON next
to it. The arbiter parses JSON (ArduinoJson) rather than YAML, so this is the
step between the file you edit and the file the board takes.

The config is checked first and nothing is written if it is wrong: `start` and
every `goto` must name a state, and every cue used must be listed under `cues`.

--post sends the JSON to the arbiter's POST /config endpoint (see
src/arbiter_mesh/gameconfig.cpp). HOST is the arbiter's address; join its
SoftAP first, where it is 192.168.4.1. Only the standard library is used for
the POST. Needs the python package `pyyaml`.
"""

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request

try:
    import yaml
except ImportError:
    sys.stderr.write("missing dep; run: pip3 install pyyaml\n")
    sys.exit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CONFIG = os.path.join(ROOT, "game_state_machine", "state_config.yaml")


def fix_keys(node):
    """YAML 1.1 reads a bare `on:` key as boolean True (and `off:` as False);
    spell them back as the words they were written as."""
    if isinstance(node, dict):
        return {{True: "on", False: "off"}.get(k, k) if isinstance(k, bool) else k:
                fix_keys(v) for k, v in node.items()}
    if isinstance(node, list):
        return [fix_keys(v) for v in node]
    return node


def load_config(path):
    with open(path) as f:
        return fix_keys(yaml.safe_load(f))


# What a cue name may be: it is sent over ESP-NOW as ASCII and the bridge writes
# it into a line on the Pi link, so only plain characters are allowed. A * is
# a wildcard the player resolves: the cue plays one of the files it matches.
# CUE_NAME_MAX matches MESH_CUE_NAME_MAX in lib/pulleys_mesh.
CUE_NAME_MAX = 63
CUE_NAME = re.compile(r"[a-z0-9_*-]{1,%d}" % CUE_NAME_MAX)

# Every event something emits. A state listing any other name would never fire.
# Keep in step with src/arbiter_mesh/gameevents.h and overlord/game.py.
KNOWN_EVENTS = {
    "first_goal_component_activated",
    "goal_component_activated",
    "nongoal_component_activated",
    "component_deactivated",
    "all_components_activated",
    "all_components_deactivated",
    "final_component_activated",
    "final_component_deactivated",
    "all_final_components_activated",
}

# How many components of each kind there are. The mesh packet and the component
# state display have a bar for each. Keep in step with gameevents.h.
COMPONENT_COUNTS = {"major": 5}

# The colours a major may be. Keep in step with COLOR_* / COLOR_GAME1_COUNT in
# lib/pulleys_channel.
GAME1_COLORS = ("red", "blue", "green", "yellow", "orange")

# The mesh packet carries the decay rate in thousandths of a percent per second
# in 16 bits. Keep in step with MeshComponents in lib/pulleys_mesh.
MAX_DECAY_RATE = 65.0

# Sections only the Pis read; the arbiter's JSON leaves them out.
OVERLORD_ONLY = ("ambient_sounds", "players")

# Addressing a cue to a player: "shrine__audio_once_bell" is for the Pi named
# shrine; no prefix is the Overlord's. Keep in step with overlord/players.py.
PRIMARY_PLAYER = "overlord"
TARGET_SEP = "__"
PLAYER_NAME = re.compile(r"[a-z0-9]+(?:[-_][a-z0-9]+)*")
PLAYER_KINDS = ("audio_once_", "audio_loop_", "video_once_", "video_loop_", "image_show_")
PLAYER_WORDS = ("audio_loop_off", "image_off")


# Device settings and inputs cross the air as 16-bit hashes of their names
# (lib/pulleys_settings): FNV-1a, folded to 16 bits, never 0. Keep in step with
# settings_hash() there.
def name_hash(text):
    h = 2166136261
    for b in text.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    folded = (h >> 16) ^ (h & 0xFFFF)
    return folded or 1


DEVICE_WORD = re.compile(r"[a-z0-9_]+")
SETTINGS_MAX = 48   # pulleys_settings SETTINGS_MAX: one frame's worth
INPUTS_MAX = 32     # devices.cpp MAX_INPUTS
AT_MAX = 16         # gamestate.cpp MAX_AT
YAML_BOOL_HINT = "quote on/off/yes/no: YAML reads them as true/false"


def check_devices(cfg, errors):
    """The devices section. Returns {"dev.setting": [values]} and {"dev.input"}."""
    settings, inputs = {}, set()
    devs = cfg.get("devices")
    if devs is None:
        return settings, inputs
    if not isinstance(devs, dict):
        errors.append("devices: must be a map of device names")
        return settings, inputs
    on_air = {}
    def unique(name):
        h = name_hash(name)
        if h in on_air:
            errors.append(f"devices: '{name}' and '{on_air[h]}' are the same on the air; rename one")
        on_air[h] = name
    for dev, body in devs.items():
        if not isinstance(dev, str) or not DEVICE_WORD.fullmatch(dev):
            errors.append(f"devices.{dev}: a device name is lowercase letters, digits and _")
            continue
        if not isinstance(body, dict):
            errors.append(f"devices.{dev}: must be a map with set and/or emits")
            continue
        for key in set(body) - {"set", "emits"}:
            errors.append(f"devices.{dev}: unknown key '{key}'; use set, emits")
        for prop, values in (body.get("set") or {}).items():
            key = f"{dev}.{prop}"
            if not isinstance(prop, str) or not DEVICE_WORD.fullmatch(prop):
                errors.append(f"devices.{key}: a setting name is lowercase letters, digits and _")
                continue
            if not isinstance(values, list) or not values:
                errors.append(f"devices.{key}: list the values it may take")
                continue
            if not all(isinstance(v, str) and DEVICE_WORD.fullmatch(v) for v in values):
                errors.append(f"devices.{key}: values are words ({YAML_BOOL_HINT})")
                continue
            if len({name_hash(v) for v in values}) != len(values):
                errors.append(f"devices.{key}: two values are the same on the air; rename one")
            unique(key)
            settings[key] = values
        emits = body.get("emits") or []
        if not isinstance(emits, list):
            errors.append(f"devices.{dev}.emits: must be a list")
            continue
        for e in emits:
            if not isinstance(e, str) or not DEVICE_WORD.fullmatch(e):
                errors.append(f"devices.{dev}.emits: '{e}' must be a lowercase word")
                continue
            unique(f"{dev}.{e}")
            inputs.add(f"{dev}.{e}")
    if len(settings) > SETTINGS_MAX:
        errors.append(f"devices: {len(settings)} settings, at most {SETTINGS_MAX}")
    if len(inputs) > INPUTS_MAX:
        errors.append(f"devices: {len(inputs)} inputs, at most {INPUTS_MAX}")
    return settings, inputs


def for_arbiter(cfg):
    return {k: v for k, v in cfg.items() if k not in OVERLORD_ONLY}


def validate(cfg):
    """Return a list of problems; empty means the config is sound."""
    errors = []
    if not isinstance(cfg, dict):
        return ["top level must be a map"]
    for key in ("game", "start", "cues", "states"):
        if key not in cfg:
            errors.append(f"missing top-level key '{key}'")
    if errors:
        return errors

    states = cfg["states"] or {}
    known_cues = {c for group in (cfg["cues"] or {}).values() for c in group}

    # A cue crosses the air as its name in ASCII, in one packet (GAME_FORMAT.md).
    for group, cues in (cfg["cues"] or {}).items():
        for cue in cues or []:
            if not isinstance(cue, str) or not CUE_NAME.fullmatch(cue):
                errors.append(f"cues.{group}: '{cue}' must be 1-{CUE_NAME_MAX} characters of "
                              "lowercase letters, digits, _ and -")

    # players: the satellite Pis a cue may be addressed to (GAME_FORMAT.md)
    players = cfg.get("players") or []
    if not isinstance(players, list):
        errors.append("players: must be a list of player names")
        players = []
    for i, name in enumerate(players):
        if (not isinstance(name, str) or not PLAYER_NAME.fullmatch(name)
                or TARGET_SEP in name or name == PRIMARY_PLAYER):
            errors.append(f"players[{i}]: '{name}' must be lowercase letters and digits "
                          f"(single - or _ between), not containing '{TARGET_SEP}' "
                          f"and not '{PRIMARY_PLAYER}'")
    targets = set(players) | {PRIMARY_PLAYER}
    for group, cues in (cfg["cues"] or {}).items():
        for cue in cues or []:
            if not isinstance(cue, str) or TARGET_SEP not in cue:
                continue
            player, bare = cue.split(TARGET_SEP, 1)
            if player not in targets:
                errors.append(f"cues.{group}: '{cue}' is addressed to '{player}', "
                              f"which is not listed under players")
            elif not (bare in PLAYER_WORDS or
                      any(bare.startswith(k) and len(bare) > len(k) for k in PLAYER_KINDS)):
                errors.append(f"cues.{group}: '{cue}': a player plays "
                              f"{', '.join(k + '<name>' for k in PLAYER_KINDS)} or "
                              f"{' / '.join(PLAYER_WORDS)}, not '{bare}'")

    def check_cue(where, cue):
        if cue not in known_cues:
            errors.append(f"{where}: cue '{cue}' is not listed under cues")

    # An entry in a cue list is a name, or {choose_one: [names]}: one of them,
    # picked at random each time the list fires.
    def check_cues(where, cues):
        for cue in cues or []:
            if isinstance(cue, dict):
                options = cue.get("choose_one")
                if set(cue) != {"choose_one"} or not isinstance(options, list) or not options:
                    errors.append(f"{where}: a map in a cue list must be "
                                  "choose_one: [cue, ...] with at least one cue")
                    continue
                for option in options:
                    check_cue(f"{where}.choose_one", option)
            else:
                check_cue(where, cue)

    def check_goto(where, target):
        if target not in states:
            errors.append(f"{where}: goto '{target}' is not a state")

    settings, inputs = check_devices(cfg, errors)

    # set: {device.setting: value}, persisting until something sets it again
    def check_set(where, block):
        if block is None:
            return
        if not isinstance(block, dict):
            errors.append(f"{where}.set: must be a map of device.setting: value")
            return
        for key, value in block.items():
            if key not in settings:
                errors.append(f"{where}.set: '{key}' is not a setting under devices")
            elif value not in settings[key]:
                errors.append(f"{where}.set: {key} cannot be {value!r} "
                              f"(one of {', '.join(settings[key])}; {YAML_BOOL_HINT})")

    # components: majors are game1 colours; finals are the purple channels and
    # are not configured (GAME_FORMAT.md)
    comps = cfg.get("components")
    if not isinstance(comps, dict):
        errors.append("components: missing, or not a map with a major list")
    else:
        count = COMPONENT_COUNTS["major"]
        colors = comps.get("major")
        if not isinstance(colors, list) or len(colors) != count:
            errors.append(f"components.major: must list exactly {count} colours")
        else:
            seen = {}
            for i, color in enumerate(colors):
                where = f"components.major[{i}]"
                if color not in GAME1_COLORS:
                    errors.append(f"{where}: '{color}' is not a game1 colour ({', '.join(GAME1_COLORS)})")
                elif color in seen:
                    errors.append(f"{where}: {color} already drives major component {seen[color] + 1}")
                else:
                    seen[color] = i
        if "final" in comps:
            errors.append("components.final: gone -- every purple channel is a final now "
                          "(set on the arbiter's COLOURS screen); remove the list")
        extra = set(comps) - set(COMPONENT_COUNTS) - {"final"}
        if extra:
            errors.append(f"components: unknown kind {sorted(map(str, extra))}; use major")

    rate = cfg.get("default_decay_rate")
    if rate is not None and (isinstance(rate, bool) or not isinstance(rate, (int, float))
                             or not 0 < rate <= MAX_DECAY_RATE):
        errors.append(f"default_decay_rate: must be a number above 0 and at most "
                      f"{MAX_DECAY_RATE} (percent of full charge per second), not {rate!r}")
    add = cfg.get("charge_per_pull")
    if add is not None and (isinstance(add, bool) or not isinstance(add, (int, float))
                            or not 0 < add <= 100):
        errors.append(f"charge_per_pull: must be a number above 0 and at most 100 "
                      f"(percent of full charge), not {add!r}")

    # ambient_sounds: overlord's own schedule (GAME_FORMAT.md)
    for i, entry in enumerate(cfg.get("ambient_sounds") or []):
        where = f"ambient_sounds[{i}]"
        if not isinstance(entry, dict):
            errors.append(f"{where}: must be a map with sound, interval and variance")
            continue
        sound = entry.get("sound")
        if sound not in (cfg["cues"] or {}).get("audio_once", []):
            errors.append(f"{where}: sound '{sound}' is not listed under cues.audio_once")
        interval, variance = entry.get("interval"), entry.get("variance", 0)
        numbers = all(isinstance(v, (int, float)) and not isinstance(v, bool)
                      for v in (interval, variance))
        if not numbers or interval <= 0 or variance < 0:
            errors.append(f"{where}: interval must be a positive number of seconds "
                          "and variance a number of seconds, 0 or more")
        elif variance >= interval:
            errors.append(f"{where}: variance must be less than interval, "
                          "or a play could be due before the last one")

    if cfg["start"] not in states:
        errors.append(f"start '{cfg['start']}' is not a state")

    for name, state in states.items():
        state = state or {}
        check_cues(f"{name}.on_enter_cue", state.get("on_enter_cue"))
        for event, action in (state.get("on") or {}).items():
            # `enter` is a lifecycle trigger handled by both the Overlord and
            # arbiter state machines; it is not emitted by a sensor event.
            # A dotted event is a device's input (devices.<device>.emits).
            if event != "enter" and event not in KNOWN_EVENTS and event not in inputs:
                errors.append(f"{name}.on.{event}: not an event anything sends")
            if action is not None and not isinstance(action, dict):
                errors.append(f"{name}.on.{event}: must be a map with cue, set and/or goto")
                continue
            action = action or {}
            check_cues(f"{name}.on.{event}", action.get("cue"))
            check_set(f"{name}.on.{event}", action.get("set"))
            if "goto" in action:
                check_goto(f"{name}.on.{event}", action["goto"])
        after = state.get("after")
        if after:
            if "goto" in after:
                check_goto(f"{name}.after", after["goto"])
            else:
                errors.append(f"{name}.after: missing goto")
        idle = state.get("idle")
        if idle is not None:
            if not isinstance(idle, dict) or not re.fullmatch(r"[1-9]\d*[sm]", str(idle.get("delay"))):
                errors.append(f"{name}.idle: needs delay like 20s or 1m, above 0, and cue")
            else:
                check_cues(f"{name}.idle", idle.get("cue"))
                check_set(f"{name}.idle", idle.get("set"))
        at = state.get("at")
        if at is not None:
            if not isinstance(at, dict) or len(at) > AT_MAX:
                errors.append(f"{name}.at: must be a map of up to {AT_MAX} delays (10s: {{set, cue}})")
                continue
            for delay, step in at.items():
                where = f"{name}.at.{delay}"
                if not re.fullmatch(r"\d+[sm]", str(delay)):
                    errors.append(f"{where}: must look like 10s or 2m")
                if not isinstance(step, dict):
                    errors.append(f"{where}: must be a map with cue and/or set")
                    continue
                if "goto" in step:
                    errors.append(f"{where}: no goto here; use after")
                check_cues(where, step.get("cue"))
                check_set(where, step.get("set"))
    return errors


def post(host, body):
    url = host if "://" in host else f"http://{host}/config"
    req = urllib.request.Request(
        url, data=body.encode(), method="POST",
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            print(f"{url} -> {resp.status} {resp.read().decode().strip()}")
    except urllib.error.HTTPError as e:
        sys.stderr.write(f"{url} -> {e.code} {e.read().decode().strip()}\n")
        sys.exit(1)
    except (urllib.error.URLError, TimeoutError) as e:
        sys.stderr.write(f"{url} unreachable: {e}\n")
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("config", nargs="?", default=DEFAULT_CONFIG)
    ap.add_argument("-o", "--output", help="JSON path (default: next to the YAML)")
    ap.add_argument("--pretty", action="store_true",
                    help="indent the JSON (default is compact, for the board)")
    ap.add_argument("--post", metavar="HOST",
                    help="also POST the JSON to the arbiter at HOST (e.g. 192.168.4.1)")
    args = ap.parse_args()

    cfg = load_config(args.config)

    errors = validate(cfg)
    if errors:
        sys.stderr.write(f"{args.config}: {len(errors)} problem(s)\n")
        for e in errors:
            sys.stderr.write(f"  {e}\n")
        sys.exit(1)

    body = json.dumps(for_arbiter(cfg), indent=2 if args.pretty else None,
                      separators=None if args.pretty else (",", ":"))
    out = args.output or os.path.splitext(args.config)[0] + ".json"
    with open(out, "w") as f:
        f.write(body + "\n")
    print(f"wrote {out} ({len(body)} bytes)")

    if args.post:
        post(args.post, body)


if __name__ == "__main__":
    main()
