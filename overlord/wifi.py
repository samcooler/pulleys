"""Wi-Fi control through NetworkManager's nmcli (Pi OS Bookworm, Debian)."""

import shutil
import subprocess
import urllib.request

# Returns 204 with an empty body when the internet is reachable.
CHECK_URL = "http://connectivitycheck.gstatic.com/generate_204"


class WifiError(Exception):
    pass


def _nmcli(*args, timeout=60):
    if not shutil.which("nmcli"):
        raise WifiError("nmcli not found (NetworkManager is not installed)")
    try:
        r = subprocess.run(["nmcli", *args], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise WifiError("nmcli timed out") from None
    if r.returncode != 0:
        raise WifiError((r.stderr or r.stdout).strip() or f"nmcli failed ({r.returncode})")
    return r.stdout


def split_terse(line):
    """Split one line of `nmcli -t` output on unescaped colons."""
    fields, cur, i = [], "", 0
    while i < len(line):
        c = line[i]
        if c == "\\" and i + 1 < len(line):
            cur += line[i + 1]
            i += 2
        elif c == ":":
            fields.append(cur)
            cur = ""
            i += 1
        else:
            cur += c
            i += 1
    fields.append(cur)
    return fields


def scan():
    """Visible networks, strongest first: [{ssid, signal, secured, active}]."""
    out = _nmcli("-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "dev", "wifi", "list", "--rescan", "yes")
    best = {}
    for line in out.splitlines():
        f = split_terse(line)
        if len(f) < 4 or not f[1]:
            continue  # hidden networks have an empty SSID
        net = {"ssid": f[1], "signal": int(f[2] or 0), "secured": f[3] not in ("", "--"),
               "active": f[0] == "*"}
        old = best.get(net["ssid"])
        if old is None or net["active"] or (not old["active"] and net["signal"] > old["signal"]):
            best[net["ssid"]] = net
    return sorted(best.values(), key=lambda n: (not n["active"], -n["signal"]))


def connect(ssid, password=None, hidden=False):
    """Join a network; NetworkManager remembers it and reconnects on boot."""
    args = ["--wait", "30", "dev", "wifi", "connect", ssid]
    if password:
        args += ["password", password]
    if hidden:
        args += ["hidden", "yes"]
    _nmcli(*args, timeout=45)


def status():
    """Short lines describing the current connection, for the menu."""
    lines = []
    for line in _nmcli("-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "dev").splitlines():
        dev, kind, state, conn = (split_terse(line) + [""] * 4)[:4]
        if kind not in ("wifi", "ethernet"):
            continue
        lines.append(f"{dev} ({kind}): {state}" + (f" - {conn}" if conn else ""))
        if state == "connected":
            for l in _nmcli("-t", "-f", "IP4.ADDRESS", "dev", "show", dev).splitlines():
                lines.append("    " + l.split(":", 1)[-1])
    return lines or ["no network devices"]


def _devices():
    """[(device, type, state, connection)] for wifi and ethernet."""
    rows = []
    for line in _nmcli("-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "dev").splitlines():
        row = (split_terse(line) + [""] * 4)[:4]
        if row[1] in ("wifi", "ethernet"):
            rows.append(tuple(row))
    return rows


def link():
    """{wifi: SSID or None, ip: first IPv4 address or None} for the menu header.
    Wi-Fi is preferred for the address; a wired link is the fallback."""
    wifi_ssid, ip = None, None
    devs = _devices()
    for dev, kind, state, conn in sorted(devs, key=lambda d: d[1] != "wifi"):
        if state != "connected":
            continue
        if kind == "wifi":
            wifi_ssid = conn
        if ip is None:
            for l in _nmcli("-t", "-f", "IP4.ADDRESS", "dev", "show", dev).splitlines():
                if l.split(":", 1)[-1].strip():
                    ip = l.split(":", 1)[-1].split("/")[0]
                    break
    return {"wifi": wifi_ssid, "ip": ip}


def internet(timeout=3.0):
    """True if a web endpoint answers. HTTP rather than ICMP: no privileges needed."""
    try:
        with urllib.request.urlopen(CHECK_URL, timeout=timeout) as r:
            return r.status in (200, 204)
    except Exception:
        return False
