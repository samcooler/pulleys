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


# The `cues:` groups whose cues make sound. Only these take a gain.
SOUND_GROUPS = ("audio_loop", "audio_bed", "audio_once", "video_once")


def cue_list(path):
    """[(cue, group)] for every cue in the YAML's `cues:` that makes sound, in
    file order. The `*_off` words are left out: they play nothing."""
    if yaml is None or not path:
        return []
    try:
        with open(path) as f:
            config = yaml.safe_load(f) or {}
    except OSError:
        return []
    cues = config.get("cues") or {}
    return [(str(c), g) for g in SOUND_GROUPS for c in (cues.get(g) or [])
            if not str(c).endswith("_off")]


def parse_gain(value):
    """A multiplier from 0 to 10, or ValueError."""
    if isinstance(value, bool):
        raise ValueError("need a number from 0 to 10")
    try:
        gain = float(value)
    except (TypeError, ValueError):
        raise ValueError("need a number from 0 to 10") from None
    if not 0 <= gain <= 10:  # also rejects nan
        raise ValueError("need a number from 0 to 10")
    return round(gain, 3)


class GainStore:
    """The gains in use: the YAML's, with live overrides on top. Overrides last
    until reset or until overlord restarts; the YAML file is never written.
    Safe to share between threads (get() is called from player threads)."""

    def __init__(self, path=None):
        import threading
        self.path = path
        self.media = None   # media(cue) -> the files that cue may play; lets rows say which have none
        self._lock = threading.Lock()
        self._base = {}
        self._over = {}
        self.reload()

    def reload(self):
        base = load(self.path)
        with self._lock:
            self._base = base

    def get(self, cue, default=1.0):
        with self._lock:
            if cue in self._over:
                return self._over[cue]
            return self._base.get(cue, default)

    def set(self, cue, value):
        gain = parse_gain(value)
        with self._lock:
            self._over[str(cue)] = gain
        return gain

    def set_many(self, values):
        """Set several at once; if any value is bad, none is applied."""
        parsed = {str(c): parse_gain(v) for c, v in values.items()}
        with self._lock:
            self._over.update(parsed)
        return parsed

    def reset(self, cue=None):
        """Drop one override, or every override when cue is None."""
        with self._lock:
            if cue is None:
                self._over.clear()
            else:
                self._over.pop(cue, None)

    def effective(self):
        with self._lock:
            return {**self._base, **self._over}

    def snapshot(self):
        """Rows for the tab: every sound cue, plus any gain for a cue that is not
        listed (a typo, or a cue since removed), so nothing hides."""
        with self._lock:
            base, over = dict(self._base), dict(self._over)
        rows, seen = [], set()
        for cue, group in cue_list(self.path):
            if cue not in seen:
                seen.add(cue)
                rows.append((cue, group))
        for cue in [*base, *over]:
            if cue not in seen:
                seen.add(cue)
                rows.append((cue, "unlisted"))
        cues = [{"cue": cue, "group": group, "yaml": base.get(cue, 1.0),
                 "in_yaml": cue in base, "gain": over.get(cue, base.get(cue, 1.0)),
                 "overridden": cue in over and over[cue] != base.get(cue, 1.0),
                 "missing": bool(self.media) and not self.media(cue)}
                for cue, group in rows]
        cues.sort(key=lambda c: c["missing"])   # cues with no media sink to the bottom, order kept
        return {"cues": cues,
                "overrides": len([c for c in over if over[c] != base.get(c, 1.0)])}

    def yaml_block(self):
        """The audio_gain: section as it would be pasted into the game YAML. A
        gain of 1 is the default, so it is left out unless the YAML had it."""
        with self._lock:
            base, merged = dict(self._base), {**self._base, **self._over}
        lines = ["audio_gain:"]
        order = [c for c, _ in cue_list(self.path)]
        order += [c for c in merged if c not in order]
        for cue in order:
            if cue in merged and (merged[cue] != 1 or cue in base):
                lines.append(f"  {cue}: {merged[cue]:g}")
        return "\n".join(lines) + "\n"
