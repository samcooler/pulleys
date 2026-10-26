# Firmware Update System Plan

## Goal

Replace the current USB-seeded, ESP32-hosted OTA flow with a system where the
Overlord fetches the latest firmware release from GitHub, the bridge exposes a
channel-matched Wi-Fi access point and REST API, and mesh nodes pull and install
their own firmware. The Overlord owns release selection, update policy, ordering,
and retry decisions. The bridge transports requests and image bytes and reports
status; nodes report their identity and running firmware hash, then perform the
installation.

## Agreed behavior

- The Overlord starts its normal service without waiting for update checks.
- At each boot it makes up to three attempts to reach GitHub. If none succeeds,
  it gives up until the next Overlord reboot. It does not block normal service.
- When online, it always uses the latest GitHub Release.
- Each release build has a deterministic SHA-256 build identity derived from
  release tag, commit, and PlatformIO environment. The release manifest also
  carries SHA-256 of the exact binary asset for transfer integrity. A node is
  current only when its reported build identity equals the manifest identity.
- A release script builds the active PlatformIO environments, creates a new
  release automatically, and increments the minor version from the latest
  release (for example, `v1.2.3` becomes `v1.3.0`). Only active environments
  are published.
- Nodes that are reachable and out of date are updated without user interaction.
  The order is otherwise arbitrary, with `arbiter_mesh` first and `bridge` last.
  Only one firmware update may be in progress at a time, including for multiple
  devices of the same type.
- A node update is successful only after the node reports successful
  installation, reboots, and is queried again to confirm the expected SHA-256.
  Each target gets at most three attempts; after that, the Overlord continues to
  the next target.
- Nodes reboot as soon as installation finishes. The bridge is updated last and
  will reboot, temporarily disconnecting the Overlord.
- The bridge has a separate self-update instruction over serial. The Overlord
  streams its image to the bridge; the bridge reports progress/result and
  installs to its inactive application slot before rebooting. Its existing
  `min_spiffs.csv` layout already provides two app slots, so keep it.
- The bridge AP is open and stays up continuously on the ESP-NOW mesh channel.
- Retire the old ESP32 image-host workflow and `tools/ota.py`. Keep a small
  bridge compatibility path for the deployed legacy node client during the
  first migration; the new bridge itself must be flashed over USB once because
  old bridge firmware cannot receive the new UART protocol. There is no planned
  manual USB updater after that bootstrap or additional recovery feature.

## Components and responsibilities

| Component | Responsibility |
| --- | --- |
| Release script | Determine the next minor release tag; build active environments with PlatformIO; create/publish GitHub Release assets and a manifest containing binary SHA-256, build identity, and legacy migration MD5. |
| Overlord | Start normal service; make bounded GitHub attempts; fetch and validate the latest release manifest; query bridge/nodes; compare reported hashes; schedule one update at a time; retry failures; confirm the post-reboot hash. |
| Bridge firmware | Maintain mesh participation and a continuously available AP on `MESH_WIFI_CHANNEL`; host the firmware REST endpoint; relay serial chunk requests and ESP-NOW offers/status; serialize transfers; handle its own serial-delivered update. |
| Mesh node firmware | Report device type, identity, and running firmware hash; accept a targeted update offer; GET the selected image from the bridge; validate size/hash; install to its inactive slot, reboot, and report the result. |

## Proposed update flow

1. The Overlord starts its existing application and serial reader as usual, then
   makes up to three bounded connection attempts for GitHub. If GitHub cannot be
   reached, it records/skips this boot's update check. If there is no bridge
   connection during the configured bridge-discovery window, it skips this
   boot's run rather than polling indefinitely.
2. The Overlord obtains the latest GitHub Release and downloads its manifest.
   It verifies that required active-environment entries exist and checks each
   downloaded asset against its declared length and SHA-256 before making it
   available to the bridge. Cache the release assets locally so a retry does
   not require another network download.
3. The Overlord asks the bridge to begin an inventory round. The bridge requests
   current firmware information from the mesh; nodes reply with stable node ID,
   environment/type, and current firmware SHA-256. The implementation should
   use the existing census facilities where possible and add OTA-specific
   version reporting/messages only where needed.
4. The Overlord compares inventory to the release manifest. Nodes with matching
   hashes are skipped. Reachable out-of-date nodes enter the queue. Process
   `arbiter_mesh` first, then the remaining node types/devices in arbitrary
   order, with the bridge's own update last. Do not allow overlapping
   downloads/updates.
5. For each mesh node, the Overlord sends a targeted update instruction through
   the bridge. The bridge announces the selected target/image over ESP-NOW. The
   target confirms readiness and performs an HTTP GET to a bridge endpoint for
   its selected image. The bridge requests bounded chunks from the Overlord
   over serial and streams them to the HTTP client with backpressure. The node
   validates the complete image, flashes its inactive slot, sends a success or
   failure status, and reboots after success.
6. The Overlord waits for the transfer/install result, then waits for the node to
   reappear in mesh inventory and requests its running hash again. A successful
   status without a matching post-reboot hash is not success. Retry that node
   up to three total attempts, then record failure and continue.
7. After all other reachable nodes have been handled, the Overlord sends the
   bridge its dedicated self-update instruction with the bridge release asset.
   The Overlord sends the image in serial chunks; the bridge validates it,
   writes its inactive app partition, reports outcome, and reboots. After the
   bridge reconnects, the Overlord queries its identity and firmware hash and
   requires the expected hash. Retry up to three attempts if the bridge remains
   responsive and is not already confirmed; if it cannot reconnect, the run
   cannot safely attempt further bridge updates and should report failure.

## Protocol design to implement

### Overlord ↔ bridge serial link

The existing UART1 link carries cue/event lines toward the Overlord. Add
line-oriented control records while preserving those events. Firmware bytes are
not sent raw: the bridge requests Base64 chunks from the Overlord using
`FW-READ sha256=<asset> offset=<n> length=384`; the Overlord replies with a
matching `FW-DATA` record. Asset SHA plus byte offset correlates each
request/response. A missing or invalid response truncates the HTTP transfer,
which fails the node install and is retried by the coordinator. The bridge
continues mesh polling between chunk requests.

### Bridge REST API

The bridge exposes a small API:

- `GET /api/v1/health` — bridge readiness, mesh channel, active transfer state.
- `GET /api/v1/firmware/{asset-sha256}` — target node requests the active image;
  bridge streams it from the Overlord over serial with a content length.
- `GET /firmware.bin` — migration-only route for deployed legacy nodes; serves
  the selected image after the legacy ESP-NOW invitation.

`GET /api/v1/health` reports AP channel and transfer state. The Overlord selects
the image and target over UART; nodes pull the image over HTTP. Only the active
asset SHA is served. The bridge AP has no Internet routing.

### ESP-NOW update messages

Reuse HELLO/CENSUS for inventory and add targeted FW_OFFER plus FW_STATUS
messages. An offer identifies target ID, environment, release build hash, asset
SHA-256, and size. New nodes verify size and SHA-256 before install. During
migration, the bridge also emits legacy OTA_ANNOUNCE/OTA_READY messages so old
node images join the open AP and pull `/firmware.bin`.

### Firmware identity and integrity

Each release image embeds a deterministic SHA-256 build identity derived from
release tag, commit, and PlatformIO environment. Nodes report that identity; the
manifest separately stores SHA-256 of the exact binary bytes. The Overlord
validates downloaded assets and new nodes validate the full binary before
installing. Signatures are out of scope.

## Release automation

Add a script that:

1. Reads GitHub Releases/tags and computes the next minor version. Establish the
   initial version when no prior release exists (proposed `v0.1.0`).
2. Builds the active environments in a deliberate order, including
   `arbiter_mesh` and `bridge`; active node environments are currently
   `sensor`, `screen`, `component_state_display`, `arbiter_mesh`, and `bridge`.
3. Collects each `.pio/build/<env>/firmware.bin`, computes size, exact binary
   SHA-256, migration MD5, and release build identity, then emits the manifest.
4. Creates the Git tag and GitHub Release, then uploads each firmware binary and
   manifest. Fail without publishing a partial release if a build or required
   asset step fails; make reruns against an existing tag safe and explicit.
5. Documents required GitHub CLI/authentication and a local dry-run/build-only
   mode. Publishing is an explicit operator command, not an implicit PlatformIO
   build side effect.

Use GitHub's API/CLI as the release mechanism; no new Overlord credential should
be required to download public releases. If the repository is private, document
the read credential mechanism without embedding secrets in firmware or source.

## Firmware and partition changes

- Embed a deterministic SHA-256 build identity in each release image and report
  it in census. Keep exact binary SHA-256 in the manifest; include MD5 only as
  an idempotence marker for the legacy node client.
- Integrate the new update state machine with mesh poll loops without blocking
  event handling or time-sensitive display/sensor work.
- Confirm each node's partition table has two suitable app slots and that the
  selected update API validates the image before boot selection.
- The bridge currently uses `min_spiffs.csv`, which provides dual 1.875 MiB app
  slots on the C3. Keep this layout and verify the built bridge image fits
  its inactive slot before publishing.
- Implement bridge self-flash using the inactive slot; send explicit progress,
  completion, and error status over serial. Reboot only after a complete,
  validated image is written.
- Remove the old host/node image-serving implementation and retire
  `tools/ota.py`; retain only the legacy announce/status and `/firmware.bin`
  compatibility path needed to migrate deployed nodes. Update documentation.

## Startup discovery and retry policy

- Keep the Overlord responsive before and during release/network attempts.
- Use three attempts for GitHub access on boot, with bounded timeouts and
  backoff; then stop until next reboot.
- Define a bounded bridge connection/discovery window. Once the bridge is found,
  collect inventory for a bounded census window and update the nodes that
  reported during it. Do not run indefinite polling. A later boot is the retry
  opportunity for nodes absent from that inventory.
- Each selected target receives at most three update attempts. Between attempts,
  re-query its status/hash so a completed update is not repeated unnecessarily.
- Persist or log a concise per-run outcome: release tag, expected hash, each
  target's attempts/result, and any nodes not seen. Avoid persisting a state that
  prevents a new boot from re-evaluating the latest release.

## Implementation phases

1. **Protocol and firmware identity:** define release manifest and build hash
   semantics; add per-node version query/report and OTA ESP-NOW message types.
2. **Serial transport:** extend bridge/Overlord communication with correlated
   bidirectional control and bounded binary chunks while keeping cue/event flow
   intact.
3. **Bridge AP/API:** start the channel-matched open AP continuously; implement
   inventory, update control, status, and streamed firmware endpoints; enforce
   single-transfer ownership.
4. **Node update path:** add targeted offer handling, HTTP pull, size/SHA-256
   verification, inactive-slot install, success/failure reporting, and reboot.
5. **Overlord coordinator:** add GitHub latest-release fetch/cache/validation,
   boot-only retry policy, inventory comparison, priority/order, one-at-a-time
   execution, three attempts, and post-reboot hash confirmation.
6. **Bridge self-update:** retain its existing dual-app partition layout,
   implement serial image intake and inactive-slot install, report status,
   reboot, and reconnect/hash verify.
7. **Release script and docs:** build active environments, calculate minor tag,
   package assets/manifest, publish GitHub Release, and document operation.
8. **Retire old updater:** remove `tools/ota.py` and old ESP32 image hosting;
   retain the narrow legacy-node migration protocol until deployed nodes have
   moved to the new firmware.

## Completion criteria

- Overlord service starts normally even without Internet or a bridge.
- GitHub latest release and each firmware asset are validated by SHA-256.
- Nodes accurately report their running image hash and can be compared with the
  manifest.
- The bridge AP remains on the ESP-NOW channel and its REST API serves a node
  image streamed from Overlord over serial without corrupting event/cue traffic.
- Only one target updates at a time; `arbiter_mesh` is first and bridge is last.
- A node is marked successful only after install success plus post-reboot hash
  confirmation; failed targets get no more than three attempts.
- Bridge self-update uses an inactive app slot, returns status, reboots, and is
  confirmed by a fresh identity/hash query.
- Release script produces a minor-incremented GitHub Release with all active
  firmware binaries and a matching manifest.
- Old image-host tooling is removed, deployed legacy nodes can migrate through
  the compatibility path, and the updated docs describe the new system.

## Implementation notes and validation still needed

- The current startup policy uses three GitHub attempts with 3- and 6-second
  backoffs, a 25-second bridge discovery window, and a 4.5-second census wait.
- The release script uses `v0.1.0` when there is no prior release; later tags
  increment the minor component and reset patch to zero. Public releases need
  no token; `GITHUB_TOKEN` can be supplied for private releases.
- PlatformIO builds still need to confirm the build-hash pre-script and image
  sizes, including the arbiter's additional pre-script. The bridge's existing
  partition CSV has two application slots; node partition fit should also be
  checked during builds before using this updater in production.
