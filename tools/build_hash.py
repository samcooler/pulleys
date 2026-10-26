"""Inject the release build identity into active PlatformIO firmware builds."""

Import("env")
import os
import re

value = os.environ.get("PULLEYS_FIRMWARE_BUILD_HASH", "unknown").lower()
if not re.fullmatch(r"[0-9a-f]{64}|unknown", value):
    raise RuntimeError("PULLEYS_FIRMWARE_BUILD_HASH must be a 64-character SHA-256")
env.Append(CPPDEFINES=[("PULLEYS_FIRMWARE_BUILD_HASH", '\\"' + value + '\\"')])
