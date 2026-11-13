"""`./fw flash` chooses firmware from the working tree's changes: the table, the plan, the CLI."""
import configparser
import contextlib
import io
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from tools import firmware_changes as changes
from tools import firmware_cli as cli
from tools import release_firmware as bundles

ROOT = Path(__file__).resolve().parents[1]


class PlanTests(unittest.TestCase):
    def plan(self, *paths):
        return changes.plan(paths, bundles.ENVS)

    def test_each_firmware_directory_rebuilds_only_its_own_firmware(self):
        self.assertEqual(self.plan("src/sensor/main.cpp").envs, ["sensor"])
        self.assertEqual(self.plan("src/screen/main.cpp").envs, list(changes.SCREENS))
        self.assertEqual(self.plan("src/television/messages.h").envs, ["screen_television"])
        self.assertEqual(self.plan("src/arbiter_mesh/ui.cpp").envs, ["arbiter_mesh", "arbiter_crowpanel7"])
        self.assertEqual(self.plan("src/bridge/main.cpp").envs, ["bridge", "bridge_esp32"])

    def test_a_shared_library_rebuilds_everything_that_includes_it(self):
        self.assertEqual(set(self.plan("lib/pulleys_extractor/pulleys_extractor.h").envs), set(changes.SCREENS))
        self.assertEqual(set(self.plan("lib/pulleys_install/pulleys_install.h").envs), set(bundles.ENVS))
        self.assertEqual(set(self.plan("lib/pulleys_lcd/x.cpp").envs), set(changes.TELEVISION + changes.ARBITERS))

    def test_the_baked_in_game_only_touches_the_arbiter(self):
        plan = self.plan("game_state_machine/state_config.yaml")
        self.assertEqual(plan.envs, list(changes.ARBITERS))
        self.assertEqual(list(plan.triggered), ["game_state_machine/state_config.yaml"])

    def test_build_system_files_rebuild_everything(self):
        for path in ("platformio.ini", "boards/esp32s3_16mb_8mbpsram.json", "tools/build_hash.py"):
            self.assertEqual(set(self.plan(path).envs), set(bundles.ENVS), path)

    def test_several_changes_are_a_union_in_the_production_order(self):
        plan = self.plan("src/bridge/main.cpp", "src/sensor/main.cpp", "lib/pulleys_detect/d.cpp")
        self.assertEqual(plan.envs, ["sensor", "bridge", "bridge_esp32"])
        self.assertEqual(plan.skipped, [env for env in bundles.ENVS if env not in plan.envs])
        self.assertEqual(plan.triggered["src/sensor/**"][1], ["src/sensor/main.cpp"])
        self.assertEqual(plan.triggered["lib/pulleys_detect/**"][1], ["lib/pulleys_detect/d.cpp"])

    def test_docs_overlord_tests_and_retired_firmware_build_nothing(self):
        plan = self.plan("README.md", "overlord/overlord.py", "test/test_x.py", "tools/overlord_restart.sh",
                         "src/traveler/main.cpp", "lib/pulleys_ritual/r.h", "game_state_machine/state_config.json")
        self.assertEqual(plan.envs, [])
        self.assertEqual(sorted(plan.retired), ["lib/pulleys_ritual/r.h", "src/traveler/main.cpp"])
        self.assertEqual(len(plan.not_firmware), 5)
        self.assertIn("nothing", plan.report("test"))

    def test_a_sibling_name_is_not_a_match(self):
        self.assertEqual(self.plan("src/sensor_old/main.cpp").envs, [])
        self.assertEqual(self.plan("lib/pulleys_install_notes/x.md").envs, [])

    def test_the_report_names_every_pattern_file_and_decision(self):
        text = self.plan("src/sensor/a.cpp", "src/sensor/b.cpp", "README.md").report("git diff HEAD")
        for expected in ("CHOOSING FIRMWARE FROM YOUR CHANGES (git diff HEAD)", "pattern src/sensor/**   [2 files]",
                         "rebuilds: sensor", "src/sensor/a.cpp", "not firmware, ignored (1): README.md",
                         "=> BUILD: sensor", "=> SKIP", "--all"):
            self.assertIn(expected, text)

    def test_long_file_lists_are_cut_short_but_counted(self):
        text = self.plan(*[f"src/sensor/f{n}.cpp" for n in range(10)]).report("x")
        self.assertIn("[10 files]", text)
        self.assertIn("... and 4 more", text)


class TableMatchesTheCode(unittest.TestCase):
    """The table is hand-written; these fail when the code moves out from under it."""

    def build_src_dirs(self):
        parser = configparser.RawConfigParser(strict=False)
        parser.read(ROOT / "platformio.ini")
        def option(env, key):
            section = f"env:{env}"
            if parser.has_option(section, key):
                return parser.get(section, key)
            if parser.has_option(section, "extends"):
                return option(parser.get(section, "extends").split(":")[1], key)
            return ""
        return {env: ["src/" + d.rstrip("/") for d in re.findall(r"\+<([^>]+)>", option(env, "build_src_filter"))]
                for env in bundles.ENVS}

    def includes(self, directory):
        found = set()
        for path in (ROOT / directory).rglob("*"):
            if path.suffix in {".cpp", ".h", ".c", ".hpp", ".ino"}:
                found.update(re.findall(r'#\s*include\s*[<"]([^>"]+)[>"]', path.read_text(errors="ignore")))
        return found

    def libraries_used(self, source_dirs):
        owner = {path.name: lib.name for lib in (ROOT / "lib").iterdir() if lib.is_dir()
                 for path in lib.rglob("*") if path.suffix in {".h", ".hpp"}}
        used, pending = set(), [h for d in source_dirs for h in self.includes(d)]
        while pending:
            lib = owner.get(Path(pending.pop()).name)
            if lib and lib not in used:
                used.add(lib)
                pending += self.includes("lib/" + lib)
        return used

    def test_production_envs_are_the_table_s_envs(self):
        self.assertEqual(list(changes.EVERYTHING), list(bundles.ENVS))

    def test_every_source_and_library_directory_is_accounted_for(self):
        patterns = [pattern for pattern, _ in changes.MAP] + changes.RETIRED
        for parent in ("src", "lib"):
            for directory in sorted(p for p in (ROOT / parent).iterdir() if p.is_dir()):
                probe = f"{parent}/{directory.name}/x"
                self.assertTrue(any(changes.matches(p, probe) for p in patterns),
                                f"{parent}/{directory.name}/ is in neither MAP nor RETIRED in tools/firmware_changes.py")

    def test_each_env_s_source_directory_is_in_the_table(self):
        for env, dirs in self.build_src_dirs().items():
            self.assertTrue(dirs, f"{env} has no build_src_filter")
            for directory in dirs:
                self.assertIn(env, self.table_envs(f"{directory}/x.cpp"), f"{directory} must rebuild {env}")

    def table_envs(self, path):
        return {env for pattern, envs in changes.MAP if changes.matches(pattern, path) for env in envs}

    def test_a_library_change_rebuilds_every_env_that_includes_it(self):
        for env, dirs in self.build_src_dirs().items():
            for lib in self.libraries_used(dirs):
                self.assertIn(env, self.table_envs(f"lib/{lib}/x.h"),
                              f"{env} includes {lib}, but a change to lib/{lib}/ would not rebuild it: fix MAP in tools/firmware_changes.py")


class ChangedFilesTests(unittest.TestCase):
    def test_staged_unstaged_untracked_and_deleted_all_count_but_ignored_files_do_not(self):
        with tempfile.TemporaryDirectory() as directory:
            def git(*args):
                subprocess.run(["git", "-c", "user.email=t@t", "-c", "user.name=t", *args], cwd=directory, check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            def write(name, text):
                path = Path(directory) / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
            for name in ("src/sensor/a.cpp", "src/sensor/b.cpp", "src/bridge/c.cpp", "README.md"):
                write(name, "1")
            write(".gitignore", ".pio/\n")
            git("init", "-q")
            git("add", ".")
            git("commit", "-q", "-m", "base")
            write("src/sensor/a.cpp", "2")                 # modified, unstaged
            write("src/bridge/c.cpp", "2")
            git("add", "src/bridge/c.cpp")                  # modified, staged
            os.remove(Path(directory) / "src/sensor/b.cpp") # deleted
            write("src/screen/new.cpp", "1")                # untracked
            write(".pio/build/x.bin", "1")                  # ignored
            self.assertEqual(changes.changed_files(directory),
                             ["src/bridge/c.cpp", "src/screen/new.cpp", "src/sensor/a.cpp", "src/sensor/b.cpp"])
            git("commit", "-qam", "all")
            self.assertEqual(changes.changed_files(directory), ["src/screen/new.cpp"])   # still untracked
            self.assertEqual(changes.changed_files(directory, since="HEAD~1"),
                             ["src/bridge/c.cpp", "src/screen/new.cpp", "src/sensor/a.cpp", "src/sensor/b.cpp"])


def snapshot():
    return {"devices": [
        {"id": "E857", "env": "sensor", "role": "SENSOR", "sha256": "unknown", "online": True},
        {"id": "59A4", "env": "screen_extractor", "role": "SCREEN", "sha256": "unknown", "online": True},
        {"id": "A9C8", "env": "arbiter_crowpanel7", "role": "ARBITER", "sha256": "unknown", "online": True},
        {"id": "1966", "env": "bridge_esp32", "role": "BRIDGE", "sha256": "unknown", "online": True}],
        "jobs": [], "releases": []}


class FlashDefaultTests(unittest.TestCase):
    def flash(self, *arguments, changed=()):
        output, errors = io.StringIO(), io.StringIO()
        with patch.object(cli.changes, "changed_files", return_value=list(changed)) as git, \
             patch.object(cli, "request", return_value=snapshot()), \
             patch.object(cli, "build_development", side_effect=RuntimeError("stop after the decision")) as build, \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            code = cli.main(["flash", *arguments])
        return code, output.getvalue(), errors.getvalue(), build, git

    def test_builds_only_what_the_changes_touch_and_says_so(self):
        _, out, _, build, _ = self.flash(changed=["src/arbiter_mesh/main.cpp", "README.md"])
        self.assertIn("CHOOSING FIRMWARE FROM YOUR CHANGES", out)
        self.assertIn("pattern src/arbiter_mesh/**", out)
        self.assertIn("=> BUILD: arbiter_mesh, arbiter_crowpanel7", out)
        build.assert_called_once_with(["arbiter_crowpanel7"], verbose=False)   # the one deployed arbiter env

    def test_a_screen_change_leaves_the_sensors_and_bridge_alone(self):
        _, out, _, build, _ = self.flash(changed=["src/screen/main.cpp"])
        build.assert_called_once_with(["screen_extractor"], verbose=False)
        self.assertIn("=> SKIP", out)

    def test_nothing_firmware_changed_builds_nothing_and_says_how_to_override(self):
        code, out, _, build, _ = self.flash(changed=["README.md", "overlord/overlord.py"])
        self.assertEqual(code, 0)
        build.assert_not_called()
        self.assertIn("Nothing to build or flash", out)
        self.assertIn("--all", out)

    def test_choosing_yourself_skips_git_and_says_so(self):
        for arguments, flag in ((["--env", "sensor"], "--env"), (["--id", "E857"], "--id"),
                                (["--type", "bridge"], "--type"), (["--all"], "--all")):
            with self.subTest(arguments=arguments):
                _, out, _, build, git = self.flash(*arguments, changed=["src/arbiter_mesh/main.cpp"])
                git.assert_not_called()
                self.assertIn(f"Firmware chosen by you ({flag}); git changes were not consulted.", out)
                self.assertNotIn("CHOOSING FIRMWARE", out)
                build.assert_called_once()

    def test_all_flashes_every_known_node_as_before(self):
        _, _, _, build, _ = self.flash("--all", changed=[])
        self.assertEqual(set(build.call_args.args[0]), {"sensor", "screen_extractor", "arbiter_crowpanel7", "bridge_esp32"})

    def test_since_is_passed_through_to_git(self):
        _, out, _, _, git = self.flash("--since", "origin/main", changed=["src/sensor/main.cpp"])
        git.assert_called_once_with(cli.ROOT, "origin/main")
        self.assertIn("git diff origin/main", out)

    def test_changes_to_firmware_no_known_device_runs_is_an_explained_error(self):
        code, out, err, build, _ = self.flash(changed=["src/television/messages.h"])
        self.assertEqual(code, 1)
        self.assertIn("no known device runs the firmware your changes touch (screen_television)", err)
        build.assert_not_called()

    def test_dry_run_shows_the_decision_without_building(self):
        _, out, _, build, _ = self.flash("--dry-run", changed=["src/sensor/main.cpp"])
        self.assertIn("=> BUILD: sensor", out)
        self.assertIn("Dry run", out)
        build.assert_not_called()


if __name__ == "__main__":
    unittest.main()
