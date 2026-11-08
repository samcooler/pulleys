"""Overlord control API and firmware dashboard on the same HTTP server."""
import json
from pathlib import Path
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlsplit

from installation import InstallationError

WEB = Path(__file__).with_name("web")
MAX_IMAGE = 7 * 1024 * 1024


class ControlServer:
    def __init__(self, handle, host="127.0.0.1", port=8080, firmware=None, installation=None):
        class Handler(BaseHTTPRequestHandler):
            def _body(self, limit, binary=False):
                if self.headers.get("Transfer-Encoding"):
                    raise ValueError("use Content-Length; chunked uploads are not supported")
                try:
                    size = int(self.headers.get("Content-Length", "0"))
                except ValueError:
                    raise ValueError("invalid Content-Length")
                if not 0 <= size <= limit:
                    raise ValueError(f"request exceeds {limit} bytes")
                self.connection.settimeout(15)
                data = self.rfile.read(size)
                if len(data) != size:
                    raise ValueError("incomplete request body")
                if binary:
                    return data
                value = json.loads(data or b"{}")
                if not isinstance(value, dict):
                    raise ValueError("JSON body must be an object")
                return value

            def _firmware(self, parts):
                if firmware is None:
                    return 503, {"error": "firmware manager unavailable"}
                route = parts[2:]
                if self.command == "GET":
                    snapshot = firmware.snapshot()
                    if not route:
                        return 200, snapshot
                    if len(route) == 2 and route[0] in {"jobs", "releases"}:
                        row = next((row for row in snapshot[route[0]] if row["id"] == route[1]), None)
                        return (200, {route[0][:-1]: row}) if row else (404, {"error": "not found"})
                if self.command == "POST":
                    body = self._body(256 * 1024)
                    if route in (["check"], ["scan"]):
                        return 202, {"job": firmware.enqueue(route[0])}
                    if route == ["releases"]:
                        return 201, {"release": firmware.register_manifest(body)}
                    if route == ["updates"]:
                        if not (body.get("device_ids") or body.get("env") or body.get("all") is True):
                            raise ValueError("select device_ids, env, or all: true")
                        return 202, {"job": firmware.enqueue("update", body.get("release_id"),
                                                            body.get("device_ids"), body.get("env"))}
                    if len(route) == 3 and route[0] == "jobs" and route[2] == "retry":
                        return 202, {"job": firmware.retry(route[1])}
                if self.command == "PUT" and len(route) == 3 and route[0] == "releases":
                    data = self._body(MAX_IMAGE, binary=True)
                    return 200, {"release": firmware.upload_image(route[1], route[2], data)}
                return 404, {"error": "no such firmware route"}

            def _serve(self):
                parts = [unquote(p) for p in urlsplit(self.path).path.split("/") if p]
                content_type = "application/json; charset=utf-8"
                try:
                    assets = {("installation",): ("installation.html", "text/html"),
                              ("installation.js",): ("installation.js", "application/javascript"),
                              ("installation.css",): ("installation.css", "text/css"),
                              (): ("index.html", "text/html"),
                              ("dashboard.css",): ("dashboard.css", "text/css"),
                              ("dashboard.js",): ("dashboard.js", "application/javascript")}
                    if self.command == "GET" and tuple(parts) in assets:
                        name, content_type = assets[tuple(parts)]
                        status, body = 200, (WEB / name).read_bytes()
                    elif parts[:2] == ["api", "installation"]:
                        if installation is None:
                            status, body = 503, {"error": "installation manager unavailable"}
                        elif self.command == "GET" and len(parts) == 2:
                            status, body = 200, installation.snapshot()
                        elif self.command == "POST" and len(parts) == 3:
                            status, body = 200, installation.mutate(parts[2], self._body(2048))
                        else:
                            status, body = 404, {"error": "no such installation route"}
                    elif parts[:2] == ["api", "firmware"]:
                        status, body = self._firmware(parts)
                    else:
                        status, body = handle(self.command, parts)
                except InstallationError as exc:
                    status, body = exc.status, {"error": str(exc)}
                except (ValueError, UnicodeError) as exc:
                    status, body = 400, {"error": str(exc)}
                except TimeoutError:
                    status, body = 408, {"error": "request body timed out"}
                except Exception as exc:
                    status, body = 500, {"error": str(exc)}
                data = body if isinstance(body, bytes) else (json.dumps(body) + "\n").encode()
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.end_headers()
                try:
                    self.wfile.write(data)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            do_GET = do_POST = do_PUT = _serve

            def log_message(self, *args):
                pass

        self._httpd = ThreadingHTTPServer((host, port), Handler)
        self._httpd.daemon_threads = True
        self.address = self._httpd.server_address

    def start(self):
        threading.Thread(target=self._httpd.serve_forever, daemon=True).start()

    def stop(self):
        self._httpd.shutdown()
        self._httpd.server_close()
