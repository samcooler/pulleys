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


def cue_hash(cfg):
    """16-bit fingerprint of the cue table, carried in every cue packet so a
    receiver can tell its table from the sender's. FNV-1a over each group name
    (then 0x01) and each cue name (then 0x00), in file order, folded to 16 bits.
    lib/pulleys_cues implements the same thing for the boards."""
    h = 2166136261

    def add(text, sep):
        nonlocal h
        for b in text.encode() + bytes([sep]):
            h = ((h ^ b) * 16777619) & 0xFFFFFFFF

    for group, cues in (cfg["cues"] or {}).items():
        add(group, 1)
        for cue in cues or []:
            add(cue, 0)
    return ((h >> 16) ^ h) & 0xFFFF


# Sections only overlord reads; the arbiter's JSON leaves them out.
OVERLORD_ONLY = ("ambient_sounds",)


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

    # A cue is sent as [folder, file], one byte each (GAME_FORMAT.md).
    groups = cfg["cues"] or {}
    if len(groups) > 256:
        errors.append(f"cues: {len(groups)} groups; the folder index is one byte (max 256)")
    for group, cues in groups.items():
        if len(cues or []) > 256:
            errors.append(f"cues.{group}: {len(cues)} cues; the file index is one byte (max 256)")

    def check_cues(where, cues):
        for cue in cues or []:
            if cue not in known_cues:
                errors.append(f"{where}: cue '{cue}' is not listed under cues")

    def check_goto(where, target):
        if target not in states:
            errors.append(f"{where}: goto '{target}' is not a state")

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
            action = action or {}
            check_cues(f"{name}.on.{event}", action.get("cue"))
            if "goto" in action:
                check_goto(f"{name}.on.{event}", action["goto"])
        after = state.get("after")
        if after:
            if "goto" in after:
                check_goto(f"{name}.after", after["goto"])
            else:
                errors.append(f"{name}.after: missing goto")
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
    print(f"wrote {out} ({len(body)} bytes, cue hash {cue_hash(cfg):04X})")

    if args.post:
        post(args.post, body)


if __name__ == "__main__":
    main()
