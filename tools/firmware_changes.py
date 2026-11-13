"""Which firmware does a change to the working tree affect? (used by `./fw flash`)

A plain table: a path pattern on the left, the firmware types it rebuilds on the
right. `./fw flash` with no selection reads `git` for what has changed, looks
each changed file up here, and builds only the firmware types that came out.
It says what it matched, so the choice is never a surprise.

    "src/sensor/**"   a directory and everything under it
    "platformio.ini"  one file, exactly

A file can match several patterns; the firmware types are the union. A file that
matches none is not firmware (docs, overlord/, tests, ...) and affects nothing.

KEEP THIS TABLE IN STEP WITH THE CODE. test/test_firmware_changes.py re-derives
which libraries each firmware type includes and fails if this table says less,
and fails on any src/ or lib/ directory that is in neither the table nor RETIRED.
"""
import fnmatch
import subprocess

# The production firmware types, as release_firmware.ENVS names them, grouped by
# the source directory they are built from (build_src_filter in platformio.ini).
SENSOR = ("sensor",)
SCREENS = ("screen", "component_state_display", "screen_extractor", "screen_diorama", "screen_party")
TELEVISION = ("screen_television",)
ARBITERS = ("arbiter_mesh", "arbiter_crowpanel7")
BRIDGES = ("bridge", "bridge_esp32")
EVERYTHING = SENSOR + SCREENS + TELEVISION + ARBITERS + BRIDGES

# pattern -> firmware types it rebuilds
MAP = [
    # ── the firmware's own source ────────────────────────────────────────────
    ("src/sensor/**",      SENSOR),
    ("src/screen/**",      SCREENS),
    ("src/television/**",  TELEVISION),
    ("src/arbiter_mesh/**", ARBITERS),
    ("src/bridge/**",      BRIDGES),

    # ── libraries: whoever includes them (derived from #include, see the test) ──
    ("lib/pulleys_census/**",    EVERYTHING),
    ("lib/pulleys_identity/**",  EVERYTHING),
    ("lib/pulleys_install/**",   EVERYTHING),   # the rope/display tables every board carries
    ("lib/pulleys_mesh/**",      EVERYTHING),
    ("lib/pulleys_ota/**",       EVERYTHING),
    ("lib/pulleys_protocol/**",  EVERYTHING),
    ("lib/pulleys_whoami/**",    EVERYTHING),
    ("lib/pulleys_channel/**",   SENSOR + SCREENS + ARBITERS),
    ("lib/pulleys_culture/**",   SENSOR + SCREENS + ARBITERS),
    ("lib/pulleys_patterns/**",  SENSOR + SCREENS + ARBITERS),
    ("lib/pulleys_settings/**",  SCREENS + TELEVISION + ARBITERS),
    ("lib/pulleys_ch422g/**",    TELEVISION + ARBITERS),
    ("lib/pulleys_lcd/**",       TELEVISION + ARBITERS),
    ("lib/pulleys_textcard/**",  TELEVISION + ARBITERS),
    ("lib/pulleys_panel/**",     SENSOR + SCREENS),
    ("lib/pulleys_wave/**",      SENSOR + SCREENS),
    ("lib/pulleys_detect/**",    SENSOR),
    ("lib/pulleys_imu/**",       SENSOR),
    ("lib/pulleys_component_display/**", SCREENS),
    ("lib/pulleys_diorama/**",   SCREENS),
    ("lib/pulleys_extractor/**", SCREENS),
    ("lib/pulleys_gt911/**",     ARBITERS),
    ("lib/pulleys_rtc/**",       ARBITERS),
    ("lib/pulleys_fw_serial/**", BRIDGES),

    # ── how everything is built ──────────────────────────────────────────────
    ("platformio.ini",       EVERYTHING),
    ("boards/**",            EVERYTHING),
    ("tools/build_hash.py",  EVERYTHING),   # extra_scripts for every env
    ("include/**",           ARBITERS),     # LVGL's config, on the include path of the arbiter

    # ── the game the arbiter has baked in (tools/embed_game.py) ───────────────
    ("game_state_machine/state_config.yaml", ARBITERS),
    ("tools/embed_game.py",   ARBITERS),
    ("tools/game_to_json.py", ARBITERS),
    ("playtest/spec.py",      ARBITERS),     # game_to_json imports it
]

# Directories that exist but build into no production firmware (retired envs, or
# libraries only they use). A change here affects nothing `fw` flashes.
RETIRED = [
    "src/arbiter/**", "src/station/**", "src/traveler/**",
    "lib/pulleys_proximity/**", "lib/pulleys_ritual/**",
]


def matches(pattern, path):
    if pattern.endswith("/**"):
        return path.startswith(pattern[:-2])
    return fnmatch.fnmatchcase(path, pattern)


def changed_files(root, since=None):
    """Paths git says differ: from HEAD (or `since`) to the working tree, staged or
    not, plus new files git does not track yet. Renames count as delete + add, so
    a file moved out of a firmware directory still counts for that directory."""
    def git(*args):
        out = subprocess.run(["git", *args], cwd=root, check=True, text=True, stdout=subprocess.PIPE).stdout
        return [line for line in out.split("\0") if line]
    tracked = git("diff", "--name-only", "--no-renames", "-z", since or "HEAD")
    untracked = git("ls-files", "--others", "--exclude-standard", "-z")
    return sorted(set(tracked) | set(untracked))


class Plan:
    """The decision: which patterns the changes triggered, which files matched
    nothing, and the firmware types that follow."""

    def __init__(self, paths, envs_in_order):
        self.paths = list(paths)
        self.triggered = {}      # pattern -> (envs, [files])
        self.retired = []        # files only a retired env uses
        self.not_firmware = []   # files no pattern claims
        for path in self.paths:
            hits = [(pattern, envs) for pattern, envs in MAP if matches(pattern, path)]
            for pattern, envs in hits:
                self.triggered.setdefault(pattern, (envs, []))[1].append(path)
            if not hits:
                (self.retired if any(matches(p, path) for p in RETIRED) else self.not_firmware).append(path)
        wanted = {env for envs, _ in self.triggered.values() for env in envs}
        self.envs = [env for env in envs_in_order if env in wanted]
        self.skipped = [env for env in envs_in_order if env not in wanted]

    def report(self, source):
        """The text `fw flash` prints, so the choice is on screen before anything builds."""
        bar = "=" * 72
        lines = [bar, f"CHOOSING FIRMWARE FROM YOUR CHANGES ({source})", bar]
        if not self.paths:
            lines.append("No changed files.")
        for pattern, (envs, files) in self.triggered.items():
            lines.append(f"pattern {pattern}   [{len(files)} file{'s' * (len(files) != 1)}]")
            lines.append(f"    rebuilds: {describe(envs)}")
            for path in files[:6]:
                lines.append(f"      {path}")
            if len(files) > 6:
                lines.append(f"      ... and {len(files) - 6} more")
        if self.retired:
            lines.append(f"retired firmware only ({len(self.retired)}): " + ", ".join(self.retired[:4]) + (" ..." if len(self.retired) > 4 else ""))
        if self.not_firmware:
            lines.append(f"not firmware, ignored ({len(self.not_firmware)}): " + ", ".join(self.not_firmware[:4]) + (" ..." if len(self.not_firmware) > 4 else ""))
        if self.envs:
            lines.append(f"=> BUILD: {', '.join(self.envs)}")
            if self.skipped:
                lines.append(f"=> SKIP (nothing they use changed): {', '.join(self.skipped)}")
        else:
            lines.append("=> BUILD: nothing. No changed file maps to firmware.")
        lines.append("   Choose yourself instead with --all, --since REF, or --id / --type / --env.")
        lines.append(bar)
        return "\n".join(lines)


def describe(envs):
    return f"all {len(envs)} firmware types" if len(envs) == len(EVERYTHING) else ", ".join(envs)


def plan(paths, envs_in_order=EVERYTHING):
    return Plan(paths, envs_in_order)
