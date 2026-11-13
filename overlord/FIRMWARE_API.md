# Firmware dashboard and API

Open `http://overlord.local:8080/` from the local network. The dashboard and the
existing media control API share one Python HTTP server. The default bind is
`0.0.0.0`; override with `--control-bind` or `OVERLORD_CONTROL_BIND`.

The Overlord never downloads firmware and does nothing about it at startup:
images are pushed to it (`./fw flash`, `tools/push_firmware.py`, or the endpoints
below). Updates and scans use one worker. A census never runs
in the middle of a transfer. The arbiter runs first and this server's bridge
runs last. A device is confirmed only after a fresh post-reboot identity report.

The dashboard polls every two seconds; the Overlord scans the mesh every 10 s while idle. Expand a device for its build identity
and activity. Select a release before updating devices. Progress
counts bytes served through the bridge, not successful installation; the
separate verification stage checks the reboot. A gray device may simply have
an old census report while another device is updating.

Catalogs, validated binaries, job results and the last 500 activity records live
in `$OVERLORD_FIRMWARE_CACHE` (default `~/.cache/overlord/firmware`). Pending jobs
become `interrupted` after a restart and can be explicitly retried; nothing is retried or started on its own. History retains up to 75 terminal jobs plus
pending work. Changing the selected release does not alter already queued jobs.

## Script workflow

Use the existing release tooling to produce `manifest.json` and its binaries.
For example, after committing firmware source changes:

```sh
python3 tools/release_firmware.py          # prints dist/<timestamp>/
python3 tools/push_firmware.py \
  --url http://overlord.local:8080 \
  --manifest dist/<timestamp>/manifest.json \
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
| POST | `/api/firmware/scan` | Queue two census rounds |
| POST | `/api/firmware/releases` | Register an unchanged schema-1 release manifest |
| PUT | `/api/firmware/releases/{release_id}/{env}` | Upload the raw `.bin` described by that environment |
| GET | `/api/firmware/releases/{release_id}` | Manifest metadata and per-image upload readiness |
| POST | `/api/firmware/updates` | Queue a selected update, returning HTTP 202 and a job |
| GET | `/api/firmware/jobs/{job_id}` | Poll one job's state, device progress and results |
| POST | `/api/firmware/jobs/{job_id}/retry` | Retry failed devices, the unfinished ones of a cancelled job, or an interrupted job's original selection |
| GET | `/api/firmware/queue` | What is running, what waits (in order), and whether the queue is paused |
| POST | `/api/firmware/queue/pause` | Hold the queue (see "Controlling the queue") |
| POST | `/api/firmware/queue/resume` | Release the hold |
| POST | `/api/firmware/queue/clear` | Cancel every waiting job; `{"include_running": true}` also cancels the running one |
| POST | `/api/firmware/jobs/{job_id}/cancel` | Cancel a waiting job, or stop a running one |
| DELETE | `/api/firmware/jobs/{job_id}` | Cancel a waiting or running job; drop a finished one from the history |
| POST | `/api/firmware/jobs/{job_id}/move` | Reorder a waiting job: `{"position": 0}` runs next; `"top"`, `"bottom"` |
| POST | `/api/firmware/jobs/{job_id}/devices` | Edit a job's devices: `{"add": ["59A4"], "remove": ["CC6B"]}` |
| DELETE | `/api/firmware/jobs/{job_id}/devices/{id}` | Remove one device from a job |
| GET | `/api/firmware/logs?since=SEQ&job=ID&limit=N` | Activity records newer than a sequence number, to follow a job; reply has `last_sequence` |
| DELETE | `/api/firmware/releases/{release_id}` | Delete a release and the cached images only it used (refused while a waiting or running job needs it) |

Register a manifest and upload an image:

```sh
curl -sS -X POST http://overlord.local:8080/api/firmware/releases \
  -H 'Content-Type: application/json' --data-binary @dist/<timestamp>/manifest.json
# Use the returned release.id:
curl -sS -X PUT http://overlord.local:8080/api/firmware/releases/RELEASE_ID/sensor \
  -H 'Content-Type: application/octet-stream' \
  --data-binary @dist/<timestamp>/sensor-firmware.bin
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
`built_at` (Unix seconds the batch was triggered; optional, shown on the dashboard in Pacific time),
`firmware`, `asset`, `size`, `sha256`, `md5`, and `build_hash` fields.

Job states: `queued`, `running`, `completed`, `failed`, `interrupted`, `cancelled`.
Device states: `offering`, `connecting`, `transferring`, `verifying`, `confirmed`,
`skipped`, `failed`, `cancelled`. Already-current devices are skipped without flashing.

## Controlling the queue

Jobs run one at a time, in the order `GET /api/firmware/queue` lists them. The
snapshot (`GET /api/firmware`) also carries `paused` and `queue` (the waiting job IDs).

- **Pause** holds the queue. The device being flashed **finishes** (a flash is never
  cut off halfway by a pause), then nothing new starts, neither the running job's
  next device nor the next job. Held devices show `queue paused; waiting to resume`.
  `resume` continues exactly where it stopped. The hold survives an Overlord
  restart, so a paused queue stays paused until someone resumes it. Scans and
  passive discovery are unaffected.
- **Cancel** a waiting job and it is dropped without running. Cancel a running one
  and the Overlord stops serving the device's download (the bridge is answered with
  an error), takes no more devices, and the job ends `cancelled`; devices that had
  not started are marked `cancelled`. A device that has not finished downloading
  discards the partial image and keeps its current firmware, so this is safe up to
  the moment the install is acknowledged. Cancelling **during the reboot check** can
  not undo an install that already went through; that device is marked `cancelled`
  with a message saying it may have installed the new firmware. A scan also stops.
  `POST .../retry` on a cancelled job queues the devices that did not finish.
- **Move** reorders a waiting job (`position` 0 runs next).
- **Edit devices**: a waiting job can gain or lose devices (a job that targets every
  outdated device takes only removals, recorded as `exclude`; its last device cannot
  be removed, cancel the job instead). A running job can drop devices it has not
  reached yet (they finish `skipped`, "removed from this job"); to add devices to a
  running one, queue a new job. A device already being flashed or finished cannot be
  removed from a running job.
- **Delete a release** to free the cache; each `./fw flash` otherwise leaves its
  images there.

```sh
curl -X POST   http://overlord.local:8080/api/firmware/queue/pause
curl -X POST   http://overlord.local:8080/api/firmware/jobs/JOB/cancel
curl -X POST   http://overlord.local:8080/api/firmware/jobs/JOB/move    -d '{"position":"top"}'
curl -X POST   http://overlord.local:8080/api/firmware/jobs/JOB/devices -d '{"remove":["CC6B"]}'
curl           'http://overlord.local:8080/api/firmware/logs?job=JOB&since=120'
```

Unknown job or release IDs return 404; an operation the job's state does not allow
(cancelling a finished job, moving a running one) returns 400 with the reason.

The existing `/status`, `/volume`, `/cue`, `/param`, and `/playtest` endpoints
retain their existing behavior.

## Fast bridge transport

Updated bridges boot at 921600 baud and request up to 4096 bytes per chunk.
Overlord probes fast and legacy rates before exposing a connected link, and
re-probes after its own bridge's successful self-update ACK. Upgrade Overlord
first; old bridges continue using 115200 baud and 384-byte requests during
migration. Native USB CDC does not have the UART rate limit.

**Throughput.** At 921600 baud one 4 KB chunk is about 5.5 KB on the wire, so the serial
link alone allows about 68 KB/s. Measured on a classic-ESP32 node over WiFi, 1 MB took
42 s (23 KB/s) before the fixes below and 21 s (47 KB/s) after. Two changes got there:

- The bridge reads the serial link in blocks. It used to read one byte at a time, and
  each `available()` + `read()` takes a lock in the serial driver (about 27 us a byte),
  so reading a chunk took 153 ms where the wire needs 61 ms.
- A bridge asks the Overlord for chunk N+1 *before* it hands chunk N on (over WiFi to a
  node), so the serial transfer overlaps the hand-off. That was worth about 27% on its
  own (18 -> 23 KB/s) while the byte-wise read was still the limit.

Per chunk the bridge now spends about 13 ms from request to first byte, 62 ms receiving
the line, and 10 ms parsing it; the WiFi write to the node is about 12 ms. After each
transfer the bridge prints `[FW] timing:` and `[FW] per chunk:` lines (they appear in
the Overlord's journal) so a regression shows up as numbers, not a feeling.

Nothing changes for the Overlord: it answers each `FW-READ` by offset, as before. The
bridge reports `pipe=1` in its `FW-BRIDGE` line (`pipe=0` for a build made with
`-D BRIDGE_PIPELINE=0`, which goes back to one chunk at a time). If the answer to a
prefetched chunk is lost or damaged the bridge simply asks again, so pipelining can only
fall back to the old behaviour. A bridge's own flash update is never pipelined, because
the serial interrupt cannot run during a flash write. A bridge also stops waiting at
once when the Overlord answers `FW-ERROR` for the chunk it asked for (a cancelled job),
instead of timing out three times.

Each `FW-DATA` record now includes `crc32=<8 hex digits>` before its final `b64`
field. Old bridges ignore the extra field; new bridges check it and retry a
corrupted chunk up to three times. The 4 KiB binary buffer and ~5.6 KiB line
buffer are static, and the bridge RX ring is 16 KiB, avoiding task-stack overflow
and line truncation. Lengths above 4096 are rejected. Full-image SHA-256, image
size, and post-reboot build identity checks are unchanged. `bridge.baud` reports
the active host baud, and `FW-BRIDGE` identity reports include `baud` and `chunk`.
