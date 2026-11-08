# Firmware dashboard and API

Open `http://overlord.local:8080/` from the local network. The dashboard and the
existing media control API share one Python HTTP server. The default bind is
`0.0.0.0`; override with `--control-bind` or `OVERLORD_CONTROL_BIND`.

Startup still checks GitHub and updates outdated devices automatically. Startup,
manual updates, scans, and release checks use one worker. A census never runs
in the middle of a transfer. The arbiter runs first and this server's bridge
runs last. A device is confirmed only after a fresh post-reboot identity report.

The dashboard polls every two seconds. Expand a device for its build identity
and activity. Select a GitHub or local release before updating devices. Progress
counts bytes served through the bridge, not successful installation; the
separate verification stage checks the reboot. A gray device may simply have
an old census report while another device is updating.

Catalogs, validated binaries, job results and the last 500 activity records live
in `$OVERLORD_FIRMWARE_CACHE` (default `~/.cache/overlord/firmware`). Pending jobs
become `interrupted` after a restart and can be explicitly retried. Startup's
normal update pass still runs. History retains up to 75 terminal jobs plus
pending work. Changing the selected release does not alter already queued jobs.

## Script workflow

Use the existing release tooling to produce `manifest.json` and its binaries.
For example, after committing firmware source changes:

```sh
python3 tools/release_firmware.py --build-only --tag v0.3.0
python3 tools/push_firmware.py \
  --url http://overlord.local:8080 \
  --manifest dist/v0.3.0/manifest.json \
  --device 59A4 --wait
```

Repeat `--device` for selected device IDs; use `--env sensor` for that environment
or `--all` for all outdated devices covered by the manifest. `--env` can narrow
`--device`. `--upload-only` registers and uploads without scheduling an update.
`--assets PATH` overrides the binary directory. `--wait` exits nonzero on a failed
or interrupted job. The script uploads only environments needed by its selection.

## HTTP endpoints

All bodies and responses are JSON except binary PUT uploads. No login is required.

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/firmware` | Devices, last seen, identities, releases, jobs, bridge link, activity |
| POST | `/api/firmware/check` | Queue GitHub release fetch and checksum verification; does not flash |
| POST | `/api/firmware/scan` | Queue two census rounds |
| POST | `/api/firmware/releases` | Register an unchanged schema-1 release manifest |
| PUT | `/api/firmware/releases/{release_id}/{env}` | Upload the raw `.bin` described by that environment |
| GET | `/api/firmware/releases/{release_id}` | Manifest metadata and per-image upload readiness |
| POST | `/api/firmware/updates` | Queue a selected update, returning HTTP 202 and a job |
| GET | `/api/firmware/jobs/{job_id}` | Poll one job's state, device progress and results |
| POST | `/api/firmware/jobs/{job_id}/retry` | Retry failed devices, or the original interrupted selection |

Register a manifest and upload an image:

```sh
curl -sS -X POST http://overlord.local:8080/api/firmware/releases \
  -H 'Content-Type: application/json' --data-binary @dist/v0.3.0/manifest.json
# Use the returned release.id:
curl -sS -X PUT http://overlord.local:8080/api/firmware/releases/RELEASE_ID/sensor \
  -H 'Content-Type: application/octet-stream' \
  --data-binary @dist/v0.3.0/sensor-firmware.bin
```

Queue explicit device IDs:

```json
{"release_id":"RELEASE_ID","device_ids":["EC52","E857"]}
```

Or choose an environment:

```json
{"release_id":"RELEASE_ID","env":"sensor"}
```

Or all outdated devices whose environments occur in the selected manifest:

```json
{"release_id":"RELEASE_ID","all":true}
```

POST these objects to `/api/firmware/updates`. IDs are four hexadecimal digits.
Devices must have been discovered before selection, and must answer the census
at execution time. The update never changes a device's firmware environment.
Local uploads check size, SHA-256, MD5, ESP chip type and the embedded build
identity before becoming usable. Local manifests can contain one or multiple
supported environments, using exactly the release tool's `schema`, `tag`,
`firmware`, `asset`, `size`, `sha256`, `md5`, and `build_hash` fields.

Job states: `queued`, `running`, `completed`, `failed`, `interrupted`.
Device states: `offering`, `connecting`, `transferring`, `verifying`, `confirmed`,
`skipped`, `failed`. Already-current devices are skipped without flashing.

The existing `/status`, `/volume`, `/cue`, `/param`, and `/playtest` endpoints
retain their existing behavior.

## Fast bridge transport

Updated bridges boot at 921600 baud and request up to 4096 bytes per chunk.
Overlord probes fast and legacy rates before exposing a connected link, and
re-probes after its own bridge's successful self-update ACK. Upgrade Overlord
first; old bridges continue using 115200 baud and 384-byte requests during
migration. Native USB CDC does not have the UART rate limit.

Each `FW-DATA` record now includes `crc32=<8 hex digits>` before its final `b64`
field. Old bridges ignore the extra field; new bridges check it and retry a
corrupted chunk up to three times. The 4 KiB binary buffer and ~5.6 KiB line
buffer are static, and the bridge RX ring is 16 KiB, avoiding task-stack overflow
and line truncation. Lengths above 4096 are rejected. Full-image SHA-256, image
size, and post-reboot build identity checks are unchanged. `bridge.baud` reports
the active host baud, and `FW-BRIDGE` identity reports include `baud` and `chunk`.
