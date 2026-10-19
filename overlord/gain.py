"""Per-clip loudness: the optional `audio_gain:` map in the game YAML.

    audio_gain:
      audio_once_chime: 1.2     # 20% louder than the file is
      audio_loop_idle: 0.8      # 80% as loud

A multiplier on the file's own level, on top of the volume setting. A cue not
listed plays at 1. Only overlord reads this; the arbiter never sees it.
"""

try:
    import yaml
except ImportError:
    yaml = None


def load(path):
    """{cue name: multiplier} from the YAML at `path`. A bad entry is reported
    and skipped, so a typo costs one clip's balance and not the whole panel."""
    if yaml is None or not path:
        return {}
    try:
        with open(path) as f:
            config = yaml.safe_load(f) or {}
    except OSError as e:
        print(f"gain: could not read {path}: {e}", flush=True)
        return {}
    gains = {}
    for cue, value in (config.get("audio_gain") or {}).items():
        try:
            gain = float(value)
            if isinstance(value, bool) or not 0 <= gain <= 10:
                raise ValueError("need a number from 0 to 10")
        except (TypeError, ValueError) as err:
            print(f"gain: skipping {cue}: {err}", flush=True)
            continue
        gains[str(cue)] = gain
    return gains
