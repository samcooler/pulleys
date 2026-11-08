#!/usr/bin/env python3
"""Upload release-tool assets and selectively update devices through Overlord HTTP.

Example:
  python3 tools/push_firmware.py --url http://overlord.local:8080 \
    --manifest dist/v0.2.0/manifest.json --device 59A4 --wait
"""
import argparse
import json
from pathlib import Path
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


class UploadBody:
    def __init__(self, data, progress):
        self.data = data
        self.total = len(data)
        self.progress = progress

    def __iter__(self):
        for offset in range(0, self.total, 64 * 1024):
            end = min(offset + 64 * 1024, self.total)
            yield self.data[offset:end]
            # http.client resumes the iterator after sending the previous
            # chunk. Reading the bytes alone must not advance sent progress.
            self.progress(end, self.total)


def request(base, path="", body=None, binary=False, progress=None):
    payload = body if binary else json.dumps(body).encode() if body is not None else None
    method = "PUT" if binary else "POST" if payload is not None else "GET"
    headers = {"Content-Type": "application/octet-stream" if binary else "application/json"}
    if binary and progress is not None:
        headers["Content-Length"] = str(len(payload))
        payload = UploadBody(payload, progress)
    req = urllib.request.Request(base.rstrip("/") + "/api/firmware" + path,
                                 data=payload, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=60) as response:
            return json.load(response)
    except urllib.error.HTTPError as exc:
        try:
            message = json.load(exc).get("error", str(exc))
        except (ValueError, AttributeError):
            message = str(exc)
        raise RuntimeError(message) from exc


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", default="http://overlord.local:8080")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--assets", type=Path, help="binary directory (default: beside the manifest)")
    parser.add_argument("--device", action="append", help="four-digit device ID; repeat to select several")
    parser.add_argument("--env", help="select a firmware environment, optionally narrowed by --device")
    parser.add_argument("--all", action="store_true", help="update all outdated devices covered by this manifest")
    parser.add_argument("--upload-only", action="store_true")
    parser.add_argument("--wait", action="store_true", help="wait for the queued job and return nonzero on failure")
    args = parser.parse_args()
    if not args.upload_only and not (args.device or args.env or args.all):
        parser.error("select --device, --env, --all, or --upload-only")
    if args.all and args.device:
        parser.error("choose --all or --device")
    manifest = json.loads(args.manifest.read_text())
    environments = set(manifest["firmware"])
    if args.env:
        if args.env not in environments:
            parser.error("--env is not in the manifest")
        environments = {args.env}
    if args.device:
        inventory = {node["id"]: node for node in request(args.url)["devices"]}
        ids = [value.upper() for value in args.device]
        if any(value not in inventory for value in ids):
            parser.error("a device is not known to the overlord; scan the mesh first")
        wanted = {inventory[value]["env"] for value in ids}
        if not wanted <= environments:
            parser.error("selected devices do not match the manifest or --env")
        environments = wanted
    assets = args.assets or args.manifest.parent
    # Verify every selected file exists before registering anything remotely.
    files = {}
    for env in environments:
        name = manifest["firmware"][env]["asset"]
        if Path(name).name != name:
            parser.error("manifest asset must be a filename")
        files[env] = (assets / name).read_bytes()
    release = request(args.url, "/releases", manifest)["release"]
    release_id = release["id"]
    for env, data in sorted(files.items()):
        request(args.url, f"/releases/{urllib.parse.quote(release_id, safe='')}/{env}", data, binary=True)
        print(f"Uploaded {env}: {len(data):,} bytes", flush=True)
    if args.upload_only:
        print(json.dumps({"release_id": release_id}))
        return
    body = {"release_id": release_id}
    if args.device:
        body["device_ids"] = [value.upper() for value in args.device]
    if args.env:
        body["env"] = args.env
    if args.all:
        body["all"] = True
    job = request(args.url, "/updates", body)["job"]
    print(json.dumps({"release_id": release_id, "job_id": job["id"]}), flush=True)
    if args.wait:
        previous = None
        while job["state"] not in {"completed", "failed", "interrupted"}:
            time.sleep(2)
            job = request(args.url, "/jobs/" + job["id"])["job"]
            progress = (job["state"], json.dumps(job["devices"], sort_keys=True))
            if progress != previous:
                print(json.dumps(job), flush=True)
                previous = progress
        if job["state"] != "completed":
            raise RuntimeError(job.get("error", job["state"]))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError) as exc:
        sys.exit(str(exc))
