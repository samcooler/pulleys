# Arbiter configuration in Overlord

## Goal and scope

Add an Installation page beside Overlord's Firmware page, exposing the active
`arbiter_mesh` touchscreen controls through the existing Overlord → bridge →
mesh connection. Keep the arbiter authoritative and keep touchscreen and browser
edits synchronized. This plan replaces the prior firmware update plan, which
remains available in Git history.

Game parameters, game uploads, and event injection already have serial or HTTP
entry points, but are not arbiter touchscreen controls. Leave those for a
separate follow-up. Do not extend the deprecated BLE arbiter.

## Inventory and required behavior

| Control | Values and actions | Behavior to preserve |
| --- | --- | --- |
| Map Ropes | Sensor IDs assigned to channels 1–24 | First pull starts a 5-second capture window; capture up to 10 ropes; assign the group to one channel; Again clears the group; Done ends mapping. Show reported channel, assigned channel, and confirmation progress. |
| Colours | Six named colours per channel: red, blue, green, yellow, orange, purple | Show rope counts per channel. Persist operator edits on the arbiter and broadcast to sensors/screens. |
| Shuffle game1 colours | Permute the first five colours | Preserve groups sharing a colour and leave purple unchanged. Operator shuffle persists; game-driven shuffle is temporary. |
| Tune RGB | R, G, B in 0–255 for each named colour; reset all to factory RGB | Affect every device wearing that colour. Touchscreen steps by 8; browser may also accept exact values. Persist edits and debounce broadcasts. |
| Set Clock | Year, month, day, hour, minute, second; Save/Cancel | Validate calendar dates and the touchscreen's 2024–2099 year range. Show RTC presence, current time, and clock source. Read back after setting. |

Mapping currently intercepts mesh events before the arbiter game sees them while
its mapping screen is open. Preserve this behavior for remote sessions; channel
0 remains excluded from operator mapping because it is the fault channel.
Unmapping exists on serial but is outside touchscreen parity.

## Architecture and invariants

- Arbiter owns configuration, persistence, mapping capture, and validation.
  Route browser and touchscreen operations through shared controller functions
  and existing `installcfg` setters. Do not maintain a second authoritative
  configuration in Overlord's settings file.
- Extend the existing bridge serial and mesh transport. Local arbiter console
  commands cannot currently be invoked through the bridge. Avoid depending on
  joining the arbiter's separate Wi-Fi AP.
- Use versioned structured messages, targeted arbiter identity, request IDs,
  bounded payloads, explicit errors, and bounded request timeouts. Define
  snapshot chunking/reassembly within the actual mesh payload limits.
- Make repeated delivery safe: deduplicate requests, especially shuffle/reset
  and session actions. A serial write is not confirmation that a change landed.
- Include an arbiter boot/session identity and configuration revision. Reject
  stale edits rather than silently overwriting a simultaneous touchscreen or
  browser change. Refresh the snapshot after conflict or reconnect.
- Distinguish accepted/applied configuration from durable storage and from
  device convergence. Report persistence failure and incomplete/offline device
  confirmation honestly; expose only confirmations the firmware can observe.
- Preserve the saved daily palette separately from the active, temporarily
  game-shuffled palette. An operator colour/RGB edit restores daily colours
  first, matching the existing arbiter behavior.
- Keep events, cues, and firmware traffic working on the shared serial link.
  Bound configuration work and return busy/unavailable during firmware work
  when the transport cannot serve both safely.

## Progress

Software implementation is in place: the Installation page covers mapping,
Colours/RGB, and Clock through a shared arbiter controller and bridge protocol.
All 46 automated API/protocol/controller and firmware regression tests pass.
Fixture-backed
browser checks cover the main flows and desktop/390px layouts. Firmware builds
and final review are recorded in `overlord/INSTALLATION_API.md`.

Real bridge/radio, touchscreen, NVS reboot/failure, and RTC verification remain
required before field use. No devices have been flashed for this change.

## Implementation steps

### 1. Define protocol and extract shared arbiter controllers

- [x] Specify request/response schemas, operation names, size limits, errors,
      retry/deduplication rules, arbiter selection, and revision semantics.
- [x] Define snapshots containing sensor assignments, reported channels and
      freshness, channel colours/rope counts, six RGB values, saved versus
      active palette state, clock/source/presence, and mapping session status.
- [x] Extract mapping state/capture from `installui.cpp` so touchscreen and
      remote sessions use the same controller and event suppression path.
- [x] Define exclusive mapping-session ownership across touchscreen and
      browsers. Add a renewable lease and expiry; disconnect, timeout, reboot,
      and explicit Done must leave mapping safely closed.
- [x] Add structured arbiter handlers using existing setters, with readback,
      persistence results, and explicit validation errors.

### 2. Implement transport and Overlord installation API

- [x] Relay configuration requests and responses through the bridge using
      dedicated serial records and mesh message handling.
- [x] Add an Overlord installation manager with bounded pending requests,
      snapshot reassembly, freshness tracking, reconnect handling, and polling.
- [x] Add installation snapshot and mutation endpoints under
      `/api/installation`; document the final route and body schemas.
- [x] Return meaningful unavailable, busy, conflict, timeout, and validation
      results. Do not present locally queued changes as confirmed success.
- [x] Ensure browser polling observes touchscreen edits and arbiter reboots.

### 3. Add navigation, Colours, and RGB

- [x] Add Installation/Firmware navigation while retaining existing firmware
      routes and workflows.
- [x] Show arbiter connection, identity, and snapshot age; disable edits until
      current authoritative state is available.
- [x] Add channels 1–24 with rope counts, colour names, swatches, and selectors.
- [x] Add game1 shuffle, six RGB editors, and reset-all-to-default action.
- [x] Debounce RGB edits and show pending/applied/error states. Avoid replacing
      a user's unfinished input during snapshot refreshes.
- [x] Surface temporary game palette changes and saved daily values clearly.

### 4. Add Map Ropes

- [x] Implement Start, countdown, captured rope list, channel assignment,
      Again, and Done using the arbiter-owned session.
- [x] Show each rope's reported and assigned channel and convergence progress.
- [x] Match the 5-second/10-rope capture behavior, including starting the next
      group when a new rope is pulled after an assignment.
- [x] Renew the session lease while the browser is active. Enforce expiry on the
      arbiter even if the browser disappears without sending Done.
- [x] Show session ownership and prevent concurrent mapping operators from
      silently taking over. Ensure expiry restores normal game event handling.

### 5. Add Clock

- [x] Show current arbiter time, clock source, and RTC availability.
- [x] Add manual date/time entry with Save/Cancel and valid-date checks.
- [x] Add Set from Overlord with an explicit local timezone and consistent
      local-wall-time conversion; do not silently use the browser timezone.
- [x] Read back the clock after setting and report missing RTC or write failure.
      Explain power-loss behavior where relevant.

### 6. Validate and document

- [ ] Test protocol validation, payload limits/chunking, missing/duplicate/out-of-
      order messages, deduplication, stale revisions, and reboot/reconnect.
- [ ] Test simultaneous touchscreen/browser edits and mapping ownership,
      capture limits, lease expiry, event suppression, and channel readback.
- [ ] Test colour/RGB persistence, reset, game1 shuffle invariants, and temporary
      game shuffle versus saved daily palette behavior.
- [ ] Test API/UI disconnected, pending, confirmed, conflict, and failed states;
      keep existing firmware and control API regression coverage passing.
- [x] Build affected arbiter/bridge firmware and verify partition fit; also
      build sensor/screen callers of the shared palette storage helper.
- [x] Check browser layouts on desktop and a phone-sized viewport.
- [ ] Verify on hardware through the real bridge: rope capture/assignment,
      device convergence, touchscreen synchronization, persistence after
      reboot, clock readback, and continued cue/event/firmware operation.
- [x] Document operation, transport/API schemas, persistence, mapping lease,
      timeout behavior, and remaining hardware limitations.

## Completion criteria

All touchscreen configuration areas are operable from Overlord with authoritative
readback. Touchscreen changes appear in the browser. Operator settings survive
reboot according to the arbiter's existing persistence semantics. No abandoned
remote mapping session can suppress game events indefinitely. Transport loss or
an offline device never appears as confirmed success. Existing firmware control,
media cues, and mesh event handling continue to work.

Implementation order: protocol/shared controllers → transport/snapshot/API →
Colours/RGB → Map Ropes → Clock → final hardware validation and documentation.

## Latest validation

- `python3 -m unittest discover -s test`: 46 tests passed (including a native
  C++ capture/lease/calendar test and HTTP/protocol regression tests).
- `pio run -e arbiter_mesh -e bridge -e bridge_esp32 -e sensor -e screen`:
  all five environments built successfully and fit their app partitions.
- `node --check overlord/web/installation.js` and `git diff --check`: passed.
- Browser fixture checks: mapping start/assignment/Done, RGB apply, clock
  readback, stale-edit rejection with unfinished input preserved, and desktop
  plus 390px layouts; no browser JavaScript errors observed.
- Hardware validation in phase 6 remains open. Firmware was built, not flashed.

## Follow-up: game goals and status overview

- [x] Include authoritative game/state/timer, visible goal colours, device
  summary, mesh counts/activity/skew, and uptime in installation snapshots.
- [x] Show named goal chips on the arbiter LCD, using the same visible-goal
  colour conversion as Overlord's snapshot.
- [x] Add the game/mesh overview to the top of Installation, with prominent
  goals and explicit stale, disconnected, firmware-paused, and unavailable states.
- [x] Test reordered major colours, hidden goals, chunked status readback, and
  browser timer/freshness rules. All 48 host regression tests pass.
- [x] Check desktop and 390px overview layouts with simulated game/mesh
  data; no page overflow or browser JavaScript errors observed.
- [x] Build updated arbiter firmware; image fits at 21.4% of its app partition.
- [ ] Verify updated LCD layout and live state/goal changes on real hardware.
