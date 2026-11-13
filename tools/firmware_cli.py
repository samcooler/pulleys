"""Unified development firmware CLI. The executable entry point is ./fw."""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
import time

if __package__:
    from . import release_firmware as bundles
    from .push_firmware import request
    from .firmware_progress import BuildProgress, ProgressDisplay
    from . import firmware_changes as changes
else:
    import release_firmware as bundles
    from push_firmware import request
    from firmware_progress import BuildProgress, ProgressDisplay
    import firmware_changes as changes

ROOT = Path(__file__).resolve().parents[1]
TYPES = {"sensor", "screen", "bridge", "arbiter", "traveler", "station"}


def node_type(env, role=""):
    if env == "component_state_display" or env.startswith("screen"):
        return "screen"
    if env.startswith("bridge"):
        return "bridge"
    if env.startswith("arbiter"):
        return "arbiter"
    if env in {"sensor", "traveler"}:
        return env
    if env.startswith("station"):
        return "station"
    return {"component_state_display": "screen"}.get(role.lower(), role.lower() or "unknown")


def values(raw):
    result = []
    for item in raw or []:
        pieces = [piece.strip() for piece in item.split(",")]
        if not all(pieces):
            raise ValueError("comma-separated filters must not contain empty values")
        result.extend(pieces)
    return list(dict.fromkeys(result))


def filters(args):
    ids = []
    for value in values(args.id):
        match = re.fullmatch(r"(?:[A-Za-z]+-)?(?:0x)?([0-9A-Fa-f]{4})", value, re.IGNORECASE)
        if not match:
            raise ValueError(f"invalid device ID: {value}")
        ids.append(match[1].upper())
    types = [value.lower() for value in values(args.type)]
    if set(types) - TYPES:
        raise ValueError(f"unknown node type: {', '.join(sorted(set(types) - TYPES))}")
    envs = values(args.env)
    known = environments()
    if set(envs) - set(known):
        raise ValueError(f"unknown firmware environment: {', '.join(sorted(set(envs) - set(known)))}; see './fw envs'")
    return {"ids": list(dict.fromkeys(ids)), "types": types, "envs": envs}


def environments():
    config = configparser.ConfigParser(interpolation=None)
    config.read(ROOT / "platformio.ini")
    return {name[4:]: config[name] for name in config.sections() if name.startswith("env:")}


def expected_class(env):
    configs = environments()
    seen = set()
    while env in configs and env not in seen:
        seen.add(env)
        board = configs[env].get("board", "")
        if board:
            if "esp32s3" in board:
                return "s3_16mb" if "16mb" in board else "s3_4mb"
            return "c3" if "esp32c3" in board else "esp32"
        parent = configs[env].get("extends", "").split(",")[0].strip()
        env = parent[4:] if parent.startswith("env:") else ""
    return None


def usb_devices(ports=None):
    try:
        from serial.tools import list_ports
        if __package__:
            from . import identify
        else:
            import identify
    except ImportError as exc:
        raise RuntimeError("USB commands need pyserial; use your PlatformIO Python environment or install pyserial") from exc
    if not ports:
        prefixes = ("/dev/cu.usb", "/dev/cu.wchusb", "/dev/ttyACM", "/dev/ttyUSB")
        ports = sorted(port.device for port in list_ports.comports() if port.device.startswith(prefixes))
    rows = []
    threads = [threading.Thread(target=identify.probe, args=(port, rows), kwargs={"versions": True}) for port in ports]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    for row in rows:
        row["id"] = row.get("id", "?").upper()
        row["type"] = node_type(row.get("env", ""), row.get("role", ""))
        row["status"] = "error" if row.get("source") == "error" else "USB"
    return sorted(rows, key=lambda row: row["port"])


def inventory(args, *, provision=False):
    ports = values(getattr(args, "port", []))
    if ports and not args.usb:
        raise ValueError("--port requires --usb")
    if args.usb:
        rows = usb_devices(ports)
        envs = values(args.env)
        # Explicit ports plus one env are also the first-flash/provisioning path.
        if provision and ports and len(envs) == 1:
            for row in rows:
                row["env"] = envs[0]
                row["type"] = node_type(envs[0])
        return rows, local_release_names()
    if getattr(args, "scan", False):
        job = request(args.url, "/scan", {})["job"]
        wait_job(args.url, job, quiet=getattr(args, "json", False))
    snapshot = request(args.url)
    names = {}
    for release in snapshot["releases"]:
        for item in release["manifest"].values():
            names[item["build_hash"]] = release_label(release)
    rows = []
    for node in snapshot["devices"]:
        row = dict(node)
        row["id"] = row["id"].upper()
        row["type"] = node_type(row.get("env", ""), row.get("role", ""))
        work = next((job["devices"][row["id"]] for job in snapshot["jobs"]
                     if job["state"] == "running" and row["id"] in job["devices"]), None)
        row["status"] = work["state"] if work else "seen" if row.get("online") else "not recent"
        rows.append(row)
    return rows, names


def release_label(release):
    """A release's build time in Pacific time; an old manifest with only a tag shows the tag."""
    return bundles.format_built(release["built_at"]) if release.get("built_at") else release["tag"]


def local_release_names():
    result = {}
    for path in sorted((ROOT / "dist").glob("**/manifest.json")):
        try:
            manifest = json.loads(path.read_text())
            for image in manifest["firmware"].values():
                result[image["build_hash"]] = release_label(manifest)
        except (OSError, KeyError, TypeError, ValueError):
            continue
    return result


def select_devices(rows, choice):
    missing = set(choice["ids"]) - {row["id"] for row in rows}
    if missing:
        raise ValueError(f"device IDs not found: {', '.join(sorted(missing))}; no devices flashed")
    return [row for row in rows
            if (not choice["ids"] or row["id"] in choice["ids"])
            and (not choice["types"] or row["type"] in choice["types"])
            and (not choice["envs"] or row.get("env") in choice["envs"])]


def print_devices(rows, names, *, json_output=False):
    rows = [dict(row, version=names.get(row.get("sha256")) or row.get("build") or "unknown") for row in rows]
    if json_output:
        print(json.dumps(rows, indent=2))
        return
    if not rows:
        print("No devices match.")
        return
    fields = [("ID", "id"), ("TYPE", "type"), ("ENV", "env"), ("VERSION / BUILD", "version"),
              ("BUILD HASH", "short_hash"), ("STATE", "status"), ("SEEN / PORT", "location")]
    for row in rows:
        digest = row.get("sha256", "unknown")
        row["short_hash"] = digest[:12] if len(digest) == 64 else "unknown"
        row["location"] = row.get("port") or (f"{max(0, int(time.time() - row['last_seen']))}s ago" if row.get("last_seen") else "unknown")
    widths = [max(len(title), *(len(str(row.get(key, "?"))) for row in rows)) for title, key in fields]
    print("  ".join(title.ljust(width) for (title, _), width in zip(fields, widths)))
    for row in rows:
        print("  ".join(str(row.get(key, "?")).ljust(width) for (_, key), width in zip(fields, widths)))
    print(f"\n{len(rows)} device(s).")


def build_development(envs, verbose=False):
    # One timestamp for the whole batch; a new identity per env every time.
    built_at, identities = bundles.new_batch(envs)
    output = ROOT / "dist" / str(built_at)
    if verbose:
        return bundles.build_bundle(envs, built_at, identities, output)
    # A line per firmware type, ticked off as each compiles; the compiler's own
    # output is shown only for a build that fails.
    with BuildProgress() as progress:
        return bundles.build_bundle(envs, built_at, identities, output, progress=progress)


def choose_by_changes(args, choice):
    """`fw flash` with nothing selected builds only the firmware the working tree's
    changes touch (tools/firmware_changes.py has the table). Always says which way
    it chose. Returns (the choice to use, the Plan or None if you chose); the choice
    is None if there is nothing to build."""
    given = [name for name, value in (("--id", choice["ids"]), ("--type", choice["types"]), ("--env", choice["envs"]),
                                      ("--port", values(args.port)), ("--manifest", args.manifest), ("--all", args.all))
             if value]
    if given:
        print(f"Firmware chosen by you ({', '.join(given)}); git changes were not consulted.", flush=True)
        return choice, None
    source = f"git diff {args.since or 'HEAD'} + untracked files"
    plan = changes.plan(changes.changed_files(ROOT, args.since), bundles.ENVS)
    print(plan.report(source), flush=True)
    if not plan.envs:
        return None, plan
    return {**choice, "envs": plan.envs}, plan


def selected_build_envs(args, choice):
    if choice["ids"] or values(args.port):
        rows, _ = inventory(args, provision=True)
        selected = select_devices(rows, choice)
        if not selected:
            raise ValueError("no devices match; nothing built")
        envs = list(dict.fromkeys(row.get("env") for row in selected))
    else:
        envs = choice["envs"] or list(bundles.ENVS)
        if choice["types"]:
            envs = [env for env in envs if node_type(env) in choice["types"]]
    if not envs or any(env not in environments() for env in envs):
        raise ValueError("no known firmware environments selected; use --env explicitly for a blank USB board")
    return envs


POLL_SECONDS = 2
POLL_FAILURES = 5   # in a row, before the watch gives up on the Overlord


def job_activity(logs, job_id):
    """What the Overlord has logged about one job, as (sequence, text) pairs.
    The "Served 64 KB of ..." lines are the bars already, so they are left out."""
    rows = []
    for row in logs:
        message = str(row.get("message", ""))
        if row.get("job_id") != job_id or message.startswith("Served "):
            continue
        when = time.strftime("%H:%M:%S", time.localtime(row.get("at", 0)))
        node = f"{row['node_id']}: " if row.get("node_id") else ""
        rows.append((row.get("sequence"), f"{when} {node}{message}"))
    return rows


def wait_job(url, job, *, quiet=False, logs=()):
    """Watch an Overlord job until it ends, drawing each device's progress and
    what the Overlord says about it. A poll that fails is retried: the job runs
    on the Overlord whether or not this is watching."""
    if not quiet:
        print(f"Watching job {job['id']} on {url.rstrip('/')}/  (Ctrl-C detaches; ./fw watch reattaches)", flush=True)
    activity, misses = job_activity(logs, job['id']), 0
    with ProgressDisplay(enabled=not quiet) as display:
        while True:
            devices = {node: {"state": "queued"} for node in job.get("device_ids") or []}
            devices.update(job["devices"])
            done = sum(item.get("state") in {"confirmed", "skipped"} for item in devices.values())
            failed = sum(item.get("state") == "failed" for item in devices.values())
            title = f"Job {job['id']} | {job['state']}"
            if devices:
                title += f" | {done}/{len(devices)} complete | {failed} failed"
            note = "Bar = bytes served; reboot verification is a separate stage." if devices else "Waiting for device discovery or job results."
            if misses:
                note = f"Overlord not answering ({misses}/{POLL_FAILURES}); retrying. The job keeps running there."
            display.draw(title, devices, job.get("error") or note, activity)
            if job["state"] in {"completed", "failed", "interrupted"}:
                if job["state"] != "completed":
                    raise RuntimeError(job.get("error", job["state"]))
                return job
            time.sleep(POLL_SECONDS)
            try:
                state = request(url)
            except (OSError, ValueError, RuntimeError) as exc:
                misses += 1
                if misses >= POLL_FAILURES:
                    display.draw(title, devices, f"Status unavailable: {exc}; the OTA job may still be running.", activity)
                    raise
                continue
            misses = 0
            job = next((row for row in state.get("jobs", []) if row["id"] == job["id"]), job)
            activity = job_activity(state.get("logs", []), job["id"])


def watch(url, job_id=None):
    """Attach to a job already on the Overlord: the one named, else the one
    running, else the most recent."""
    state = request(url)
    jobs = state.get("jobs", [])
    if job_id:
        job = next((row for row in jobs if row["id"] == job_id), None)
        if not job:
            raise ValueError(f"no job {job_id} on the Overlord")
    else:
        job = next((row for row in jobs if row["id"] == state.get("active_job")), jobs[0] if jobs else None)
        if not job:
            raise ValueError("the Overlord has no jobs to watch")
    return wait_job(url, job, logs=state.get("logs", []))


def upload_and_queue(url, path, manifest, rows, *, wait=True):
    needed = {row["env"] for row in rows}
    files = {}
    for env in needed:
        item = manifest["firmware"].get(env)
        if not item or Path(item["asset"]).name != item["asset"]:
            raise ValueError(f"manifest has no valid image for {env}")
        data = (path.parent / item["asset"]).read_bytes()
        if (len(data) != item["size"] or hashlib.sha256(data).hexdigest() != item["sha256"] or
                hashlib.md5(data, usedforsecurity=False).hexdigest() != item["md5"]):
            raise ValueError(f"{env}: binary does not match manifest")
        files[env] = data
    release_id = request(url, "/releases", manifest)["release"]["id"]
    uploads = {env: {"state": "queued", "bytes": 0, "total": len(data)} for env, data in sorted(files.items())}
    with ProgressDisplay() as display:
        for env, data in sorted(files.items()):
            item = uploads[env]
            item["state"] = "uploading"
            def progress(sent, total):
                item.update(bytes=sent, total=total, state="validating" if sent >= total else "uploading")
                note = "100% sent; waiting for Overlord to validate the image." if sent >= total else "Percent = image bytes sent to Overlord."
                display.draw("Uploading images to Overlord", uploads, note)
            display.draw("Uploading images to Overlord", uploads, "Sending firmware images to Overlord.")
            try:
                request(url, f"/releases/{release_id}/{env}", data, binary=True, progress=progress)
            except (OSError, ValueError, RuntimeError) as exc:
                item.update(state="failed", error=str(exc))
                display.draw("Image upload failed", uploads)
                raise
            item.update(state="uploaded", bytes=len(data))
            display.draw("Uploading images to Overlord", uploads, "Uploaded images are validated before a device update is queued.")
    # Freeze resolved IDs, not a broad environment selector that could include
    # another board appearing while the build was running.
    ids = list(dict.fromkeys(row["id"] for row in rows))
    job = request(url, "/updates", {"release_id": release_id, "device_ids": ids})["job"]
    if wait:
        return wait_job(url, job)
    print(f"Queued job {job['id']} · {url.rstrip('/')}/")
    return job


def flash_usb(rows, manifest):
    executable = bundles.pio_path()
    for row in rows:
        env, port = row["env"], row["port"]
        child_env = os.environ.copy()
        child_env["PULLEYS_FIRMWARE_BUILD_HASH"] = manifest["firmware"][env]["build_hash"]
        print(f"Flashing {row.get('id', '?')} ({env}) on {port}…", flush=True)
        # The build already ran once per env. Preserve its exact image while
        # uploading it to each matching board, with PlatformIO's chip checks.
        subprocess.run([executable, "run", "-e", env, "-t", "nobuild", "-t", "upload", "--upload-port", port],
                       cwd=ROOT, env=child_env, check=True)
        verified = usb_devices([port])
        expected = manifest["firmware"][env]["build_hash"]
        if not any(item.get("sha256") == expected and item.get("env") == env and
                   (row.get("id") in (None, "?") or item.get("id") == row["id"]) for item in verified):
            raise RuntimeError(f"flash written, but reboot identity not confirmed on {port}; check './fw list --usb'")
        print(f"Confirmed {row.get('id', '?')}: {expected[:12]}", flush=True)


def parser():
    result = argparse.ArgumentParser(
        description="Build, inspect and flash Pulleys firmware. OTA through Overlord is the default.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Update only one type of node:
  ./fw flash --type arbiter       Build and update just the arbiter node(s)
  ./fw flash --type sensor        Build and update just the sensors
  ./fw flash --type screen        Update screens, including their display variants
  ./fw flash --type bridge        Update bridges, including both chip variants

Inspect or build before updating:
  ./fw list --type arbiter --scan       Refresh inventory and list arbiters
  ./fw flash --type arbiter --dry-run   Show targets without building or flashing
  ./fw build --type arbiter             Build arbiter firmware without flashing

Other ways to select nodes:
  ./fw flash --id 6C37                  Update one specific device
  ./fw flash --env arbiter_mesh         Select an exact firmware environment
  ./fw flash --type sensor,screen       Select multiple node types

Filters work with list, build, and flash. Values within a filter are alternatives;
--type, --env, and --id are intersected when combined. Flash builds only the
selected environments and updates matching known nodes, preserving each node's
firmware profile.

With no filters, flash chooses from your git changes: it maps each changed file to
the firmware it affects (tools/firmware_changes.py), builds only those, and prints
which patterns it matched and why. --all flashes every known node instead, and
--since REF reads the changes since a commit rather than since HEAD.

Add --usb to use attached USB boards. Run ./fw flash --help for all flash options.
Run ./fw envs to see production firmware environments and their node types.

Interactive terminals show updating upload/device bars with percentages on the
right. Redirected output uses plain progress lines. Transfer reaching 100% is
followed by reboot/hash verification before a device is confirmed.""")
    def transport_options(target, *, inherited=False):
        target.add_argument("--url", default=argparse.SUPPRESS if inherited else os.environ.get("OVERLORD_URL", "http://overlord.local:8080"), help="Overlord URL (or OVERLORD_URL)")
        target.add_argument("--usb", action="store_true", default=argparse.SUPPRESS if inherited else False, help="use attached USB boards instead of Overlord OTA")
    transport_options(result)
    commands = result.add_subparsers(dest="command")
    commands.add_parser("help", help="show usage and node-type filtering examples")
    for name, help_text in (("list", "list visible devices and their firmware versions"),
                            ("build", "build selected firmware without flashing; uncommitted changes allowed"),
                            ("flash", "build once per selected env, then flash matching devices")):
        sub = commands.add_parser(name, help=help_text)
        transport_options(sub, inherited=True)
        sub.add_argument("--id", "-i", action="append", default=[], help="device ID(s), comma-separated; repeatable")
        sub.add_argument("--type", "-t", action="append", default=[], help="node role(s): sensor, screen, bridge, arbiter; comma-separated")
        sub.add_argument("--env", "-e", action="append", default=[], help="exact PlatformIO env(s), comma-separated; repeatable")
        sub.add_argument("--port", "-p", action="append", default=[], help="USB port(s), comma-separated; requires --usb")
        if name == "list":
            sub.add_argument("--scan", action="store_true", help="queue and wait for a fresh Overlord census before listing")
            sub.add_argument("--json", action="store_true", help="machine-readable device list")
        else:
            sub.add_argument("--dry-run", action="store_true", help="show selection without building, uploading or flashing")
            sub.add_argument("--verbose", "-v", action="store_true", help="show the compiler's output instead of a status line per firmware type")
        if name == "flash":
            sub.add_argument("--all", action="store_true", help="every known node, ignoring git changes (the default is only the firmware your changes touch)")
            sub.add_argument("--since", metavar="REF", help="choose firmware from changes since REF (e.g. HEAD~3, origin/main) instead of since HEAD")
            sub.add_argument("--no-wait", action="store_true", help="return after queueing an OTA job (by default fw stays and watches the Overlord until it finishes)")
            sub.add_argument("--manifest", type=Path, help="OTA: upload an existing bundle instead of rebuilding")
    watching = commands.add_parser("watch", help="watch a firmware job on the Overlord: the running one, the latest, or JOB_ID")
    transport_options(watching, inherited=True)
    watching.add_argument("job", nargs="?", help="job ID (default: the running job, else the latest)")
    envs = commands.add_parser("envs", help="list production firmware environments and node types")
    envs.add_argument("--json", action="store_true")
    return result


def main(argv=None):
    cli = parser()
    args = cli.parse_args(argv)
    if not args.command or args.command == "help":
        cli.print_help()
        return 0
    try:
        if args.command == "envs":
            rows = [{"env": env, "type": node_type(env)} for env in bundles.ENVS]
            if args.json:
                print(json.dumps(rows, indent=2))
            else:
                for row in rows:
                    print(f"{row['type']:<8} {row['env']}")
            return 0
        if args.command == "watch":
            watch(args.url, args.job)
            return 0
        choice = filters(args)
        by_changes = None
        if args.command == "flash":
            choice, by_changes = choose_by_changes(args, choice)
            if choice is None:
                print("Nothing to build or flash. Use --all to flash every known node anyway.")
                return 0
        if values(args.port) and not args.usb:
            raise ValueError("--port requires --usb")
        if args.command == "build":
            envs = selected_build_envs(args, choice)
            print("Build environments: " + ", ".join(envs), flush=True)
            if not args.dry_run:
                build_development(envs, verbose=args.verbose)
            return 0
        rows, names = inventory(args, provision=args.command == "flash")
        rows = select_devices(rows, choice)
        if args.command == "list":
            print_devices(rows, names, json_output=args.json)
            return 0
        if not rows:
            if by_changes:
                raise ValueError("no known device runs the firmware your changes touch (" + ", ".join(by_changes.envs) +
                                 "); nothing built or flashed. --all flashes every known node.")
            raise ValueError("no devices match; nothing built or flashed")
        envs = list(dict.fromkeys(row.get("env") for row in rows))
        if any(env not in environments() for env in envs):
            raise ValueError("cannot identify firmware; provision blank USB boards with --usb --port PORT --env ENV")
        if args.usb:
            if args.manifest or args.no_wait:
                raise ValueError("--manifest and --no-wait are OTA options")
            for row in rows:
                if row.get("source") == "error":
                    raise ValueError(f"cannot open {row['port']}: {row.get('error', 'serial error')}")
                wanted = expected_class(row["env"])
                if row.get("cls") not in (None, "?") and wanted and row["cls"] != wanted:
                    raise ValueError(f"{row['port']}: {row['env']} needs {wanted}, board reports {row['cls']}")
        elif set(envs) - set(bundles.ENVS):
            raise ValueError("Overlord OTA only supports production environments; use --usb for other profiles")
        print_devices(rows, names)
        print("Build environments: " + ", ".join(envs), flush=True)
        if args.dry_run:
            print("Dry run: no builds, uploads or flash writes.")
            return 0
        if args.manifest:
            path = args.manifest.resolve()
            print(f"Using existing bundle {path}; no rebuild.", flush=True)
            manifest = json.loads(path.read_text())
            if manifest.get("schema") != 1 or not isinstance(manifest.get("firmware"), dict):
                raise ValueError("manifest must use release schema 1")
        else:
            path, manifest = build_development(envs, verbose=args.verbose)
        if args.usb:
            flash_usb(rows, manifest)
        else:
            upload_and_queue(args.url, path, manifest, rows, wait=not args.no_wait)
        return 0
    except KeyboardInterrupt:
        print("Detached; any queued OTA job continues on the Overlord. Reattach with ./fw watch.", file=sys.stderr)
        return 130
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"fw: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
