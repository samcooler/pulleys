# Pulleys: notes for Claude

## Restarting the Overlord

Use `tools/overlord_restart.sh`. Do not type `ssh` by hand.

```sh
tools/overlord_restart.sh            # git pull, restart the app, wait for it, report
tools/overlord_restart.sh --update   # also rerun the boot update (git pull + media sync), as a reboot does
tools/overlord_restart.sh --status   # change nothing, just report
```

It prints the commit the Overlord is on, whether the app is up, whether the bridge
is connected, and any running firmware job.

- The login (`overlord` / `strings`, from `overlord/README.md`) is stored in the
  script. It is not sensitive, and the user asked for it to live there. Use the
  script, and change the password in that one place if it ever changes.
- The app reads its code and its serial port only at startup. So deploying is
  push to `main`, then run the script; and a bridge plugged in after the app
  started is not seen until a restart.
- The script tries `overlord.local`, then falls back to the hotspot IP
  (`OVERLORD_IP`, default `172.20.10.2`), checking the host key against the
  `overlord.local` entry either way. It needs `sshpass` (`brew install sshpass`).
- `--update` can take minutes when media is syncing; `OVERLORD_WAIT` (seconds)
  sets how long the script waits for the app to answer.
- Restarting interrupts any running firmware job. Check `--status` first.

## Reaching the Overlord from Python on macOS

macOS's Local Network privacy blocks non-Apple binaries from the local subnet and
reports it as `[Errno 65] No route to host`. `curl` and `/usr/bin/python3` are
exempt. If `./fw` fails that way, run it as `/usr/bin/python3 fw ...` (and
`OVERLORD_URL=http://172.20.10.2:8080` if `overlord.local` does not resolve), or
allow the terminal under System Settings > Privacy & Security > Local Network.

## Firmware reaches the Overlord by push, not by download

The Overlord never fetches firmware from GitHub and does nothing about firmware
at startup. `./fw flash` builds, uploads to the Overlord, queues the update and
watches it (see `DEVELOPING.md`). `./fw watch` reattaches to a running job.

`./fw flash` with no selection builds only the firmware the git changes touch, and
prints the patterns it matched (`tools/firmware_changes.py` is the table). It
flashes nothing if only docs, tests or `overlord/` changed. Use `--all` for every
known node, `--since REF` after committing, or `--id/--type/--env` to choose.
When adding a `src/` or `lib/` directory, add it to that table (a test enforces it).
