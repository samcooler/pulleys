#!/usr/bin/env python3
"""Balance the audio clips: set `audio_gain:` in the game YAML from the Drive folder.

Pulls the media folder from Google Drive (needs `pip install gdown`; the folder
must be shared as "anyone with the link"), measures each audio clip's average
loudness with ffmpeg (EBU R128 integrated loudness, in LUFS), and writes the
gain that brings every clip to the same loudness. Prints each cue's old and new
value. The rest of the YAML, comments included, is left alone.

    python3 tools/balance_audio.py                 # download, measure, write
    python3 tools/balance_audio.py --dry-run       # show the changes only
    python3 tools/balance_audio.py --no-download   # reuse the last download
    python3 tools/balance_audio.py --target -20    # aim at -20 LUFS, not the median clip

Cues only on this machine and not in the Drive folder (beep, chime) keep their
gain. A clip that would clip after the boost is flagged but still written.
"""

import argparse
import math
import re
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_YAML = ROOT / "game_state_machine" / "state_config.yaml"
DEFAULT_CACHE = Path.home() / ".cache" / "pulleys-drive"
DRIVE_FOLDER = "https://drive.google.com/drive/folders/1ztfY8nkVEQuVMculoi4RvDAwYBr-PmnH"

# Drive folder -> cue prefix. Same layout as overlord/media.
FOLDERS = {"audio_loops": "audio_loop_", "audio_once": "audio_once_"}

# Extra dB added to a whole category's target, for when background loops should
# sit quieter than effects (e.g. "audio_loop_": -6). 0 means everything is equal.
CATEGORY_OFFSET_DB = {"audio_loop_": 0.0, "audio_once_": 0.0}

MAX_GAIN = 10.0  # overlord/gain.py rejects anything above this


def download(cache):
    cache.mkdir(parents=True, exist_ok=True)
    try:
        import gdown
    except ImportError:
        sys.exit("gdown is not installed: pip install gdown")
    gdown.download_folder(url=DRIVE_FOLDER, output=str(cache), quiet=True, use_cookies=False)


def measure(path):
    """(integrated LUFS, true peak dBTP) of a file, or None if it has no audio."""
    r = subprocess.run(
        ["ffmpeg", "-nostats", "-hide_banner", "-i", str(path), "-vn",
         "-af", "ebur128=peak=true", "-f", "null", "-"],
        capture_output=True, text=True)
    summary = r.stderr.rsplit("Summary:", 1)[-1]
    lufs = re.search(r"I:\s+(-?[\d.]+) LUFS", summary)
    peak = re.search(r"Peak:\s+(-?[\d.]+) dBFS", summary)
    if not lufs or float(lufs.group(1)) < -69:  # silent or unreadable
        return None
    return float(lufs.group(1)), float(peak.group(1)) if peak else 0.0


def cue_name(prefix, path):
    return path.stem if path.stem.startswith(prefix) else prefix + path.stem


def clips(cache):
    found = {}
    for folder, prefix in FOLDERS.items():
        for p in sorted((cache / folder).glob("*")):
            if p.is_file() and not p.name.startswith("."):
                found[cue_name(prefix, p)] = p
    return found


def write_yaml(path, new):
    """Set each cue's value inside the `audio_gain:` block, keeping comments.
    Returns {cue: old value or None}."""
    lines = path.read_text().splitlines(keepends=True)
    try:
        start = next(i for i, l in enumerate(lines) if l.startswith("audio_gain:"))
    except StopIteration:
        sys.exit(f"{path} has no audio_gain: block")
    end = start + 1
    while end < len(lines) and (lines[end].startswith((" ", "\t")) or not lines[end].strip()):
        end += 1
    while not lines[end - 1].strip():  # blank lines belong after the block
        end -= 1
    old, todo = {}, dict(new)
    for i in range(start + 1, end):
        m = re.match(r"(\s+)([\w-]+):\s*([\d.]+)(.*)", lines[i], re.S)
        if m and m.group(2) in todo:
            cue = m.group(2)
            old[cue] = float(m.group(3))
            lines[i] = f"{m.group(1)}{cue}: {todo.pop(cue):g}{m.group(4)}"
            if not lines[i].endswith("\n"):
                lines[i] += "\n"
    for cue, value in todo.items():  # not in the file yet
        lines.insert(end, f"  {cue}: {value:g}\n")
        end += 1
        old[cue] = None
    path.write_text("".join(lines))
    return old


def read_old(path):
    import yaml
    return {k: float(v) for k, v in (yaml.safe_load(path.read_text()).get("audio_gain") or {}).items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--yaml", type=Path, default=DEFAULT_YAML)
    ap.add_argument("--cache", type=Path, default=DEFAULT_CACHE, help="where the Drive folder is downloaded")
    ap.add_argument("--no-download", action="store_true", help="use what is already in the cache")
    ap.add_argument("--target", type=float, metavar="LUFS", help="loudness to aim for (default: the median clip)")
    ap.add_argument("--dry-run", action="store_true", help="print the changes, write nothing")
    args = ap.parse_args()

    if not args.no_download:
        print(f"downloading {DRIVE_FOLDER} -> {args.cache}", flush=True)
        download(args.cache)

    measured = {}
    for cue, path in clips(args.cache).items():
        m = measure(path)
        if m is None:
            print(f"skipping {cue}: no audible audio in {path.name}")
        else:
            measured[cue] = m
    if not measured:
        sys.exit(f"no audio clips found under {args.cache}")

    offset = lambda cue: next(o for p, o in CATEGORY_OFFSET_DB.items() if cue.startswith(p))
    # Compare clips on a level footing: take each category's offset out first.
    target = args.target if args.target is not None else statistics.median(
        lufs - offset(cue) for cue, (lufs, _) in measured.items())

    new = {}
    for cue, (lufs, peak) in measured.items():
        db = target + offset(cue) - lufs
        new[cue] = round(min(10 ** (db / 20), MAX_GAIN), 2)

    old = read_old(args.yaml)
    print(f"\ntarget {target:.1f} LUFS ({'given' if args.target is not None else 'median clip'})\n")
    print(f"{'cue':38} {'LUFS':>6} {'old':>6} {'new':>6}")
    for cue in sorted(new):
        o = old.get(cue)
        flag = ""
        peak_after = measured[cue][1] + 20 * math.log10(new[cue]) if new[cue] else -99
        if peak_after > -1.0:
            flag = f"  peak would reach {peak_after:+.1f} dBFS (clipping risk)"
        print(f"{cue:38} {measured[cue][0]:6.1f} {'-' if o is None else format(o, 'g'):>6} {new[cue]:6g}{flag}")
    for cue in sorted(set(old) - set(new)):
        print(f"{cue:38} {'':6} {old[cue]:6g} {'(kept, not in Drive)':>6}")

    if args.dry_run:
        print("\ndry run: nothing written")
    else:
        write_yaml(args.yaml, new)
        print(f"\nwrote {args.yaml}")


if __name__ == "__main__":
    main()
