#!/usr/bin/env python3
"""Build release-tool manifests; legacy entry point publishes unless --build-only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ENVS = ("sensor", "screen", "component_state_display", "screen_extractor", "screen_diorama", "screen_party", "arbiter_mesh", "arbiter_crowpanel7", "bridge", "bridge_esp32")


def pio_path():
    configured = os.environ.get("PLATFORMIO_CMD")
    executable = shutil.which(configured or "pio")
    if not executable and not configured:
        candidate = Path.home() / ".platformio/penv/bin/pio"
        if candidate.is_file():
            executable = str(candidate)
    if not executable:
        raise RuntimeError("PlatformIO not found; install pio or set PLATFORMIO_CMD to its executable")
    return executable


def run(args, *, capture=False):
    return subprocess.run(args, cwd=ROOT, check=True, text=True,
                          stdout=subprocess.PIPE if capture else None)


def latest_version():
    rows = json.loads(run(["gh", "release", "list", "--limit", "100", "--json", "tagName"], capture=True).stdout)
    versions = []
    for row in rows:
        match = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)", row["tagName"])
        if match:
            versions.append(tuple(map(int, match.groups())))
    if not versions:
        return "v0.1.0"
    major, minor, _ = max(versions)
    return f"v{major}.{minor + 1}.0"


def build_bundle(envs, tag, identities, output, *, pio=None):
    """Build each selected env once and package the exact resulting binaries."""
    executable = pio or pio_path()
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    manifest = {"schema": 1, "tag": tag, "firmware": {}}
    for env in dict.fromkeys(envs):
        identity = identities[env]
        if not re.fullmatch(r"[0-9a-f]{64}", identity):
            raise ValueError("build identity must be a SHA-256 value")
        print(f"Building {env}…", flush=True)
        child_env = os.environ.copy()
        child_env["PULLEYS_FIRMWARE_BUILD_HASH"] = identity
        subprocess.run([executable, "run", "-e", env], cwd=ROOT, check=True, env=child_env)
        source = ROOT / ".pio/build" / env / "firmware.bin"
        data = source.read_bytes()
        if identity.encode() not in data:
            raise RuntimeError(f"{env}: build output does not contain the requested identity; refusing stale firmware")
        asset = f"{env}-firmware.bin"
        (output / asset).write_bytes(data)
        manifest["firmware"][env] = {
            "asset": asset, "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "md5": hashlib.md5(data, usedforsecurity=False).hexdigest(), "build_hash": identity,
        }
    path = output / "manifest.json"
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(manifest, indent=2) + "\n")
    temporary.replace(path)
    print(f"Built {tag}: {path}", flush=True)
    return path, manifest


def release_bundle(tag=None, *, publish=False):
    executable = pio_path()
    dirty = subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT, text=True)
    if dirty:
        raise RuntimeError("Commit or stash changes before a release; use './fw build' for uncommitted development firmware")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if publish:
        if not shutil.which("gh"):
            raise RuntimeError("GitHub CLI not found; install gh and run gh auth login")
        run(["gh", "auth", "status"])
    tag = tag or (latest_version() if publish else "v0.1.0")
    if not re.fullmatch(r"v\d+\.\d+\.\d+", tag):
        raise ValueError("release tag must use vMAJOR.MINOR.PATCH format")
    identities = {env: hashlib.sha256(f"pulleys-fw-v1\0{tag}\0{commit}\0{env}".encode()).hexdigest() for env in ENVS}
    path, manifest = build_bundle(ENVS, tag, identities, ROOT / "dist" / tag, pio=executable)
    if publish:
        run(["gh", "release", "create", tag,
             *[str(path.parent / item["asset"]) for item in manifest["firmware"].values()], str(path),
             "--target", commit, "--title", tag, "--notes", f"Pulleys firmware release {tag}"])
        print(f"Published GitHub Release {tag}")
    return path, manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--tag")
    args = parser.parse_args(argv)
    try:
        release_bundle(args.tag, publish=not args.build_only)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(str(exc))


if __name__ == "__main__":
    main()
