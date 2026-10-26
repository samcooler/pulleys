#!/usr/bin/env python3
"""Build and publish firmware binaries for the latest minor GitHub Release.

Requires `pio` and the authenticated GitHub CLI (`gh`). Run from any directory:
    python3 tools/release_firmware.py
    python3 tools/release_firmware.py --build-only
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENVS = ("sensor", "screen", "component_state_display", "arbiter_mesh", "bridge")
PIO = os.environ.get("PLATFORMIO_CMD") or shutil.which("pio") or str(Path.home() / ".platformio/penv/bin/pio")


def run(args, *, capture=False):
    return subprocess.run(args, cwd=ROOT, check=True, text=True,
                          stdout=subprocess.PIPE if capture else None)


def latest_version():
    result = run(["gh", "release", "list", "--limit", "100", "--json", "tagName"], capture=True)
    versions = []
    for row in json.loads(result.stdout):
        match = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)", row["tagName"])
        if match:
            versions.append(tuple(map(int, match.groups())))
    if not versions:
        return "v0.1.0"
    major, minor, _patch = max(versions)
    return f"v{major}.{minor + 1}.0"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-only", action="store_true",
                        help="build binaries and manifest without creating a GitHub release")
    parser.add_argument("--tag", help="explicit release tag (normally auto-incremented)")
    args = parser.parse_args()
    if not args.build_only:
        if not Path(PIO).is_file():
            sys.exit(f"PlatformIO CLI not found at {PIO}; set PLATFORMIO_CMD if it is elsewhere.")
        if shutil.which("gh") is None:
            sys.exit("GitHub CLI not found: gh")
        if subprocess.call(["gh", "auth", "status"], cwd=ROOT) != 0:
            sys.exit("Authenticate GitHub CLI first with `gh auth login`.")

    dirty = subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True)
    if dirty:
        sys.exit("Commit or stash changes before making release assets; build identity is tied to HEAD.")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()

    tag = args.tag or (latest_version() if not args.build_only else "v0.1.0")
    if not re.fullmatch(r"v\d+\.\d+\.\d+", tag):
        sys.exit("Tag must use vMAJOR.MINOR.PATCH format.")
    out = ROOT / "dist" / tag
    out.mkdir(parents=True, exist_ok=True)
    manifest = {"schema": 1, "tag": tag, "firmware": {}}
    for env in ENVS:
        print(f"Building {env}…", flush=True)
        build_hash = hashlib.sha256(f"pulleys-fw-v1\0{tag}\0{commit}\0{env}".encode()).hexdigest()
        child_env = os.environ.copy()
        child_env["PULLEYS_FIRMWARE_BUILD_HASH"] = build_hash
        subprocess.run([PIO, "run", "-e", env], cwd=ROOT, check=True, env=child_env)
        source = ROOT / ".pio" / "build" / env / "firmware.bin"
        if not source.is_file():
            sys.exit(f"PlatformIO did not produce {source}")
        asset = f"{env}-firmware.bin"
        target = out / asset
        target.write_bytes(source.read_bytes())
        data = target.read_bytes()
        manifest["firmware"][env] = {
            "asset": asset,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "md5": hashlib.md5(data, usedforsecurity=False).hexdigest(),
            "build_hash": build_hash,
        }
    manifest_path = out / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Built {tag} into {out}")
    if args.build_only:
        return
    run(["gh", "release", "create", tag,
         *[str(out / f"{env}-firmware.bin") for env in ENVS], str(manifest_path),
         "--title", tag, "--notes", f"Pulleys firmware release {tag}"])
    print(f"Published GitHub Release {tag}")


if __name__ == "__main__":
    main()
