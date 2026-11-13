"""Waveforms for the Gain tab, drawn from the media files and kept on disk.

A cue's waveform is BINS peaks (0-1) and a duration, decoded once by ffmpeg
(or mpv, which the Overlord always has) and cached in a JSON file keyed by the
file's path, size and modified time, so a changed file is redone and an
unchanged one never is. A background thread fills the cache at startup and
whenever the page notices cues it has none for.
"""

import array
import json
import os
import shutil
import subprocess
import threading
from pathlib import Path

BINS = 120
RATE = 8000   # plenty for a loudness picture; keeps the decode small and quick
TIMEOUT = 120


def _decode_command(path):
    """The command that writes `path` as raw signed 16-bit mono PCM on stdout."""
    nice = [shutil.which("nice"), "-n", "15"] if shutil.which("nice") else []
    if shutil.which("ffmpeg"):
        return [*nice, "ffmpeg", "-v", "error", "-nostdin", "-i", str(path), "-vn", "-ac", "1",
                "-ar", str(RATE), "-f", "s16le", "-"]
    return [*nice, "mpv", "--no-config", "--really-quiet", "--no-video", "--no-terminal",
            "--audio-format=s16", f"--audio-samplerate={RATE}", "--audio-channels=mono",
            "--ao=pcm", "--ao-pcm-waveheader=no", "--ao-pcm-file=/dev/stdout", str(path)]


def analyze(path):
    """{"d": seconds, "p": [BINS peaks 0-1]} for a media file, or None if it has no sound."""
    out = subprocess.run(_decode_command(path), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         stdin=subprocess.DEVNULL, timeout=TIMEOUT).stdout
    samples = array.array("h")
    samples.frombytes(out[: len(out) // 2 * 2])
    if os.sys.byteorder == "big":
        samples.byteswap()
    if not samples:
        return None
    per = max(1, -(-len(samples) // BINS))   # ceil
    peaks = []
    for i in range(0, len(samples), per):
        chunk = samples[i:i + per]
        peaks.append(round(max(max(chunk), -min(chunk)) / 32768, 3))
    return {"d": round(len(samples) / RATE, 3), "p": peaks[:BINS]}


class WaveformCache:
    def __init__(self, media, cache_path):
        """media(cue) -> the sorted files that cue may play (first one is drawn)."""
        self._media = media
        self._path = Path(cache_path)
        self._lock = threading.Lock()
        self._data = {}        # file path -> {"key": [size, mtime_ns], "d":.., "p":..} (no "d" = no sound)
        self._worker = None
        self._todo = 0
        try:
            self._data = json.loads(self._path.read_text())
        except (OSError, ValueError):
            pass

    @staticmethod
    def _key(path):
        st = path.stat()
        return [st.st_size, st.st_mtime_ns]

    def _entry(self, cue):
        """(file, cached entry or None) for the cue's first file; (None, None) if no file."""
        files = self._media(cue)
        if not files:
            return None, None, 0
        try:
            key = self._key(files[0])
        except OSError:
            return None, None, 0
        with self._lock:
            e = self._data.get(str(files[0]))
        return files[0], (e if e and e.get("key") == key else None), len(files)

    def waves(self, cues):
        """{cue: {"d","p","name","n"}} for the cues that have a waveform now, and the
        number still to do. Starts the background fill if any are missing."""
        out, todo = {}, []
        for cue in cues:
            f, e, n = self._entry(cue)
            if f is None:
                continue
            if e is None:
                todo.append(cue)
            elif "p" in e:
                out[cue] = {"d": e["d"], "p": e["p"], "name": f.name, "n": n}
            else:   # decoded fine, but the file has no sound (a silent video)
                out[cue] = {"silent": True, "name": f.name, "n": n}
        if todo:
            self._start(todo)
        with self._lock:
            pending = self._todo if self._worker else 0
        return out, pending

    def _start(self, cues):
        with self._lock:
            if self._worker and self._worker.is_alive():
                return
            self._todo = len(cues)
            self._worker = threading.Thread(target=self._fill, args=(cues,), daemon=True)
            self._worker.start()

    def warm(self, cues):
        """Begin filling the cache for these cues in the background."""
        self.waves(cues)

    def _fill(self, cues):
        done = 0
        for cue in cues:
            f, e, _n = self._entry(cue)
            if f is not None and e is None:
                try:
                    result = analyze(f) or {}
                except (OSError, subprocess.SubprocessError) as err:
                    print(f"waveform: {f.name}: {err}", flush=True)
                    result = {}
                with self._lock:
                    self._data[str(f)] = {"key": self._key(f), **result}
                done += 1
                if done % 10 == 0:
                    self._save()
            with self._lock:
                self._todo = max(0, self._todo - 1)
        self._save()

    def _save(self):
        try:
            with self._lock:
                text = json.dumps(self._data)
            self._path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self._path.with_suffix(".tmp")
            tmp.write_text(text)
            tmp.replace(self._path)
        except OSError as err:
            print(f"waveform: could not save the cache: {err}", flush=True)
