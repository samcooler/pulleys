"""verdict_ota env: read the OTA host + password from src/verdict/secrets.h (gitignored)."""

Import("env")
import os
import re

path = os.path.join(env["PROJECT_DIR"], "src", "verdict", "secrets.h")
if not os.path.exists(path):
    raise RuntimeError("missing src/verdict/secrets.h (copy secrets.h.example)")
text = open(path).read()


def define(name):
    m = re.search(r'#define\s+%s\s+"([^"]*)"' % name, text)
    return m.group(1) if m else None


if not env.subst("$UPLOAD_PORT") and define("OTA_HOST"):
    env.Replace(UPLOAD_PORT=define("OTA_HOST"))
if define("OTA_PASS"):
    env.Append(UPLOAD_FLAGS=["--auth=" + define("OTA_PASS")])
