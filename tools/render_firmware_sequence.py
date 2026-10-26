#!/usr/bin/env python3
"""Render the firmware update protocol as a three-party sequence diagram."""

from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "docs" / "diagrams" / "firmware-update-sequence.png"

ACTORS = {"Overlord": 1.4, "Bridge": 6.7, "Node": 12.0}
COLORS = {
    "serial": "#2563eb",
    "mesh": "#7c3aed",
    "http": "#15803d",
    "local": "#64748b",
}


def main():
    messages = [
        ("Overlord", "Bridge", "FW-PING", "serial"),
        ("Bridge", "Overlord", "FW-BRIDGE: ID + running build hash", "serial"),
        ("Overlord", "Bridge", "FW-CENSUS", "serial"),
        ("Bridge", "Node", "ESP-NOW HELLO", "mesh"),
        ("Node", "Bridge", "ESP-NOW CENSUS: env + build hash", "mesh"),
        ("Bridge", "Overlord", "PULLEYS-CENSUS over UART", "serial"),
        ("Overlord", "Bridge", "FW-OFFER: target + asset SHA-256 + size", "serial"),
        ("Bridge", "Node", "ESP-NOW FW_OFFER", "mesh"),
        ("Node", "Bridge", "HTTP GET /api/v1/firmware/{sha256}", "http"),
        ("Bridge", "Overlord", "FW-READ: asset SHA-256 + offset + length", "serial"),
        ("Overlord", "Bridge", "FW-DATA: Base64 image chunk", "serial"),
        ("Bridge", "Node", "HTTP response: firmware bytes", "http"),
        ("Node", "Node", "Verify size + SHA-256; install; reboot", "local"),
        ("Node", "Bridge", "ESP-NOW FW_STATUS: install result", "mesh"),
        ("Bridge", "Overlord", "FW-STATUS over UART", "serial"),
        ("Overlord", "Bridge", "FW-QUERY after reboot", "serial"),
        ("Bridge", "Node", "ESP-NOW census request", "mesh"),
        ("Node", "Bridge", "Fresh census: running build hash", "mesh"),
        ("Bridge", "Overlord", "Fresh PULLEYS-CENSUS over UART", "serial"),
        ("Overlord", "Bridge", "FW-BRIDGE-UPDATE (bridge is last)", "serial"),
        ("Bridge", "Overlord", "FW-READ for bridge image chunks", "serial"),
        ("Overlord", "Bridge", "FW-DATA chunks", "serial"),
        ("Bridge", "Bridge", "Verify + install inactive slot; reboot", "local"),
        ("Overlord", "Bridge", "FW-PING after bridge reconnects", "serial"),
        ("Bridge", "Overlord", "Bridge ID + fresh running build hash", "serial"),
    ]

    fig, ax = plt.subplots(figsize=(14, 17), layout="constrained")
    fig.patch.set_facecolor("white")
    ax.set_facecolor("white")
    ax.set_xlim(0, 16.5)
    ax.set_ylim(-1.2, len(messages) + 4)
    ax.invert_yaxis()
    ax.axis("off")
    ax.set_title("Firmware update sequence", loc="left", fontsize=20, weight="bold", pad=18)

    # Startup and scheduler notes sit above the participant lifelines.
    note = FancyBboxPatch((0.45, 0.15), 15.7, 1.55,
                          boxstyle="round,pad=0.12,rounding_size=0.08",
                          linewidth=0.8, edgecolor="#cbd5e1", facecolor="#f8fafc")
    ax.add_patch(note)
    ax.text(0.7, 0.52,
            "Startup: fetch latest GitHub release (3 attempts), validate assets, then discover bridge (25 s).",
            fontsize=10, va="center", color="#0f172a")
    ax.text(0.7, 0.92,
            "Scheduler: arbiter_mesh first, other reachable nodes arbitrary, bridge last; one target at a time.",
            fontsize=10, va="center", color="#0f172a")
    ax.text(0.7, 1.32,
            "Retry each target up to 3 times; success needs install status and a fresh matching hash after reboot.",
            fontsize=10, va="center", color="#0f172a")

    actor_y = 2.05
    for name, x in ACTORS.items():
        ax.text(x, actor_y, name, ha="center", va="center", fontsize=12, weight="bold",
                bbox={"boxstyle": "round,pad=0.45", "fc": "#e2e8f0", "ec": "#94a3b8"})
        ax.plot([x, x], [actor_y + 0.42, len(messages) + 1.1], color="#94a3b8",
                linewidth=1, linestyle=(0, (3, 4)), zorder=0)

    for idx, (source, target, label, kind) in enumerate(messages, start=1):
        y = idx + 1.85
        color = COLORS[kind]
        if source == target:
            x = ACTORS[source]
            ax.annotate("", xy=(x + 0.48, y + 0.24), xytext=(x + 0.48, y - 0.24),
                        arrowprops={"arrowstyle": "->", "color": color, "lw": 1.7})
            ax.text(x + 0.68, y, label, fontsize=9.5, ha="left", va="center", color="#0f172a")
        else:
            x1, x2 = ACTORS[source], ACTORS[target]
            ax.annotate("", xy=(x2, y), xytext=(x1, y),
                        arrowprops={"arrowstyle": "->", "color": color, "lw": 1.7,
                                    "shrinkA": 2, "shrinkB": 2})
            mid = (x1 + x2) / 2
            ax.text(mid, y - 0.12, label, fontsize=9.2, ha="center", va="bottom",
                    color="#0f172a", bbox={"fc": "white", "ec": "none", "pad": 1.2})

    legend = [
        ("serial", "Serial"), ("mesh", "ESP-NOW"), ("http", "HTTP"), ("local", "Node/bridge action")
    ]
    for i, (key, text) in enumerate(legend):
        x = 0.65 + i * 2.25
        ax.plot([x, x + 0.38], [len(messages) + 2.75] * 2, color=COLORS[key], lw=2)
        ax.text(x + 0.48, len(messages) + 2.75, text, va="center", fontsize=9)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT, dpi=180, bbox_inches="tight")
    print(OUT)


if __name__ == "__main__":
    main()
