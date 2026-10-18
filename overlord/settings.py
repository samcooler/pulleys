"""Settings that survive a restart, changed from the debug menu."""

import json
import os
from pathlib import Path

PATH = Path(os.environ.get("OVERLORD_SETTINGS") or Path.home() / ".config" / "overlord" / "settings.json")


def load():
    try:
        return json.loads(PATH.read_text())
    except (OSError, ValueError):
        return {}


def save(**changes):
    """Merge changes into the file. Best-effort: a read-only disk must not break playback."""
    try:
        data = {**load(), **changes}
        PATH.parent.mkdir(parents=True, exist_ok=True)
        tmp = PATH.with_suffix(".tmp")
        tmp.write_text(json.dumps(data))
        tmp.replace(PATH)
    except OSError as e:
        print(f"settings: could not save: {e}", flush=True)
