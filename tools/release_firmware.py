#!/usr/bin/env python3
"""Build firmware batches. Every batch is stamped with the Unix time the build was
triggered; all images in it share that timestamp. Nothing is published."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid
from datetime import datetime
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
ENVS = ("sensor", "screen", "component_state_display", "screen_extractor", "screen_diorama", "screen_party", "screen_television", "arbiter_mesh", "arbiter_crowpanel7", "bridge", "bridge_esp32")


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


def new_batch(envs):
    """One timestamp for the whole batch, and a fresh build identity per env.
    The nonce makes every trigger distinct, which also covers changed toolchains."""
    built_at = int(time.time())
    nonce = uuid.uuid4().hex
    return built_at, {env: hashlib.sha256(f"pulleys-fw-v2\0{built_at}\0{nonce}\0{env}".encode()).hexdigest() for env in envs}


def format_built(built_at):
    """The build time as Pacific wall-clock time, e.g. '2026-11-13 09:41 PST'."""
    return datetime.fromtimestamp(int(built_at), ZoneInfo("America/Los_Angeles")).strftime("%Y-%m-%d %H:%M %Z")


def build_bundle(envs, built_at, identities, output, *, pio=None, progress=None):
    """Build each selected env once and package the exact resulting binaries."""
    executable = pio or pio_path()
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    manifest = {"schema": 1, "tag": str(built_at), "built_at": int(built_at), "firmware": {}}
    unique = list(dict.fromkeys(envs))
    if progress:
        progress.begin(unique)
    for env in unique:
        identity = identities[env]
        if not re.fullmatch(r"[0-9a-f]{64}", identity):
            raise ValueError("build identity must be a SHA-256 value")
        child_env = os.environ.copy()
        child_env["PULLEYS_FIRMWARE_BUILD_HASH"] = identity
        if progress:
            progress.run(env, [executable, "run", "-e", env], ROOT, child_env)
        else:
            print(f"Building {env}…", flush=True)
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
    print(f"Built {built_at} ({format_built(built_at)}): {path}", flush=True)
    return path, manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--env", action="append", help="build only this env (repeatable); default is every production env")
    args = parser.parse_args(argv)
    envs = args.env or list(ENVS)
    try:
        built_at, identities = new_batch(envs)
        build_bundle(envs, built_at, identities, ROOT / "dist" / str(built_at))
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(str(exc))


if __name__ == "__main__":
    main()
